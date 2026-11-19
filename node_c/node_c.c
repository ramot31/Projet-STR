/* node_c.c - Noeud C : Decision / Ordonnanceur (M1), port 5003.
 *
 * Flux :  A --FLIGHT_DELAY(copie)--> C        (C connait le vol et demarre un chrono)
 *         B --RISK_RESULT--> C                (risque IA)
 *         C --RECHERCHE_PORTE--> A  /  C --TRAITER_GROUPE--> P
 *         A --GATE_ASSIGNED--> C ,  P --ACK--> C
 * Mode degrade : si B ne repond pas dans --timeout-ms, C decide avec fallback_risk() (sans IA).
 * Politique : --policy ai    -> les vols a risque eleve sont traites en premier
 *             --policy fixed -> ordre d'arrivee (priorites fixes), pour comparer.
 * Convention : le champ src_ts (horodatage de l'evenement initial) doit etre propage par A et B. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include "fallback.h"
#include "net.h"
#include "types.h"

#define MAXF 512
#define T4_DEADLINE_MS 100.0
enum { ST_WAIT_RISK, ST_QUEUED, ST_DISPATCHED };

typedef struct {
    char id[16]; int delay, time_left, conn;
    int state, gate_done, order, got_delay;
    char source[10], gate[8], level_s[10];
    double risk; risk_level_t level;
    uint64_t src_ts, t_delay, t_risk, t_dispatch, t_gate, t_ack;
} fl_t;

/* donnees de base des vols de demo : id, arrivee prevue, vol suivant (minutes depuis minuit), correspondances */
static const struct { const char *id; int sched, next, conn; } BASE[] = {
    { "TU701", 620, 675, 35 }, { "TU702", 630, 690, 12 }, { "TU703", 625, 680, 20 }, { "TU704", 635, 685, 8 } };

static fl_t g_fl[MAXF]; static int g_n, g_order, g_done, g_stop;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER, g_logmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static int g_policy_ai = 1, g_timeout_ms = 300, g_window_ms = 20, g_idle_ms = 0;
static const char *g_csv = NULL;
static uint64_t g_t0, g_last;

static void say(const char *fmt, ...) {
    pthread_mutex_lock(&g_logmu);
    if (!g_t0) g_t0 = now_ns();
    printf("[+%8.1f ms] ", (now_ns() - g_t0) / 1e6);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n"); fflush(stdout);
    pthread_mutex_unlock(&g_logmu);
}
static int wait_ms(int ms) { /* g_mu doit etre verrouille */
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000L; ts.tv_sec += ts.tv_nsec / 1000000000L; ts.tv_nsec %= 1000000000L;
    return pthread_cond_timedwait(&g_cv, &g_mu, &ts);
}
static const char *lvl_name(risk_level_t l) { return l == RISK_CRITICAL ? "CRITIQUE" : l == RISK_MEDIUM ? "MOYEN" : "FAIBLE"; }

static fl_t *find_or_create(const char *id) { /* g_mu verrouille */
    for (int i = 0; i < g_n; i++) if (!strcmp(g_fl[i].id, id)) return &g_fl[i];
    if (g_n >= MAXF) return NULL;
    fl_t *f = &g_fl[g_n++]; memset(f, 0, sizeof *f);
    snprintf(f->id, sizeof f->id, "%s", id); f->t_delay = now_ns(); f->state = ST_WAIT_RISK;
    f->time_left = 60; f->conn = 10;
    return f;
}

static void on_msg(const message_t *m, void *arg) {
    (void)arg; char id[16];
    pthread_mutex_lock(&g_mu); g_last = now_ns();
    if (kv_get(m->payload, "flight", id, sizeof id)) { pthread_mutex_unlock(&g_mu); say("RECU %s (%s)", event_name(m->type), m->payload); return; }
    fl_t *f = find_or_create(id);
    if (!f) { pthread_mutex_unlock(&g_mu); return; }
    uint64_t src = (uint64_t)kv_get_double(m->payload, "src_ts", 0);
    if (src && !f->src_ts) f->src_ts = src;
    switch (m->type) {
    case EV_FLIGHT_DELAY: {
        f->delay = (int)kv_get_long(m->payload, "delay", 0); f->got_delay = 1;
        for (unsigned i = 0; i < sizeof BASE / sizeof BASE[0]; i++) if (!strncmp(id, BASE[i].id, 5)) {
            f->conn = BASE[i].conn; f->time_left = BASE[i].next - (BASE[i].sched + f->delay); }
        say("RECU FLIGHT_DELAY %s retard=%d min, temps restant=%d min, correspondances=%d (attente du risque IA, repli apres %d ms)",
            id, f->delay, f->time_left, f->conn, g_timeout_ms);
        break; }
    case EV_RISK_RESULT:
        if (f->state != ST_WAIT_RISK) { say("RISK_RESULT tardif ignore pour %s (deja decide)", id); break; }
        f->risk = kv_get_double(m->payload, "risk", 0.0); f->level = risk_to_level(f->risk);
        snprintf(f->source, sizeof f->source, "IA"); f->state = ST_QUEUED; f->t_risk = now_ns();
        say("RECU RISK_RESULT %s risque=%.2f -> niveau %s", id, f->risk, lvl_name(f->level));
        pthread_cond_broadcast(&g_cv); break;
    case EV_GATE_ASSIGNED:
        kv_get(m->payload, "gate", f->gate, sizeof f->gate); f->t_gate = now_ns(); f->gate_done = 1;
        say("RECU GATE_ASSIGNED %s -> porte %s", id, f->gate); pthread_cond_broadcast(&g_cv); break;
    case EV_ACK: f->t_ack = now_ns(); say("RECU ACK %s (groupe traite)", id); break;
    default: say("RECU %s (%s)", event_name(m->type), m->payload);
    }
    pthread_mutex_unlock(&g_mu);
}

static void *watchdog(void *arg) { /* mode degrade */
    (void)arg;
    while (!g_stop) {
        usleep(10000); pthread_mutex_lock(&g_mu); uint64_t now = now_ns();
        for (int i = 0; i < g_n; i++) { fl_t *f = &g_fl[i];
            if (f->state == ST_WAIT_RISK && f->got_delay && (now - f->t_delay) / 1000000 >= (uint64_t)g_timeout_ms) {
                f->risk = fallback_risk(f->delay, f->time_left); f->level = risk_to_level(f->risk);
                snprintf(f->source, sizeof f->source, "REPLI"); f->state = ST_QUEUED; f->t_risk = now;
                say("MODE DEGRADE : pas de reponse de B pour %s -> regle de repli, risque=%.2f niveau %s", f->id, f->risk, lvl_name(f->level));
                pthread_cond_broadcast(&g_cv); } }
        pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}

static int cmp_idx(const void *a, const void *b) {
    const fl_t *x = &g_fl[*(const int *)a], *y = &g_fl[*(const int *)b];
    if (g_policy_ai) { if (x->risk != y->risk) return x->risk < y->risk ? 1 : -1; }
    return x->t_risk < y->t_risk ? -1 : (x->t_risk > y->t_risk);
}

static void *decision(void *arg) {
    (void)arg; pthread_mutex_lock(&g_mu);
    while (!g_stop) {
        int has = 0; for (int i = 0; i < g_n; i++) if (g_fl[i].state == ST_QUEUED) has = 1;
        if (!has) { wait_ms(100); continue; }
        pthread_mutex_unlock(&g_mu); usleep(g_window_ms * 1000); pthread_mutex_lock(&g_mu); /* fenetre de regroupement */
        int idx[MAXF], k = 0; for (int i = 0; i < g_n; i++) if (g_fl[i].state == ST_QUEUED) idx[k++] = i;
        qsort(idx, k, sizeof(int), cmp_idx);
        { char ord[256] = ""; for (int j = 0; j < k; j++) { size_t l = strlen(ord); if (l > 200) break; snprintf(ord + l, sizeof ord - l, "%s%s(%.2f)", j ? " > " : "", g_fl[idx[j]].id, g_fl[idx[j]].risk); }
          say("DECISION [%s] ordre de traitement : %s", g_policy_ai ? "priorite IA" : "priorites fixes", ord); }
        for (int j = 0; j < k; j++) {
            fl_t *f = &g_fl[idx[j]]; f->state = ST_DISPATCHED; f->t_dispatch = now_ns(); f->order = ++g_order;
            char id[16], pl[160]; snprintf(id, sizeof id, "%s", f->id); int conn = f->conn; double risk = f->risk;
            risk_level_t lv = f->level; uint64_t src = f->src_ts ? f->src_ts : f->t_delay;
            pthread_mutex_unlock(&g_mu);
            message_t a, p;
            msg_init(&a, EV_RECHERCHE_PORTE, "NODE_C"); snprintf(pl, sizeof pl, "flight=%s,level=%s,risk=%.2f,src_ts=%llu", id, lvl_name(lv), risk, (unsigned long long)src); strcpy(a.payload, pl);
            msg_init(&p, EV_TRAITER_GROUPE, "NODE_C"); snprintf(p.payload, sizeof p.payload, "flight=%s,conn=%d,level=%s,src_ts=%llu", id, conn, lvl_name(lv), (unsigned long long)src);
            int ra = net_send(PORT_NODE_A, &a), rp = net_send(PORT_NODE_P, &p);
            say("ENVOI RECHERCHE_PORTE -> A et TRAITER_GROUPE -> P pour %s (priorite %s)%s", id, lvl_name(lv), (ra || rp) ? " [ECHEC ENVOI]" : "");
            pthread_mutex_lock(&g_mu);
            while (!f->gate_done) if (wait_ms(600) == ETIMEDOUT) { say("TIMEOUT : pas de porte recue pour %s", id); break; }
            g_done++;
        }
    }
    pthread_mutex_unlock(&g_mu); return NULL;
}

static void summary(void) {
    pthread_mutex_lock(&g_mu);
    printf("\n=========== RESUME (politique : %s) ===========\n", g_policy_ai ? "priorite IA" : "priorites fixes");
    printf("%-3s %-9s %-6s %-5s %-9s %-5s %8s %8s %9s  %s\n", "#", "vol", "src", "risque", "niveau", "porte", "IA(ms)", "T4(ms)", "bout-bout", "T4<=100ms");
    FILE *c = g_csv ? fopen(g_csv, "w") : NULL; if (c) fprintf(c, "order,flight,source,risk,level,gate,ai_ms,t4_ms,e2e_ms\n");
    double sum = 0; int n = 0, miss = 0;
    for (int o = 1; o <= g_order; o++) for (int i = 0; i < g_n; i++) { fl_t *f = &g_fl[i]; if (f->order != o) continue;
        uint64_t src = f->src_ts ? f->src_ts : f->t_delay;
        double ai = (f->t_risk - src) / 1e6, t4 = (f->t_dispatch - f->t_risk) / 1e6, e2e = f->t_gate ? (f->t_gate - src) / 1e6 : -1;
        int ok = t4 <= T4_DEADLINE_MS; if (!ok) miss++; if (e2e >= 0) { sum += e2e; n++; }
        printf("%-3d %-9s %-6s %-6.2f %-9s %-5s %8.1f %8.1f %9.1f  %s\n", o, f->id, f->source, f->risk, lvl_name(f->level), f->gate, ai, t4, e2e, ok ? "OK" : "DEPASSE");
        if (c) fprintf(c, "%d,%s,%s,%.2f,%s,%s,%.1f,%.1f,%.1f\n", o, f->id, f->source, f->risk, lvl_name(f->level), f->gate, ai, t4, e2e); }
    printf("Vols traites : %d | temps moyen evenement -> decision finale : %.1f ms | depassements T4 : %d\n", g_order, n ? sum / n : 0.0, miss);
    if (c) { fclose(c); printf("CSV : %s\n", g_csv); }
    pthread_mutex_unlock(&g_mu);
}
static void on_sig(int s) { (void)s; g_stop = 1; }

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--policy") && i + 1 < argc) g_policy_ai = strcmp(argv[++i], "fixed") != 0;
        else if (!strcmp(argv[i], "--timeout-ms") && i + 1 < argc) g_timeout_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--window-ms") && i + 1 < argc) g_window_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--idle-exit") && i + 1 < argc) g_idle_ms = (int)(atof(argv[++i]) * 1000);
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) g_csv = argv[++i];
        else { fprintf(stderr, "usage: %s [--policy ai|fixed] [--timeout-ms N] [--window-ms N] [--idle-exit S] [--csv f]\n", argv[0]); return 1; }
    }
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    if (net_serve_start(PORT_NODE_C, on_msg, NULL)) return 1;
    pthread_t td, tw; pthread_create(&td, NULL, decision, NULL); pthread_create(&tw, NULL, watchdog, NULL);
    say("Noeud C pret (port %d, politique %s, repli apres %d ms)", PORT_NODE_C, g_policy_ai ? "IA" : "fixe", g_timeout_ms);
    while (!g_stop) {
        usleep(100000);
        pthread_mutex_lock(&g_mu); int idle = g_idle_ms && g_order > 0 && g_done == g_order && (now_ns() - g_last) / 1000000 > (uint64_t)g_idle_ms; pthread_mutex_unlock(&g_mu);
        if (idle) break;
    }
    g_stop = 1; usleep(150000); summary(); return 0;
}
