/* stubs.c - Noeuds FACTICES pour tester C et le simulateur seuls : bin/stubs a | b | p
 * (remplaces par les vrais noeuds de M6 (A), M2 (B), M4 (P)). Ils montrent aussi le contrat attendu :
 *  - A transmet FLIGHT_DELAY a B ET en copie a C, en propageant src_ts (horodatage de l'evenement initial)
 *  - B repond RISK_RESULT a C avec flight, risk, src_ts
 *  - A repond GATE_ASSIGNED a C ; P repond ACK a C */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "net.h"
#include "types.h"

static gate_t gates[NB_GATES] = { { "G12", GATE_FREE, "", 400 }, { "G15", GATE_FREE, "", 450 }, { "G18", GATE_FREE, "", 500 } };
static pthread_mutex_t gmu = PTHREAD_MUTEX_INITIALIZER;
static int buses = NB_BUSES;

static void on_a(const message_t *m, void *arg) { (void)arg; char fl[16], g[8];
    printf("[A] recu %s %s\n", event_name(m->type), m->payload); fflush(stdout);
    if (m->type == EV_FLIGHT_DELAY) {
        message_t o; kv_get(m->payload, "flight", fl, sizeof fl);
        char pl[160]; snprintf(pl, sizeof pl, "%s,src_ts=%llu", m->payload, (unsigned long long)m->timestamp_ns);
        msg_init(&o, EV_FLIGHT_DELAY, "NODE_A"); strcpy(o.payload, pl); net_send(PORT_NODE_C, &o);  /* copie pour C */
        msg_init(&o, EV_FLIGHT_DELAY, "NODE_A"); strcpy(o.payload, pl); net_send(PORT_NODE_B, &o);  /* vers l'IA */
    } else if (m->type == EV_GATE_UNAVAILABLE) {
        kv_get(m->payload, "gate", g, sizeof g); pthread_mutex_lock(&gmu);
        for (int i = 0; i < NB_GATES; i++) if (!strcmp(gates[i].name, g)) gates[i].state = GATE_UNAVAILABLE;
        pthread_mutex_unlock(&gmu);
    } else if (m->type == EV_RECHERCHE_PORTE) {
        kv_get(m->payload, "flight", fl, sizeof fl); const char *res = "NONE";
        pthread_mutex_lock(&gmu); usleep(30000); /* recherche de porte = 30 ms dans la section critique */
        for (int i = 0; i < NB_GATES; i++) if (gates[i].state == GATE_FREE) { gates[i].state = GATE_OCCUPIED; snprintf(gates[i].flight, 8, "%s", fl); res = gates[i].name; break; }
        pthread_mutex_unlock(&gmu);
        message_t o; msg_init(&o, EV_GATE_ASSIGNED, "NODE_A");
        snprintf(o.payload, sizeof o.payload, "flight=%s,gate=%s", fl, res); net_send(PORT_NODE_C, &o);
    }
}
static void on_b(const message_t *m, void *arg) { (void)arg; char fl[16];
    if (m->type != EV_FLIGHT_DELAY) return;
    kv_get(m->payload, "flight", fl, sizeof fl); double risk = 0.50;   /* risque FIXE par vol : stub de predict() */
    if (!strncmp(fl, "TU701", 5)) risk = 0.87; else if (!strncmp(fl, "TU702", 5)) risk = 0.20; else if (!strncmp(fl, "TU703", 5)) risk = 0.65;
    usleep(10000); /* temps d'inference simule */
    message_t o; msg_init(&o, EV_RISK_RESULT, "NODE_B");
    snprintf(o.payload, sizeof o.payload, "flight=%s,risk=%.2f,src_ts=%llu", fl, risk, (unsigned long long)kv_get_double(m->payload, "src_ts", 0));
    net_send(PORT_NODE_C, &o); printf("[B] risque %s = %.2f\n", fl, risk); fflush(stdout);
}
static void on_p(const message_t *m, void *arg) { (void)arg; char fl[16];
    printf("[P] recu %s %s\n", event_name(m->type), m->payload); fflush(stdout);
    if (m->type == EV_BUS_BREAKDOWN) buses = (int)kv_get_long(m->payload, "buses", buses - 1);
    if (m->type == EV_TRAITER_GROUPE) { kv_get(m->payload, "flight", fl, sizeof fl); usleep(10000);
        message_t o; msg_init(&o, EV_ACK, "NODE_P"); snprintf(o.payload, sizeof o.payload, "flight=%s,bus=%d", fl, buses); net_send(PORT_NODE_C, &o); }
}
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s a|b|p\n", argv[0]); return 1; }
    int port = argv[1][0] == 'a' ? PORT_NODE_A : argv[1][0] == 'b' ? PORT_NODE_B : PORT_NODE_P;
    msg_handler_t h = argv[1][0] == 'a' ? on_a : argv[1][0] == 'b' ? on_b : on_p;
    if (net_serve_start(port, h, NULL)) return 1;
    printf("[stub %s] port %d\n", argv[1], port); fflush(stdout);
    for (;;) pause();
}
