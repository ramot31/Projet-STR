/* flights.c - Ordonnancement intelligent dans les vrais threads (M3, milestone 6).
 * Pour chaque vol decide par T4, un thread execute les travaux associes :
 *   1. recherche de porte   (calcul + gate_reserve : semaphore des portes de M4)
 *   2. chemin passager      (calcul)
 *   3. notification         (calcul + bus_reserve : semaphore compteur des bus de M4)
 * La PRIORITE OS du thread vient de la politique :
 *   --policy fixed : ordre d'arrivee (le premier vol detecte est le plus prioritaire)
 *   --policy ai    : base + min(3, risque x 3) (regle de M5) -> TU701 (87 %) passe devant TU702 (20 %)
 * Tous les threads d'un lot attendent un "top depart" commun : ils entrent en concurrence au meme
 * instant et c'est l'ordonnanceur du noyau qui les departage selon leur priorite. */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "rt_time.h"

#define GATE_SEARCH_MS   20.0
#define PATH_MS          15.0
#define NOTIFY_MS         5.0
#define GATE_TIMEOUT_MS  50
#define BUS_TIMEOUT_MS   50
#define PAX_PER_BUS      40
#define MAX_BUS_PER_FL   NB_BUSES

typedef struct { pthread_mutex_t mu; pthread_cond_t cv; int go; } start_gate_t;
static start_gate_t g_start[MAX_FL];

static int cmp_fixed(const void *a, const void *b) {
    const flight_rec_t *x = &g_eng.fl[*(const int *)a], *y = &g_eng.fl[*(const int *)b];
    return x->arrival_rank - y->arrival_rank;
}
static int cmp_ai(const void *a, const void *b) {
    const flight_rec_t *x = &g_eng.fl[*(const int *)a], *y = &g_eng.fl[*(const int *)b];
    if (x->risk != y->risk) return x->risk < y->risk ? 1 : -1;
    return x->arrival_rank - y->arrival_rank;
}

static void wait_start(start_gate_t *g) {
    pthread_mutex_lock(&g->mu);
    while (!g->go) pthread_cond_wait(&g->cv, &g->mu);
    pthread_mutex_unlock(&g->mu);
}

static int reserve_buses(int nb, int ids[]) {
    int got = 0;
    for (int i = 0; i < nb && i < MAX_BUS_PER_FL; i++)
        if (bus_reserve(BUS_TIMEOUT_MS, &ids[got]) == RES_OK) got++;
    return got;
}

static void *flight_main(void *arg) {
    flight_rec_t *f = arg;
    wait_start(&g_start[f->batch]);
    f->t_start = mono_ns();
    /* 1. recherche de porte : la reservation est protegee par le semaphore + mutex de M4 */
    burn_cpu_ms(GATE_SEARCH_MS);
    char gate[GATE_NAME_LEN];
    if (gate_reserve(f->id, GATE_TIMEOUT_MS, gate) == RES_OK) snprintf(f->gate, sizeof f->gate, "%s", gate);
    else snprintf(f->gate, sizeof f->gate, "---");
    f->t_gate = mono_ns();
    /* 2. chemin passager */
    burn_cpu_ms(PATH_MS);
    f->t_path = mono_ns();
    /* 3. bus + notification (le trajet lui-meme est simule par le noeud P : bus rendu apres l'envoi) */
    int ids[MAX_BUS_PER_FL], got = reserve_buses(f->nb_bus, ids);
    f->bus_id = got ? ids[0] : 0;
    burn_cpu_ms(NOTIFY_MS);
    for (int i = 0; i < got; i++) bus_release(ids[i]);
    f->t_notify = mono_ns();
    eng_log("VOL", "%s [%s] porte %s, %d/%d bus, passagers notifies : reponse %.1f ms depuis l'evenement",
            f->id, rt_sched_fifo_active() ? "FIFO" : "nice", f->gate, got, f->nb_bus,
            (double)(f->t_notify - f->t_src) / NS_PER_MS);
    pthread_mutex_lock(&g_eng.mu);
    f->done = 1;
    g_eng.ndone++;
    pthread_mutex_unlock(&g_eng.mu);
    return NULL;
}

static void log_order(const int *idx, int k) {
    char ord[320] = "";
    for (int j = 0; j < k; j++) {
        const flight_rec_t *f = &g_eng.fl[idx[j]];
        size_t l = strlen(ord);
        if (l > sizeof ord - 48) { snprintf(ord + l, sizeof ord - l, " ..."); break; }
        snprintf(ord + l, sizeof ord - l, "%s%s(%.2f, %s %d)", j ? " > " : "", f->id, f->risk,
                 rt_sched_fifo_active() ? "FIFO" : "nice", rt_sched_fifo_active() ? f->fifo_prio : f->nice);
    }
    eng_log("T4", "DECISION [%s] %s", g_eng.policy == POLICY_AI ? "priorite IA" : "priorites fixes", ord);
}

void flights_dispatch(int *idx, int k) {
    if (k <= 0) return;
    pthread_mutex_lock(&g_eng.mu);
    int b = g_eng.nbatch++ % MAX_FL;
    start_gate_t *g = &g_start[b];
    pthread_mutex_init(&g->mu, NULL); pthread_cond_init(&g->cv, NULL); g->go = 0;
    qsort(idx, (size_t)k, sizeof idx[0], g_eng.policy == POLICY_AI ? cmp_ai : cmp_fixed);
    for (int j = 0; j < k; j++) {
        flight_rec_t *f = &g_eng.fl[idx[j]];
        f->order = ++g_eng.ndispatched;
        f->batch = b;
        f->boost = m5_boost(f->risk, g_eng.policy);
        f->fifo_prio = job_fifo_prio(g_eng.policy, f->boost, j);
        f->nice = job_nice(g_eng.policy, f->boost, j);
        f->nb_bus = (f->conn + PAX_PER_BUS - 1) / PAX_PER_BUS;
        f->t_dispatch = mono_ns();
    }
    log_order(idx, k);
    for (int j = 0; j < k; j++) {
        flight_rec_t *f = &g_eng.fl[idx[j]];
        if (rt_thread_start(&f->th, f->fifo_prio, f->nice, flight_main, f) == 0) f->launched = 1;
        else { f->done = 1; g_eng.ndone++; }
    }
    pthread_mutex_unlock(&g_eng.mu);
    pthread_mutex_lock(&g->mu); g->go = 1; pthread_cond_broadcast(&g->cv); pthread_mutex_unlock(&g->mu);
}

void flights_join_all(void) {
    for (int i = 0; i < g_eng.nfl; i++) if (g_eng.fl[i].launched) pthread_join(g_eng.fl[i].th, NULL);
}

static double ms_between(uint64_t a, uint64_t b) { return a && b >= a ? (double)(b - a) / NS_PER_MS : -1.0; }

void flights_print_summary(FILE *out) {
    fprintf(out, "%-3s %-9s %-6s %-9s %-6s %-8s %-5s %-4s %8s %8s %9s %9s %12s\n", "#", "vol", "risque", "niveau",
            "source", "prio", "porte", "bus", "detect", "risque", "decision", "reponse", "apres decis.");
    for (int o = 1; o <= g_eng.ndispatched; o++) for (int i = 0; i < g_eng.nfl; i++) {
        const flight_rec_t *f = &g_eng.fl[i];
        if (f->order != o) continue;
        char prio[16];
        if (rt_sched_fifo_active()) snprintf(prio, sizeof prio, "FIFO %d", f->fifo_prio);
        else snprintf(prio, sizeof prio, "nice %d", f->nice);
        fprintf(out, "%-3d %-9s %-6.2f %-9s %-6s %-8s %-5s %-4d %8.1f %8.1f %9.1f %9.1f %12.1f\n", o, f->id,
                f->risk, m5_level_name(f->level), f->source, prio, f->gate, f->bus_id,
                ms_between(f->t_src, f->t_detect), ms_between(f->t_src, f->t_risk),
                ms_between(f->t_src, f->t_dispatch), ms_between(f->t_src, f->t_notify),
                ms_between(f->t_dispatch, f->t_notify));
    }
    fprintf(out, "(ms depuis l'evenement initial ; reponse = evenement -> passagers notifies ;\n"
                 " apres decis. = decision de T4 -> passagers notifies : effet de la priorite seule)\n");
}

int flights_write_csv(const char *path) {
    FILE *c = fopen(path, "w");
    if (!c) { perror(path); return -1; }
    fprintf(c, "policy,sched,order,flight,risk,level,source,fifo_prio,nice,gate,bus,detect_ms,risk_ms,"
               "dispatch_ms,gate_ms,notify_ms,after_decision_ms\n");
    for (int o = 1; o <= g_eng.ndispatched; o++) for (int i = 0; i < g_eng.nfl; i++) {
        const flight_rec_t *f = &g_eng.fl[i];
        if (f->order != o) continue;
        fprintf(c, "%s,%s,%d,%s,%.2f,%s,%s,%d,%d,%s,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                g_eng.policy == POLICY_AI ? "ai" : "fixed", rt_sched_fifo_active() ? "fifo" : "nice", o, f->id,
                f->risk, m5_level_name(f->level), f->source, f->fifo_prio, f->nice, f->gate, f->bus_id,
                ms_between(f->t_src, f->t_detect), ms_between(f->t_src, f->t_risk),
                ms_between(f->t_src, f->t_dispatch), ms_between(f->t_src, f->t_gate),
                ms_between(f->t_src, f->t_notify), ms_between(f->t_dispatch, f->t_notify));
    }
    fclose(c);
    return 0;
}
