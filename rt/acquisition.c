/* acquisition.c - T0 : acquisition des evenements (M3).
 * Rejoue un scenario au format du simulateur de M1 (t_s ; noeud ; TYPE ; cle=val,...), au bon instant
 * (clock_nanosleep absolu), puis DEPOSE chaque evenement dans la file du thread qui doit le traiter.
 * T0 ne decide rien : il recoit, horodate et route. Ainsi aucune tache temps reel n'attend une entree.
 *   GATE_UNAVAILABLE              -> q_alert  (alerte critique, D = 50 ms)
 *   FLIGHT_DELAY                  -> q_flight (lu par T1 toutes les 100 ms)
 *   QUEUE_UPDATE / SLOWDOWN / BUS -> q_ground (lu par T2 toutes les 500 ms)
 * Les valeurs se changent sans recompiler : --set TU701.delay=25 --set queue=150, --load K. */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "net.h"
#include "rt_time.h"

#define MAX_EV           512
#define EV_PAYLOAD_LEN   112
#define PUSH_TIMEOUT_MS  20
#define MAX_KV           10

typedef struct { double t; char node; int type; char payload[EV_PAYLOAD_LEN]; } ev_t;
static ev_t g_ev[MAX_EV];
static int  g_nev;

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
    return s;
}

int acq_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }
    char line[300];
    while (fgets(line, sizeof line, f) && g_nev < MAX_EV) {
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        char *f1 = strtok(s, ";"), *f2 = strtok(NULL, ";"), *f3 = strtok(NULL, ";"), *f4 = strtok(NULL, "");
        if (!f1 || !f2 || !f3 || !f4) { fprintf(stderr, "ligne ignoree : %s\n", s); continue; }
        int ty = event_from_name(trim(f3));
        if (ty < 0) { fprintf(stderr, "type inconnu : %s\n", f3); continue; }
        ev_t *e = &g_ev[g_nev++];
        e->t = atof(trim(f1)); e->node = trim(f2)[0]; e->type = ty;
        snprintf(e->payload, sizeof e->payload, "%s", trim(f4));
    }
    fclose(f);
    return 0;
}

int acq_count(void) { return g_nev; }

/* Remplace la valeur des cles deja presentes selon les --set (meme semantique que le simulateur). */
static void apply_overrides(char *payload, size_t n) {
    char keys[MAX_KV][24], vals[MAX_KV][48], flight[ID_LEN] = "", tmp[EV_PAYLOAD_LEN];
    int k = 0;
    snprintf(tmp, sizeof tmp, "%s", payload);
    for (char *save = NULL, *tok = strtok_r(tmp, ",", &save); tok && k < MAX_KV; tok = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        snprintf(keys[k], sizeof keys[k], "%s", tok); snprintf(vals[k], sizeof vals[k], "%s", eq + 1);
        if (!strcmp(keys[k], "flight")) snprintf(flight, sizeof flight, "%s", vals[k]);
        k++;
    }
    for (int o = 0; o < g_eng.novr; o++) {
        const override_t *ov = &g_eng.ovr[o];
        if (ov->flight[0] && strcmp(ov->flight, flight)) continue;
        for (int i = 0; i < k; i++) if (!strcmp(keys[i], ov->key)) snprintf(vals[i], sizeof vals[i], "%s", ov->val);
    }
    payload[0] = 0;
    for (int i = 0; i < k; i++) {
        size_t l = strlen(payload);
        int w = snprintf(payload + l, n - l, "%s%s=%s", i ? "," : "", keys[i], vals[i]);
        if (w < 0 || (size_t)w >= n - l) { fprintf(stderr, "payload tronque : %s\n", payload); break; }
    }
}

/* Copie numero c (>= 2) d'un retard pour les tests de charge : TU701 -> TU701x2. */
static void rename_copy(char *payload, size_t n, int c) {
    char fl[ID_LEN], rest[EV_PAYLOAD_LEN];
    char *pos = strstr(payload, "flight=");
    if (!pos || kv_get(payload, "flight", fl, sizeof fl)) return;
    char *comma = strchr(pos, ',');
    snprintf(rest, sizeof rest, "%s", comma ? comma : "");
    snprintf(pos, n - (size_t)(pos - payload), "flight=%sx%d%s", fl, c, rest);
}

static msg_queue_t *route_queue(int type) {
    switch (type) {
    case EV_GATE_UNAVAILABLE:  return &g_eng.q_alert;
    case EV_FLIGHT_DELAY:      return &g_eng.q_flight;
    case EV_QUEUE_UPDATE:
    case EV_SECURITY_SLOWDOWN:
    case EV_BUS_BREAKDOWN:     return &g_eng.q_ground;
    default:                   return NULL;
    }
}

static void emit(int type, const char *payload, uint64_t release) {
    message_t m;
    msg_init(&m, (event_type_t)type, "SIM");
    m.timestamp_ns = release;   /* usage interne : horodatage MONOTONIC de liberation */
    snprintf(m.payload, sizeof m.payload, "%s,src=%llu", payload, (unsigned long long)release);
    msg_queue_t *q = route_queue(type);
    if (!q) { eng_log("T0", "evenement %s ignore (non gere par ce noeud)", event_name(m.type)); return; }
    if (mq_push(q, &m, PUSH_TIMEOUT_MS)) eng_log("T0", "ECHEC : file pleine, %s perdu", event_name(m.type));
    else eng_log("T0", "recu %-17s %s -> file", event_name(m.type), payload);
}

void *acq_main(void *arg) {
    (void)arg;
    for (int i = 0; i < g_nev && !atomic_load(&g_eng.stop); i++) {
        const ev_t *e = &g_ev[i];
        uint64_t release = g_eng.t0 + (uint64_t)(e->t / g_eng.speed * (double)NS_PER_S);
        sleep_until_ns(release);
        uint64_t start = mono_ns(), cpu0 = thread_cpu_ns();
        char pl[EV_PAYLOAD_LEN];
        snprintf(pl, sizeof pl, "%s", e->payload);
        apply_overrides(pl, sizeof pl);
        int copies = e->type == EV_FLIGHT_DELAY ? g_eng.load : 1;
        for (int c = 1; c <= copies; c++) {
            char p2[EV_PAYLOAD_LEN];
            snprintf(p2, sizeof p2, "%s", pl);
            if (c > 1) rename_copy(p2, sizeof p2, c);
            emit(e->type, p2, release);
        }
        rt_task_record(&g_eng.t_acq, release, start, mono_ns(), thread_cpu_ns() - cpu0);
    }
    atomic_store(&g_eng.acq_done, 1);
    return NULL;
}
