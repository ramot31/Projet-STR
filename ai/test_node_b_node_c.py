"""Optional end-to-end check for the Node B -> Node C TCP exchange.

Run from this directory with:
    python test_node_b_node_c.py

Requires the trained model and a compiled Node C executable in ../bin/.
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path


AI_DIR = Path(__file__).resolve().parent
PROJECT_DIR = AI_DIR.parent
NODE_C_PORT = 5003
NODE_B_PORT = 5002
FLIGHT_ID = "TU701"


def find_node_c(requested_path):
    if requested_path:
        executable = Path(requested_path).expanduser().resolve()
        if executable.is_file():
            return executable
        raise FileNotFoundError(f"Exécutable node_c introuvable : {executable}")

    candidates = (
        PROJECT_DIR / "bin" / "node_c",
        PROJECT_DIR / "bin" / "node_c.exe",
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate

    raise FileNotFoundError(
        "Exécutable node_c introuvable dans bin/. "
        "Compile-le depuis la racine du projet (cible Makefile : bin/node_c), "
        "ou indique son chemin avec --node-c."
    )


def require_free_port(port):
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.bind(("127.0.0.1", port))
    except OSError as exc:
        raise RuntimeError(
            f"Le port {port} est déjà utilisé. Arrête le service qui l'occupe, puis relance le test."
        ) from exc


def wait_for_port(port, process, service_name, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        return_code = process.poll()
        if return_code is not None:
            output, _ = process.communicate()
            raise RuntimeError(
                f"{service_name} s'est arrêté avant d'écouter sur le port {port} "
                f"(code {return_code}).\n{output}"
            )
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise TimeoutError(f"{service_name} n'écoute pas sur le port {port} après {timeout:.1f} s.")


def send_flight_delay(port, source_timestamp):
    message = (
        f"1;1;NODE_A;{source_timestamp};"
        f"flight={FLIGHT_ID},delay=10,src_ts={source_timestamp}\n"
    )
    with socket.create_connection(("127.0.0.1", port), timeout=2.0) as conn:
        conn.sendall(message.encode("utf-8"))


def stop_process(process):
    if process is None:
        return ""
    if process.poll() is None:
        process.terminate()
    try:
        output, _ = process.communicate(timeout=3.0)
    except subprocess.TimeoutExpired:
        process.kill()
        output, _ = process.communicate()
    return output


def main():
    parser = argparse.ArgumentParser(
        description="Teste l'envoi d'un RISK_RESULT de node_b.py vers node_c."
    )
    parser.add_argument(
        "--node-c",
        help="Chemin de l'exécutable node_c (par défaut : ../bin/node_c[.exe]).",
    )
    parser.add_argument(
        "--b-port",
        type=int,
        default=NODE_B_PORT,
        help=f"Port d'écoute temporaire de Node B (défaut : {NODE_B_PORT}).",
    )
    parser.add_argument(
        "--startup-timeout",
        type=float,
        default=20.0,
        help="Délai maximal de démarrage de chaque nœud, en secondes.",
    )
    parser.add_argument(
        "--result-timeout",
        type=float,
        default=12.0,
        help="Délai maximal d'attente de l'arrêt de Node C, en secondes.",
    )
    args = parser.parse_args()

    node_c_executable = find_node_c(args.node_c)
    model_path = AI_DIR / "model" / "risk_model.joblib"
    if not model_path.is_file():
        raise FileNotFoundError(f"Modèle exporté introuvable : {model_path}")
    if args.b_port == NODE_C_PORT:
        raise ValueError("Le port de Node B doit être différent du port fixe de Node C (5003).")

    require_free_port(args.b_port)
    require_free_port(NODE_C_PORT)

    env = os.environ.copy()
    env["AIRPORT_HOST"] = "127.0.0.1"
    node_b = None
    node_c = None
    try:
        node_b = subprocess.Popen(
            [
                sys.executable,
                "-u",
                str(AI_DIR / "node_b.py"),
                "--port",
                str(args.b_port),
                "--c-port",
                str(NODE_C_PORT),
            ],
            cwd=AI_DIR,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        wait_for_port(args.b_port, node_b, "Node B", args.startup_timeout)

        node_c = subprocess.Popen(
            [
                str(node_c_executable),
                "--policy",
                "fixed",
                "--timeout-ms",
                "5000",
                "--idle-exit",
                "2",
            ],
            cwd=PROJECT_DIR,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        wait_for_port(NODE_C_PORT, node_c, "Node C", args.startup_timeout)

        source_timestamp = time.time_ns()
        send_flight_delay(NODE_C_PORT, source_timestamp)
        send_flight_delay(args.b_port, source_timestamp)

        try:
            node_c_output, _ = node_c.communicate(timeout=args.result_timeout)
        except subprocess.TimeoutExpired as exc:
            raise TimeoutError(
                f"Node C n'a pas terminé dans les {args.result_timeout:.1f} s imparties."
            ) from exc

        risk_match = re.search(
            rf"RECU RISK_RESULT {re.escape(FLIGHT_ID)} risque=([0-9]+(?:\.[0-9]+)?)",
            node_c_output,
        )
        if not risk_match:
            raise RuntimeError(
                "Node C n'a pas journalisé le RISK_RESULT envoyé par Node B.\n"
                f"Sortie de Node C :\n{node_c_output}"
            )

        risk = float(risk_match.group(1))
        if not 0.0 <= risk <= 1.0:
            raise RuntimeError(f"Probabilité reçue invalide pour {FLIGHT_ID} : {risk}")

        print(f"OK : Node C a reçu le RISK_RESULT de Node B pour {FLIGHT_ID} (risque={risk:.2f}).")
        print(node_c_output.rstrip())
        return 0
    finally:
        stop_process(node_b)
        stop_process(node_c)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, RuntimeError, TimeoutError, ValueError, OSError) as exc:
        print(f"ÉCHEC DU TEST : {exc}", file=sys.stderr)
        raise SystemExit(1)
