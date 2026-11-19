/* protocol.h - Format des messages inter-noeuds (fige par M1).
 * Ligne texte terminee par '\n' :
 *     id;type;origin;timestamp_ns;payload
 * payload = cle=valeur,cle=valeur   (ex: flight=TU701,delay=20)
 * Simple a produire/analyser en C, lisible dans les logs, utilisable avec TCP ou UDP.
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define MSG_MAX 256

/* Ports (localhost par defaut) */
#define PORT_NODE_A 5001   /* Vols + portes (M6) */
#define PORT_NODE_B 5002   /* IA (M2) */
#define PORT_NODE_C 5003   /* Decision / ordonnanceur (M1) */
#define PORT_NODE_P 5004   /* Passagers + files + bus (M4) */

typedef enum {
    EV_FLIGHT_DELAY = 1,   /* simulateur -> A, puis A -> B */
    EV_GATE_UNAVAILABLE,   /* simulateur -> A : alerte critique (D = 50 ms) */
    EV_QUEUE_UPDATE,       /* simulateur -> P, puis P -> B */
    EV_BUS_BREAKDOWN,      /* simulateur -> P : 5 bus puis 4 */
    EV_SECURITY_SLOWDOWN,  /* simulateur -> P : attente x2 */
    EV_RISK_RESULT,        /* B -> C */
    EV_RECHERCHE_PORTE,    /* C -> A */
    EV_GATE_ASSIGNED,      /* A -> C */
    EV_TRAITER_GROUPE,     /* C -> P */
    EV_ACK,                /* accuse de reception (P -> C, A -> C) */
    EV_HEARTBEAT           /* tous -> C : detection de panne */
} event_type_t;

typedef struct {
    uint64_t id;
    event_type_t type;
    char     origin[8];        /* "NODE_A" ... */
    uint64_t timestamp_ns;     /* CLOCK_REALTIME */
    char     payload[160];
} message_t;

static inline uint64_t now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline int msg_format(const message_t *m, char *buf, size_t n) {
    return snprintf(buf, n, "%llu;%d;%s;%llu;%s\n",
        (unsigned long long)m->id, (int)m->type, m->origin,
        (unsigned long long)m->timestamp_ns, m->payload);
}

static inline int msg_parse(const char *line, message_t *m) {
    unsigned long long id, ts; int type;
    char origin[8], payload[160];
    if (sscanf(line, "%llu;%d;%7[^;];%llu;%159[^\n]", &id, &type, origin, &ts, payload) != 5)
        return -1;
    m->id = id; m->type = (event_type_t)type; m->timestamp_ns = ts;
    strncpy(m->origin, origin, sizeof m->origin - 1); m->origin[sizeof m->origin - 1] = 0;
    strncpy(m->payload, payload, sizeof m->payload - 1); m->payload[sizeof m->payload - 1] = 0;
    return 0;
}
#endif
