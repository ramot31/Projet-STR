/* tasks.c - Corps des taches temps reel (M3).
 *   T1 surveillance vols    periodique  T=100 D=100  : lit les retards, declenche T3
 *   T2 surveillance files   periodique  T=500 D=500  : file de securite, ralentissement, pannes de bus
 *   ALERTE porte HS         sporadique  D=50         : retire la porte (semaphore de M4)
 *   T3 prediction IA        sporadique  D=200        : risque de correspondance manquee
 *   T4 decision/ressources  sporadique  D=100        : regroupe, ordonne, lance les travaux de vol
 *   BG analyse historique   aperiodique sans deadline : charge de fond, priorite la plus basse */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "fallback.h"
#include "net.h"
#include "rt_time.h"

#define PUSH_TIMEOUT_MS   20
#define SEC_MIN_PER_PAX   0.1     /* attente securite = personnes x 0,1 min x ralentissement (cadrage) */
#define BG_CHUNK_MS       5.0
#define DEFAULT_TIME_LEFT 60
#define DEFAULT_CONN      10

/* Donnees de base des vols de demo (identiques au noeud C de M1) : minutes depuis minuit. */
static const struct { const char *id; int sched, next, conn; } BASE[] = {
    { "TU701", 620, 675, 35 }, { "TU702", 630, 690, 12 }, { "TU703", 625, 680, 20 }, { "TU704", 635, 685, 8 } };

/* Risques du noeud B (memes valeurs que le scenario de l'enonce et que stubs.c). Modifiables par --risk. */
static const struct { const char *id; double risk; } AI_RISK[] = {
    { "TU701", 0.87 }, { "TU702", 0.20 }, { "TU703", 0.65 }, { "TU704", 0.50 } };

#define NELEM(a) (int)(sizeof(a) / sizeof((a)[0]))

/* ------------------------------------------------------------------ T1 : surveillance des vols */

static void fill_base(flight_rec_t *f) {
    f->conn = DEFAULT_CONN; f->time_left = DEFAULT_TIME_LEFT;
    for (int i = 0; i < NELEM(BASE); i++) if (!strncmp(f->id, BASE[i].id, 5)) {
        f->conn = BASE[i].conn;
        f->time_left = BASE[i].next - (BASE[i].sched + f->delay);
    }
}

static void handle_delay(const message_t *m) {
    char id[ID_LEN];
    if (kv_get(m->payload, "flight", id, sizeof id)) return;
    uint64_t now = mono_ns();
    pthread_mutex_lock(&g_eng.mu);
    flight_rec_t *f = eng_flight_get(id, 1);
    if (!f) { pthread_mutex_unlock(&g_eng.mu); eng_log("T1", "table des vols pleine, %s ignore", id); return; }
    if (f->launched) { pthread_mutex_unlock(&g_eng.mu); eng_log("T1", "%s deja traite, nouveau retard ignore", id); return; }
    f->delay = (int)kv_get_long(m->payload, "delay", 0);
    fill_base(f);
    f->t_src = eng_payload_u64(m->payload, "src");
    f->t_detect = now;
    int delay = f->delay, left = f->time_left, conn = f->conn;
    pthread_mutex_unlock(&g_eng.mu);
    eng_log("T1", "detecte retard %s : %d min, temps restant %d min, %d correspondances -> declenche T3",
            id, delay, left, conn);
    message_t a = *m;
    a.timestamp_ns = now;   /* liberation de T3 = detection par T1 */
    if (mq_push(&g_eng.q_ai, &a, PUSH_TIMEOUT_MS)) eng_log("T1", "ECHEC : file IA pleine (%s)", id);
}

void mon_body(void *arg) {
    (void)arg;
    message_t m;
    /* consommateur unique de q_flight : la taille ne peut que croitre entre le test et le pop */
    while (mq_size(&g_eng.q_flight) > 0 && mq_pop(&g_eng.q_flight, &m) == 0) handle_delay(&m);
}

/* ------------------------------------------------------------------ T2 : surveillance des files */

static void handle_ground(const message_t *m) {
    if (m->type == EV_QUEUE_UPDATE) {
        pthread_mutex_lock(&g_eng.mu);
        g_eng.sec_queue = (int)kv_get_long(m->payload, "queue", g_eng.sec_queue);
        double wait = g_eng.sec_queue * SEC_MIN_PER_PAX * g_eng.sec_slow;
        int q = g_eng.sec_queue;
        pthread_mutex_unlock(&g_eng.mu);
        eng_log("T2", "file de securite = %d personnes -> attente %.1f min", q, wait);
    } else if (m->type == EV_SECURITY_SLOWDOWN) {
        pthread_mutex_lock(&g_eng.mu);
        g_eng.sec_slow = kv_get_double(m->payload, "factor", 2.0);
        double slow = g_eng.sec_slow, wait = g_eng.sec_queue * SEC_MIN_PER_PAX * slow;
        pthread_mutex_unlock(&g_eng.mu);
        eng_log("T2", "ralentissement du controle x%.1f -> attente %.1f min", slow, wait);
    } else if (m->type == EV_BUS_BREAKDOWN) {
        long target = kv_get_long(m->payload, "buses", -1);
        int cap;
        if (target >= 0) cap = bus_set_capacity((int)target);
        else { bus_breakdown(); cap = buses_capacity(); }
        eng_log("T2", "panne de bus -> capacite %d bus (semaphore compteur ajuste)", cap);
    }
}

void pax_body(void *arg) {
    (void)arg;
    message_t m;
    while (mq_size(&g_eng.q_ground) > 0 && mq_pop(&g_eng.q_ground, &m) == 0) handle_ground(&m);
}

/* ------------------------------------------------------------------ alerte critique */

void *alert_main(void *arg) {
    (void)arg;
    message_t m;
    while (mq_pop(&g_eng.q_alert, &m) == 0) {
        uint64_t start = mono_ns(), cpu0 = thread_cpu_ns();
        char gate[8] = "?";
        kv_get(m.payload, "gate", gate, sizeof gate);
        int rc = gate_set_unavailable(gate);
        int left = gates_free_count();
        uint64_t end = mono_ns();
        int miss = rt_task_record(&g_eng.t_alert, m.timestamp_ns, start, end, thread_cpu_ns() - cpu0);
        eng_log("ALR", "ALERTE CRITIQUE porte %s hors service -> %s, portes libres : %d (R = %.3f ms, D = 50 ms%s)",
                gate, rc == RES_OK ? "retiree" : "inconnue", left,
                (double)(end - m.timestamp_ns) / NS_PER_MS, miss ? " DEPASSEE" : "");
    }
    return NULL;
}

/* ------------------------------------------------------------------ T3 : prediction IA */

static double estimate_risk(const char *id, int delay, int time_left, const char **source) {
    for (int i = 0; i < g_eng.nrisk; i++)
        if (!strcmp(g_eng.risk_ovr[i].flight, id)) { *source = "IA"; return g_eng.risk_ovr[i].risk; }
    for (int i = 0; i < NELEM(AI_RISK); i++)
        if (!strncmp(id, AI_RISK[i].id, 5)) { *source = "IA"; return AI_RISK[i].risk; }
    *source = "REPLI";   /* vol inconnu du modele : regle de repli sans IA de M1 */
    return fallback_risk(delay, time_left);
}

void *ai_main(void *arg) {
    (void)arg;
    message_t m;
    while (mq_pop(&g_eng.q_ai, &m) == 0) {
        uint64_t start = mono_ns(), cpu0 = thread_cpu_ns();
        char id[ID_LEN];
        if (kv_get(m.payload, "flight", id, sizeof id)) continue;
        burn_cpu_ms(g_eng.ai_cost_ms);            /* cout d'inference mesure par M2 (3,98 ms) */
        pthread_mutex_lock(&g_eng.mu);
        flight_rec_t *f = eng_flight_get(id, 0);
        if (!f) { pthread_mutex_unlock(&g_eng.mu); continue; }
        const char *src;
        f->risk = estimate_risk(id, f->delay, f->time_left, &src);
        f->level = m5_level(f->risk);
        snprintf(f->source, sizeof f->source, "%s", src);
        f->has_risk = 1;
        f->t_risk = mono_ns();
        double risk = f->risk; risk_level_t lv = f->level; uint64_t t_risk = f->t_risk;
        pthread_mutex_unlock(&g_eng.mu);
        uint64_t end = mono_ns();
        rt_task_record(&g_eng.t_ai, m.timestamp_ns, start, end, thread_cpu_ns() - cpu0);
        eng_log("T3", "risque %s = %.2f (%s, source %s)", id, risk, m5_level_name(lv), src);
        message_t d = m;
        d.timestamp_ns = t_risk;                  /* liberation de T4 = risque connu */
        if (mq_push(&g_eng.q_decision, &d, PUSH_TIMEOUT_MS)) eng_log("T3", "ECHEC : file decision pleine");
    }
    return NULL;
}

/* ------------------------------------------------------------------ T4 : decision */

static void add_to_batch(const message_t *m, int *idx, int *k) {
    char id[ID_LEN];
    if (kv_get(m->payload, "flight", id, sizeof id)) return;
    pthread_mutex_lock(&g_eng.mu);
    flight_rec_t *f = eng_flight_get(id, 0);
    int i = f ? (int)(f - g_eng.fl) : -1;
    pthread_mutex_unlock(&g_eng.mu);
    for (int j = 0; j < *k; j++) if (idx[j] == i) return;
    if (i >= 0 && *k < MAX_FL) idx[(*k)++] = i;
}

void *decision_main(void *arg) {
    (void)arg;
    message_t m;
    while (mq_pop(&g_eng.q_decision, &m) == 0) {
        uint64_t release = m.timestamp_ns;        /* risque du premier vol du lot connu */
        uint64_t start = mono_ns(), cpu0 = thread_cpu_ns();
        int idx[MAX_FL], k = 0;
        add_to_batch(&m, idx, &k);
        /* fenetre de regroupement : les situations quasi simultanees sont comparees ensemble */
        sleep_until_ns(start + (uint64_t)g_eng.window_ms * NS_PER_MS);
        while (mq_size(&g_eng.q_decision) > 0 && mq_pop(&g_eng.q_decision, &m) == 0) add_to_batch(&m, idx, &k);
        flights_dispatch(idx, k);
        uint64_t end = mono_ns();
        int miss = rt_task_record(&g_eng.t_dec, release, start, end, thread_cpu_ns() - cpu0);
        eng_log("T4", "lot de %d vol(s) ordonne et lance : R = %.3f ms (D = 100 ms%s)", k,
                (double)(end - release) / NS_PER_MS, miss ? " DEPASSEE" : "");
    }
    return NULL;
}

/* ------------------------------------------------------------------ analyse de fond */

void *background_main(void *arg) {
    (void)arg;
    while (!atomic_load(&g_eng.stop)) {
        uint64_t start = mono_ns(), cpu0 = thread_cpu_ns();
        burn_cpu_ms(BG_CHUNK_MS);                 /* "analyse historique" : occupe le CPU libre */
        rt_task_record(&g_eng.t_bg, start, start, mono_ns(), thread_cpu_ns() - cpu0);
        pthread_mutex_lock(&g_eng.mu); g_eng.bg_chunks++; pthread_mutex_unlock(&g_eng.mu);
    }
    return NULL;
}
