/* race_demo.c - Demonstration experimentale obligatoire de M4 (enonce, milestone 4) :
 *   TU701 et TU702 demandent une porte "en meme temps" ; G12 est indisponible, G15 et G18 sont libres.
 *   (1) SANS synchronisation : les deux vols peuvent obtenir G15.
 *   (2) AVEC synchronisation (semaphore + mutex de resources.c) : TU701 -> G15, TU702 -> G18.
 *   (3) On repete N fois (1000 par defaut) et on compte les conflits : N contre 0.
 *
 * Le code "sans synchronisation" est un allocateur naif ecrit ici (verifier puis marquer, sans verrou),
 * c'est exactement l'erreur que decrit l'enonce. Le code "avec synchronisation" est le vrai resources.c,
 * compile avec RES_TEST_WINDOW_US : meme fenetre de 100 us entre la verification et le marquage,
 * mais DANS la section critique -> si la protection est mauvaise, le conflit apparait.
 *
 * Usage : race_demo [--trials N] [--csv fichier] */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../node_p/resources.h"

#ifndef RES_TEST_WINDOW_US
#error "compiler avec -DRES_TEST_WINDOW_US=100 (voir Makefile)"
#endif

typedef struct { const char *flight; char gate[GATE_NAME_LEN]; } request_t;
typedef struct { request_t *req; int (*allocate)(const char *, char *); pthread_barrier_t *start; } job_t;

static gate_t g_naive[NB_GATES];
static void naive_init(void) {
    static const char *names[NB_GATES] = { "G12", "G15", "G18" };
    for (int i = 0; i < NB_GATES; i++) { memset(&g_naive[i], 0, sizeof g_naive[i]); snprintf(g_naive[i].name, 4, "%s", names[i]); }
    g_naive[0].state = GATE_UNAVAILABLE;
}
static int naive_reserve(const char *flight, char out[GATE_NAME_LEN]) {
    for (int i = 0; i < NB_GATES; i++) if (g_naive[i].state == GATE_FREE) {
        usleep(RES_TEST_WINDOW_US);
        g_naive[i].state = GATE_OCCUPIED;              /* trop tard : l'autre thread a deja choisi cette porte */
        snprintf(g_naive[i].flight, 8, "%s", flight); memcpy(out, g_naive[i].name, GATE_NAME_LEN);
        return 0;
    }
    return -1;
}
static int naive_reserve_no_window(const char *flight, char out[GATE_NAME_LEN]) {
    for (int i = 0; i < NB_GATES; i++) if (g_naive[i].state == GATE_FREE) {
        sched_yield();
        g_naive[i].state = GATE_OCCUPIED;
        snprintf(g_naive[i].flight, 8, "%s", flight); memcpy(out, g_naive[i].name, GATE_NAME_LEN);
        return 0;
    }
    return -1;
}

static int safe_reserve(const char *flight, char *out) { return gate_reserve(flight, 1000, out) == RES_OK ? 0 : -1; }

static void *run_job(void *arg) {
    job_t *j = arg;
    pthread_barrier_wait(j->start);
    if (j->allocate(j->req->flight, j->req->gate)) snprintf(j->req->gate, GATE_NAME_LEN, "--");
    return NULL;
}

static int one_trial(int (*allocate)(const char *, char *), request_t out[2]) {
    request_t r[2] = { { "TU701", "" }, { "TU702", "" } };
    pthread_barrier_t b; pthread_barrier_init(&b, NULL, 2);
    job_t jobs[2] = { { &r[0], allocate, &b }, { &r[1], allocate, &b } };
    pthread_t t[2];
    for (int i = 0; i < 2; i++) pthread_create(&t[i], NULL, run_job, &jobs[i]);
    for (int i = 0; i < 2; i++) pthread_join(t[i], NULL);
    pthread_barrier_destroy(&b);
    out[0] = r[0]; out[1] = r[1];
    return strcmp(r[0].gate, "--") && !strcmp(r[0].gate, r[1].gate);
}

static void reset_naive(void) { naive_init(); }
static void reset_safe(void) { resources_init(); gate_set_unavailable("G12"); }

typedef struct { const char *label; int (*allocate)(const char *, char *); void (*reset)(void); int conflicts; } mode_t_;

int main(int argc, char **argv) {
    int trials = 1000; const char *csv = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trials") && i + 1 < argc) trials = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) csv = argv[++i];
        else { fprintf(stderr, "usage: %s [--trials N] [--csv f]\n", argv[0]); return 1; }
    }
    if (trials < 1) return 1;
    mode_t_ modes[] = {
        { "sans synchronisation (fenetre naturelle)", naive_reserve_no_window, reset_naive, 0 },
        { "sans synchronisation (fenetre 100 us)",    naive_reserve,           reset_naive, 0 },
        { "avec semaphore + mutex (fenetre 100 us)",  safe_reserve,            reset_safe,  0 },
    };
    int nm = (int)(sizeof modes / sizeof modes[0]);

    printf("=== Scenario : G12 indisponible, G15 et G18 libres ; TU701 et TU702 demandent une porte ===\n");
    for (int m = 0; m < nm; m++) {
        request_t r[2]; int shown = 0;
        for (int i = 0; i < trials; i++) {
            modes[m].reset();
            int c = one_trial(modes[m].allocate, r);
            modes[m].conflicts += c;
            if (i == 0 || (c && !shown && m < 2)) {
                if (c) shown = 1;
                printf("  [%s] essai %d : TU701 -> %s, TU702 -> %s%s\n", m < 2 ? "SANS" : "AVEC", i + 1, r[0].gate, r[1].gate, c ? "   <-- CONFLIT (meme porte)" : "");
            }
        }
    }
    printf("\n%-46s %8s %10s %8s\n", "mode", "essais", "conflits", "taux");
    for (int m = 0; m < nm; m++)
        printf("%-46s %8d %10d %7.1f%%\n", modes[m].label, trials, modes[m].conflicts, 100.0 * modes[m].conflicts / trials);

    if (csv) {
        FILE *f = fopen(csv, "w");
        if (!f) { perror(csv); return 1; }
        fprintf(f, "mode,trials,conflicts\n");
        for (int m = 0; m < nm; m++) fprintf(f, "\"%s\",%d,%d\n", modes[m].label, trials, modes[m].conflicts);
        fclose(f); printf("CSV : %s\n", csv);
    }
    return modes[2].conflicts == 0 ? 0 : 2;
}
