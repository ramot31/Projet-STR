"""
node_b.py - Nœud B (IA) du système AI Smart Airport (M2).

Écoute en TCP sur le port 5002 (PORT_NODE_B). Protocole de M1 (protocol.h) :
une ligne texte terminée par '\\n' :   id;type;origin;timestamp_ns;payload
avec payload = cle=valeur,cle=valeur.

Flux :
    A  --FLIGHT_DELAY (type 1)-->  B     ex. flight=TU701,delay=20,src_ts=...
    P  --QUEUE_UPDATE (type 3)-->  B     met à jour le contexte au sol (cache)
    B  --RISK_RESULT  (type 6)-->  C     flight=TU701,risk=0.83,src_ts=...

B ne répond jamais avec un risque inventé : en cas d'erreur (champ obligatoire absent,
modèle illisible), il n'envoie rien et le nœud C passe en mode dégradé après son délai.
Le champ de protocole delay est converti en delay_min ; les autres variables doivent
utiliser les noms du modèle.

Usage :  python node_b.py [--port 5002] [--c-port 5003]
Variable d'environnement : AIRPORT_HOST (défaut 127.0.0.1), comme dans net.c.
"""
import argparse
import itertools
import logging
import os
import socket
import socketserver
import threading
import time

from predict import load_model, predict

PORT_NODE_B = 5002
PORT_NODE_C = 5003
EV_FLIGHT_DELAY, EV_QUEUE_UPDATE, EV_RISK_RESULT = 1, 3, 6

logger = logging.getLogger("airport.nodeB")

# Données de vol connues du scénario. Marge = next - sched (tableau BASE de node_c.c).
# aircraft_type et gate_distance_m : TU701 vient de l'énoncé (450 m) ; les autres vols sont
# des VALEURS PROVISOIRES à confirmer avec M1 et M6.
FLIGHTS = {
    "TU701": dict(sched=620, next=675, connecting_pax=35, aircraft_type="medium", gate_distance_m=450),
    "TU702": dict(sched=630, next=690, connecting_pax=12, aircraft_type="small", gate_distance_m=150),
    "TU703": dict(sched=625, next=680, connecting_pax=20, aircraft_type="medium", gate_distance_m=300),
    "TU704": dict(sched=635, next=685, connecting_pax=8, aircraft_type="large", gate_distance_m=400),
}

# Contexte au sol mémorisé (dernières valeurs reçues). Vide au départ : un champ jamais reçu
# est remplacé par la valeur par défaut du modèle, avec un avertissement.
CONTEXT = {}
CONTEXT_LOCK = threading.Lock()
CONTEXT_KEYS = {"security_wait_min", "buses_available", "weather", "peak_hour"}
CONTEXT_PROTOCOL_KEYS = {"queue", "factor", "slowdown", "flight", "src_ts"}

_ids = itertools.count(1)
_C_PORT = PORT_NODE_C


def parse_line(line):
    """'id;type;origin;ts;payload' -> (type, origin, ts, dict payload)."""
    parts = line.rstrip("\n").split(";", 4)
    if len(parts) != 5:
        raise ValueError(f"Ligne invalide : {line!r}")
    _, msg_type, origin, ts, payload = parts
    fields = dict(kv.split("=", 1) for kv in payload.split(",") if "=" in kv)
    return int(msg_type), origin, int(ts), fields


def send(port, msg_type, payload):
    """Envoie un message (une connexion TCP par message, 3 essais, comme net_send en C)."""
    line = f"{next(_ids)};{msg_type};NODE_B;{time.time_ns()};{payload}\n"
    host = os.environ.get("AIRPORT_HOST", "127.0.0.1")
    for _ in range(3):
        try:
            with socket.create_connection((host, port), timeout=1.0) as conn:
                conn.sendall(line.encode())
            return True
        except OSError:
            time.sleep(0.02)
    return False


def update_context(fields):
    """Mémorise le contexte au sol reçu (file de sécurité, bus, météo, heure de pointe)."""
    unknown = set(fields) - CONTEXT_KEYS - CONTEXT_PROTOCOL_KEYS
    if unknown:
        raise ValueError(f"Champs de contexte inconnus : {', '.join(sorted(unknown))}")
    updates = {k: v for k, v in fields.items() if k in CONTEXT_KEYS}
    if "queue" in fields and "security_wait_min" not in updates:
        # attente = personnes x 0,1 min x facteur de ralentissement (documents de M1)
        factor = float(fields.get("factor", fields.get("slowdown", 1.0)))
        updates["security_wait_min"] = float(fields["queue"]) * 0.1 * factor
    if updates:
        with CONTEXT_LOCK:
            CONTEXT.update(updates)
        logger.info("Contexte mis à jour : %s", updates)


def on_flight_delay(fields):
    flight = fields.get("flight")
    if not flight:
        raise ValueError("FLIGHT_DELAY sans clé flight")
    features = {}
    info = FLIGHTS.get(flight, {})
    features.update({k: v for k, v in info.items() if k not in ("sched", "next")})
    if "sched" in info:
        features["scheduled_buffer_min"] = info["next"] - info["sched"]
    with CONTEXT_LOCK:
        features.update(CONTEXT)
    received = {k: v for k, v in fields.items() if k not in ("flight", "src_ts")}
    if "delay" in received:
        if "delay_min" in received:
            raise ValueError("FLIGHT_DELAY contient à la fois delay et delay_min")
        received["delay_min"] = received.pop("delay")
    # les valeurs reçues dans le message l'emportent sur le reste
    features.update(received)

    start = time.perf_counter()
    risk = predict(features)["risk"]
    ms = (time.perf_counter() - start) * 1000

    payload = f"flight={flight},risk={risk:.2f}"
    if "src_ts" in fields:
        payload += f",src_ts={fields['src_ts']}"
    ok = send(_C_PORT, EV_RISK_RESULT, payload)
    logger.info("RISK_RESULT %s risque=%.2f (inférence %.1f ms)%s",
                flight, risk, ms, "" if ok else " [ECHEC ENVOI]")


def handle_line(line):
    try:
        msg_type, origin, _, fields = parse_line(line)
        if msg_type == EV_FLIGHT_DELAY:
            logger.info("RECU FLIGHT_DELAY de %s : %s", origin, fields)
            on_flight_delay(fields)
        elif msg_type == EV_QUEUE_UPDATE:
            update_context(fields)
        else:
            logger.debug("Message de type %s ignoré", msg_type)
    except Exception as exc:                      # pas de réponse -> C bascule en repli
        logger.error("Message non traité (%s) : %r", exc, line.strip())


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        for raw in self.rfile:                    # une ligne par message, jusqu'à la fermeture
            handle_line(raw.decode("utf-8", errors="replace"))


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    global _C_PORT
    parser = argparse.ArgumentParser(description="Nœud B (IA)")
    parser.add_argument("--port", type=int, default=PORT_NODE_B)
    parser.add_argument("--c-port", type=int, default=PORT_NODE_C)
    args = parser.parse_args()
    _C_PORT = args.c_port
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(message)s")

    load_model()                                  # échoue tout de suite si le modèle est absent
    try:                                          # échauffement : le 1er appel est plus lent
        predict({"delay_min": 10, "scheduled_buffer_min": 60})
    except Exception as exc:
        logger.warning("Échauffement impossible : %s", exc)
    with Server(("0.0.0.0", args.port), Handler) as server:
        logger.info("Nœud B prêt (port %d, résultats vers le port %d)", args.port, _C_PORT)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            logger.info("Arrêt du nœud B")


if __name__ == "__main__":
    main()