/* test_rt.c - Tests unitaires du module temps reel (M3) :
 *   1. politique IA de M5 traduite en priorites de threads (seuils, boost plafonne, bandes protegees) ;
 *   2. mesures d'une tache (C, R, jitter, depassement de deadline) ;
 *   3. moteur periodique : clock_nanosleep en temps absolu => pas de derive. */
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include "ai_policy.h"
#include "rt_task.h"
#include "rt_time.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("  ECHEC ligne %d : %s\n", __LINE__, #c); g_fail++; } } while (0)
#define NEAR(a, b) ((a) - (b) < 1e-9 && (b) - (a) < 1e-9)

static void test_levels(void) {
    CHECK(m5_level(0.0) == RISK_LOW);
    CHECK(m5_level(0.3499) == RISK_LOW);
    CHECK(m5_level(0.35) == RISK_MEDIUM);      /* frontiere incluse dans "moyen" */
    CHECK(m5_level(0.8499) == RISK_MEDIUM);
    CHECK(m5_level(0.85) == RISK_CRITICAL);    /* frontiere incluse dans "critique" */
    CHECK(m5_level(1.0) == RISK_CRITICAL);
    CHECK(m5_level(-1.0) == RISK_LOW);         /* valeur invalide -> bornee */
    CHECK(m5_level(2.0) == RISK_CRITICAL);
    CHECK(m5_level(0.0 / 0.0) == RISK_LOW);    /* NaN = IA indisponible = risque 0 */
}

static void test_boost(void) {
    CHECK(NEAR(m5_boost(0.87, POLICY_FIXED), 0.0));   /* priorites fixes : l'IA est ignoree */
    CHECK(NEAR(m5_boost(0.87, POLICY_AI), 2.61));
    CHECK(NEAR(m5_boost(0.20, POLICY_AI), 0.60));
    CHECK(NEAR(m5_boost(1.50, POLICY_AI), M5_MAX_BOOST)); /* plafond */
    CHECK(NEAR(m5_boost(-0.5, POLICY_AI), 0.0));
}

static void test_job_priorities(void) {
    /* IA : plus le risque est grand, plus la priorite FIFO est haute et le nice bas */
    int p701 = job_fifo_prio(POLICY_AI, m5_boost(0.87, POLICY_AI), 2);
    int p703 = job_fifo_prio(POLICY_AI, m5_boost(0.65, POLICY_AI), 1);
    int p702 = job_fifo_prio(POLICY_AI, m5_boost(0.20, POLICY_AI), 0);
    CHECK(p701 > p703 && p703 > p702);
    CHECK(job_nice(POLICY_AI, m5_boost(0.87, POLICY_AI), 2) < job_nice(POLICY_AI, m5_boost(0.20, POLICY_AI), 0));
    /* fixe : ordre d'arrivee, le risque n'intervient pas */
    CHECK(job_fifo_prio(POLICY_FIXED, 0.0, 0) > job_fifo_prio(POLICY_FIXED, 0.0, 1));
    CHECK(job_nice(POLICY_FIXED, 0.0, 0) < job_nice(POLICY_FIXED, 0.0, 1));
    /* garde-fou : aucun travail de vol, meme boost au maximum, n'atteint une tache critique */
    for (int rank = 0; rank < 200; rank++) {
        int pa = job_fifo_prio(POLICY_AI, M5_MAX_BOOST, rank), pf = job_fifo_prio(POLICY_FIXED, 0.0, rank);
        CHECK(pa >= JOB_FIFO_MIN && pa <= JOB_FIFO_MAX && pa < PRIO_T2);
        CHECK(pf >= JOB_FIFO_MIN && pf <= JOB_FIFO_MAX);
        CHECK(job_nice(POLICY_AI, M5_MAX_BOOST, rank) >= JOB_NICE_BEST);
        CHECK(job_nice(POLICY_FIXED, 0.0, rank) <= JOB_NICE_WORST);
    }
}

static void test_record(void) {
    rt_task_t t;
    rt_task_def_t d = { "T4", "test", "haute", RT_SPORADIC, 0, 100, 80, 0 };
    rt_task_init(&t, &d);
    uint64_t ms = NS_PER_MS;
    CHECK(rt_task_record(&t, 0, 2 * ms, 30 * ms, 5 * ms) == 0);      /* R = 30 <= 100 */
    CHECK(rt_task_record(&t, 0, 10 * ms, 101 * ms, 7 * ms) == 1);    /* R = 101 > 100 : faute */
    CHECK(rt_task_record(&t, 5 * ms, 4 * ms, 6 * ms, 1 * ms) == 0);  /* debut avant liberation : jitter 0 */
    CHECK(t.n == 3 && t.misses == 1);
    CHECK(t.c_max == 7 * ms && t.c_sum == 13 * ms);
    CHECK(t.r_max == 101 * ms);
    CHECK(t.j_max == 10 * ms && t.j_sum == 12 * ms);
}

static int g_ticks;
static uint64_t g_first, g_last;
static atomic_int g_stop_flag;
static void tick_body(void *arg) {
    (void)arg; uint64_t now = mono_ns();
    if (!g_ticks) g_first = now;
    g_last = now;
    if (++g_ticks >= 20) atomic_store(&g_stop_flag, 1);
}

static void test_periodic_no_drift(void) {
    rt_task_t t;
    rt_task_def_t d = { "T", "test periodique", "basse", RT_PERIODIC, 10, 10, 0, 0 };
    rt_task_init(&t, &d);
    rt_periodic_t p = { &t, tick_body, NULL, &g_stop_flag, 0 };
    rt_periodic_main(&p);
    /* 20 activations a 10 ms : 19 periodes entre la 1re et la 20e. Un sleep relatif accumulerait
     * le temps d'execution + la latence de reveil a chaque tour ; ici l'ecart reste borne. */
    double span_ms = (double)(g_last - g_first) / NS_PER_MS;
    printf("  periodique : 20 activations, ecart 1re->20e = %.2f ms (attendu 190), jitter max = %.3f ms\n",
           span_ms, (double)t.j_max / NS_PER_MS);
    CHECK(t.n == 20);
    CHECK(span_ms > 185.0 && span_ms < 200.0);
}

int main(void) {
    printf("test_rt : politique IA, mesures, moteur periodique\n");
    test_levels();
    test_boost();
    test_job_priorities();
    test_record();
    test_periodic_no_drift();
    if (g_fail) { printf("test_rt : %d ECHEC(S)\n", g_fail); return 1; }
    printf("test_rt : OK\n");
    return 0;
}
