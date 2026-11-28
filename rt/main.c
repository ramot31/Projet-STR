/* main.c - bin/rt_engine : moteur temps reel concurrent du systeme (M3).
 * Usage :
 *   bin/rt_engine [--scenario f] [--policy ai|fixed] [--speed x] [--set [VOL.]cle=val] [--load K]
 *                 [--risk VOL=r] [--ai-cost-ms c] [--window-ms w] [--cpu N|-1] [--no-fifo]
 *                 [--no-background] [--duration s] [--csv-prefix logs/rt_ai]
 * Exemples :
 *   sudo bin/rt_engine --scenario scenarios/demo_tu701.txt --set TU701.delay=25 --set queue=150
 *   sudo bin/rt_engine --scenario scenarios/multi_flights.txt --policy fixed
 * sudo donne le droit d'utiliser SCHED_FIFO ; sans lui le programme marche en repli (nice) et le dit. */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "engine.h"
#include "net.h"
#include "rt_time.h"

#define START_OFFSET_MS  200     /* laisse les threads demarrer avant le premier evenement */
#define QUIET_END_MS     600     /* fin quand plus rien ne bouge depuis ce delai */
#define POLL_MS          50
#define NB_QUEUES        5

#define LOG_BUFFER_SIZE  (1 << 20)

engine_t g_eng;
static pthread_mutex_t g_logmu;
static int g_log_tty;
static char g_log_buf[LOG_BUFFER_SIZE];

/* Journal horodate. Vers un terminal : ecrit ligne par ligne. Vers un fichier : mis en tampon
 * (1 Mo) et ecrit a la fin, car une ecriture disque dans une tache temps reel peut couter
 * plusieurs ms (mesure : T0 depassait sa deadline de 10 ms avec un fflush par ligne sur /mnt/c). */
void eng_log(const char *who, const char *fmt, ...) {
    double t = ((double)mono_ns() - (double)g_eng.t0) / (double)NS_PER_MS;
    pthread_mutex_lock(&g_logmu);
    printf("[%+10.3f ms] %-3s ", t, who);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    if (g_log_tty) fflush(stdout);
    pthread_mutex_unlock(&g_logmu);
}

flight_rec_t *eng_flight_get(const char *id, int create) {
    for (int i = 0; i < g_eng.nfl; i++) if (!strcmp(g_eng.fl[i].id, id)) return &g_eng.fl[i];
    if (!create || g_eng.nfl >= MAX_FL) return NULL;
    flight_rec_t *f = &g_eng.fl[g_eng.nfl];
    memset(f, 0, sizeof *f);
    snprintf(f->id, sizeof f->id, "%s", id);
    snprintf(f->gate, sizeof f->gate, "---");
    f->arrival_rank = g_eng.nfl++;    /* ordre de detection : base de la politique fixe */
    return f;
}

uint64_t eng_payload_u64(const char *payload, const char *key) {
    char b[32];
    return kv_get(payload, key, b, sizeof b) ? 0 : strtoull(b, NULL, 10);
}

/* ------------------------------------------------------------------ options */

static void usage(const char *p) {
    fprintf(stderr, "usage: %s [--scenario f] [--policy ai|fixed] [--speed x] [--set [VOL.]cle=val] [--load K]\n"
                    "          [--risk VOL=r] [--ai-cost-ms c] [--window-ms w] [--cpu N|-1] [--no-fifo]\n"
                    "          [--no-background] [--duration s] [--csv-prefix p]\n", p);
}

/* Copie bornee : refuse (au lieu de tronquer en silence) une valeur trop longue. */
static int copy_field(char *dst, size_t n, const char *src) {
    int w = snprintf(dst, n, "%s", src);
    if (w < 0 || (size_t)w >= n) { fprintf(stderr, "valeur trop longue : %s\n", src); return -1; }
    return 0;
}

static int add_override(const char *arg) {
    char buf[64];
    if (copy_field(buf, sizeof buf, arg)) return -1;
    char *eq = strchr(buf, '=');
    if (!eq || g_eng.novr >= MAX_OVR) return -1;
    *eq = 0;
    override_t o; memset(&o, 0, sizeof o);
    char *dot = strchr(buf, '.');
    if (dot) {
        *dot = 0;
        if (copy_field(o.flight, sizeof o.flight, buf) || copy_field(o.key, sizeof o.key, dot + 1)) return -1;
    } else if (copy_field(o.key, sizeof o.key, buf)) return -1;
    if (copy_field(o.val, sizeof o.val, eq + 1)) return -1;
    g_eng.ovr[g_eng.novr++] = o;
    return 0;
}

static int add_risk(const char *arg) {
    char buf[48];
    if (copy_field(buf, sizeof buf, arg)) return -1;
    char *eq = strchr(buf, '=');
    if (!eq || g_eng.nrisk >= MAX_OVR) return -1;
    *eq = 0;
    double r = atof(eq + 1);
    if (r < 0.0 || r > 1.0) { fprintf(stderr, "--risk : le risque doit etre dans [0, 1]\n"); return -1; }
    risk_override_t o;
    if (copy_field(o.flight, sizeof o.flight, buf)) return -1;
    o.risk = r;
    g_eng.risk_ovr[g_eng.nrisk++] = o;
    return 0;
}

static int parse_args(int argc, char **argv, int *want_fifo, int *cpu) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--no-fifo")) { *want_fifo = 0; continue; }
        if (!strcmp(a, "--no-background")) { g_eng.background = 0; continue; }
        if (!v) { usage(argv[0]); return -1; }
        i++;
        if (!strcmp(a, "--scenario")) g_eng.scenario = v;
        else if (!strcmp(a, "--policy")) g_eng.policy = strcmp(v, "fixed") ? POLICY_AI : POLICY_FIXED;
        else if (!strcmp(a, "--speed")) g_eng.speed = atof(v);
        else if (!strcmp(a, "--load")) g_eng.load = atoi(v);
        else if (!strcmp(a, "--ai-cost-ms")) g_eng.ai_cost_ms = atof(v);
        else if (!strcmp(a, "--window-ms")) g_eng.window_ms = atoi(v);
        else if (!strcmp(a, "--cpu")) *cpu = atoi(v);
        else if (!strcmp(a, "--duration")) g_eng.duration_s = atof(v);
        else if (!strcmp(a, "--csv-prefix")) g_eng.csv_prefix = v;
        else if (!strcmp(a, "--set")) { if (add_override(v)) { usage(argv[0]); return -1; } }
        else if (!strcmp(a, "--risk")) { if (add_risk(v)) { usage(argv[0]); return -1; } }
        else { usage(argv[0]); return -1; }
    }
    if (g_eng.speed <= 0 || g_eng.load < 1 || g_eng.window_ms < 0 || g_eng.ai_cost_ms < 0) {
        fprintf(stderr, "parametre invalide (speed > 0, load >= 1, window >= 0, ai-cost >= 0)\n");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ taches */

static void define_tasks(void) {
    static const rt_task_def_t defs[] = {
        { "T0",  "acquisition des evenements",   "haute",    RT_SPORADIC,   0,  10, PRIO_T0,    0 },
        { "T1",  "surveillance des vols",        "haute",    RT_PERIODIC, 100, 100, PRIO_T1,    0 },
        { "T2",  "surveillance des files",       "moyenne",  RT_PERIODIC, 500, 500, PRIO_T2,    1 },
        { "ALR", "alerte porte indisponible",    "critique", RT_SPORADIC,   0,  50, PRIO_ALERT, 0 },
        { "T3",  "prediction IA",                "haute",    RT_SPORADIC,   0, 200, PRIO_T3,    0 },
        { "T4",  "decision et ressources",       "haute",    RT_SPORADIC,   0, 100, PRIO_T4,    0 },
        { "BG",  "analyse historique (fond)",    "basse",    RT_APERIODIC,  0,   0, 0, NICE_BACKGROUND },
    };
    rt_task_t *slots[] = { &g_eng.t_acq, &g_eng.t_mon, &g_eng.t_pax, &g_eng.t_alert, &g_eng.t_ai,
                           &g_eng.t_dec, &g_eng.t_bg };
    for (int i = 0; i < (int)(sizeof slots / sizeof slots[0]); i++) rt_task_init(slots[i], &defs[i]);
}

static msg_queue_t *queue_at(int i) {
    msg_queue_t *qs[NB_QUEUES] = { &g_eng.q_flight, &g_eng.q_ground, &g_eng.q_alert, &g_eng.q_ai, &g_eng.q_decision };
    return qs[i];
}

static int all_quiet(void) {
    if (!atomic_load(&g_eng.acq_done)) return 0;
    for (int i = 0; i < NB_QUEUES; i++) if (mq_size(queue_at(i))) return 0;
    pthread_mutex_lock(&g_eng.mu);
    int pending = 0;
    for (int i = 0; i < g_eng.nfl; i++) if (!g_eng.fl[i].done) pending = 1;
    pthread_mutex_unlock(&g_eng.mu);
    return !pending;
}

static void wait_end(void) {
    uint64_t limit = g_eng.t0 + (uint64_t)(g_eng.duration_s * (double)NS_PER_S), quiet_since = 0;
    while (mono_ns() < limit) {
        usleep(POLL_MS * 1000);
        if (!all_quiet()) { quiet_since = 0; continue; }
        if (!quiet_since) quiet_since = mono_ns();
        else if (mono_ns() - quiet_since > QUIET_END_MS * NS_PER_MS) return;
    }
    eng_log("---", "duree maximale atteinte (%.0f s)", g_eng.duration_s);
}

static void print_report(rt_task_t *const *tasks, int n) {
    printf("\n================ RESULTATS (politique %s) ================\n",
           g_eng.policy == POLICY_AI ? "IA : base + min(3, risque x 3)" : "fixe : ordre d'arrivee");
    printf("Ordonnancement : %s\n\n", rt_sched_mode());
    rt_task_print_table(stdout, tasks, n);
    printf("\n");
    flights_print_summary(stdout);
    printf("\nAnalyse de fond (aperiodique, priorite la plus basse) : %ld tranches de 5 ms executees\n",
           g_eng.bg_chunks);
    if (g_eng.csv_prefix) {
        char p1[256], p2[256];
        snprintf(p1, sizeof p1, "%s_tasks.csv", g_eng.csv_prefix);
        snprintf(p2, sizeof p2, "%s_flights.csv", g_eng.csv_prefix);
        if (!rt_task_write_csv(p1, g_eng.policy == POLICY_AI ? "ai" : "fixed", tasks, n) && !flights_write_csv(p2))
            printf("CSV : %s, %s\n", p1, p2);
    }
}

static void init_engine(void) {
    g_eng.scenario = "scenarios/demo_tu701.txt";
    g_eng.policy = POLICY_AI;
    g_eng.speed = 1.0;
    g_eng.load = 1;
    g_eng.window_ms = 20;
    g_eng.ai_cost_ms = 4.0;       /* inference moyenne mesuree par M2 : 3,98 ms */
    g_eng.duration_s = 30.0;
    g_eng.background = 1;
    g_eng.sec_slow = 1.0;
    g_log_tty = isatty(STDOUT_FILENO);
    if (!g_log_tty) setvbuf(stdout, g_log_buf, _IOFBF, sizeof g_log_buf);
    rt_mutex_init_pi(&g_logmu);
    rt_mutex_init_pi(&g_eng.mu);   /* heritage de priorite : etat partage entre taches de priorites differentes */
    for (int i = 0; i < NB_QUEUES; i++) mq_init(queue_at(i));
    resources_init();
    define_tasks();
}

int main(int argc, char **argv) {
    int want_fifo = 1, cpu = 0;
    init_engine();
    if (parse_args(argc, argv, &want_fifo, &cpu)) return 1;
    if (acq_load(g_eng.scenario)) return 1;
    rt_sched_setup(want_fifo, cpu);
    g_eng.t0 = mono_ns() + START_OFFSET_MS * NS_PER_MS;
    printf("rt_engine : scenario %s (%d evenements, charge x%d), politique %s\n%s\n\n", g_eng.scenario,
           acq_count(), g_eng.load, g_eng.policy == POLICY_AI ? "IA" : "fixe", rt_sched_mode());

    /* T1 et T2 sont mis en phase avec l'instant 0 du scenario : mesures reproductibles.
     * (Sans cette phase, la latence de detection d'un retard vaut entre 0 et T1 = 100 ms.) */
    static rt_periodic_t p_mon = { &g_eng.t_mon, mon_body, NULL, &g_eng.stop, 0 };
    static rt_periodic_t p_pax = { &g_eng.t_pax, pax_body, NULL, &g_eng.stop, 0 };
    p_mon.first_release_ns = g_eng.t0;
    p_pax.first_release_ns = g_eng.t0;
    pthread_t th[7]; int nth = 0;
    /* les consommateurs d'abord, le producteur T0 en dernier */
    if (!rt_thread_start(&th[nth], PRIO_ALERT, 0, alert_main, NULL)) nth++;
    if (!rt_thread_start(&th[nth], PRIO_T3, 0, ai_main, NULL)) nth++;
    if (!rt_thread_start(&th[nth], PRIO_T4, 0, decision_main, NULL)) nth++;
    if (!rt_thread_start(&th[nth], PRIO_T1, 0, rt_periodic_main, &p_mon)) nth++;
    if (!rt_thread_start(&th[nth], PRIO_T2, 1, rt_periodic_main, &p_pax)) nth++;
    if (g_eng.background && !rt_thread_start(&th[nth], 0, NICE_BACKGROUND, background_main, NULL)) nth++;
    if (rt_thread_start(&th[nth], PRIO_T0, 0, acq_main, NULL)) { fprintf(stderr, "T0 impossible\n"); return 1; }
    nth++;

    wait_end();
    atomic_store(&g_eng.stop, 1);
    for (int i = 0; i < NB_QUEUES; i++) mq_close(queue_at(i));
    for (int i = 0; i < nth; i++) pthread_join(th[i], NULL);
    flights_join_all();

    rt_task_t *tasks[] = { &g_eng.t_acq, &g_eng.t_alert, &g_eng.t_mon, &g_eng.t_dec, &g_eng.t_ai,
                           &g_eng.t_pax, &g_eng.t_bg };
    print_report(tasks, 7);
    return 0;
}
