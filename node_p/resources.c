/* resources.c - Voir resources.h pour le modele. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "resources.h"

#define MAX_SLOTS 8
#define OWNER_LEN 8

/* Fenetre artificielle entre "j'ai trouve une unite libre" et "je la marque occupee".
 * Uniquement pour tests/race_demo.c : elle prouve que la protection tient meme quand le
 * parcours dure longtemps. Vide en production. */
#ifdef RES_TEST_WINDOW_US
#define CRITICAL_WINDOW() usleep(RES_TEST_WINDOW_US)
#else
#define CRITICAL_WINDOW() ((void)0)
#endif

typedef enum {
    SLOT_FREE,
    SLOT_BUSY,
    SLOT_BUSY_FAILED,   /* en panne alors qu'occupee : sort du service a la liberation */
    SLOT_FAILED
} slot_state_t;

/* Invariant : valeur du semaphore = nombre de SLOT_FREE, aux permis "en vol" pres (voir pool_acquire). */
typedef struct {
    sem_t           permits;
    pthread_mutex_t mu;
    int             n;
    slot_state_t    state[MAX_SLOTS];
    char            owner[MAX_SLOTS][OWNER_LEN];
} pool_t;

static pool_t g_gates, g_buses;
static int    g_ready;

static const struct { const char *name; int distance_m; } GATE_DEF[NB_GATES] = {
    /* ordre = distance croissante : "premiere porte libre" = "porte la plus proche" */
    { "G12", 400 }, { "G15", 450 }, { "G18", 500 } };

static struct timespec deadline_after_ms(int ms) {
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);   /* sem_timedwait exige CLOCK_REALTIME */
    t.tv_sec += ms / 1000; t.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    return t;
}

static void pool_init(pool_t *p, int n) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    /* glibc declare PTHREAD_PRIO_INHERIT en enum : seule la macro d'option POSIX est testable. */
#if defined(_POSIX_THREAD_PRIO_INHERIT) && _POSIX_THREAD_PRIO_INHERIT > 0
    pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
#endif
    pthread_mutex_init(&p->mu, &a); pthread_mutexattr_destroy(&a);
    sem_init(&p->permits, 0, (unsigned)n);
    p->n = n;
    for (int i = 0; i < MAX_SLOTS; i++) { p->state[i] = i < n ? SLOT_FREE : SLOT_FAILED; p->owner[i][0] = 0; }
}

static void pool_destroy(pool_t *p) { sem_destroy(&p->permits); pthread_mutex_destroy(&p->mu); }

static int wait_permit(pool_t *p, int timeout_ms, const struct timespec *deadline) {
    int rc;
    do {
        if (timeout_ms < 0) rc = sem_wait(&p->permits);
        else if (timeout_ms == 0) rc = sem_trywait(&p->permits);
        else rc = sem_timedwait(&p->permits, deadline);
    } while (rc != 0 && errno == EINTR);
    return rc == 0 ? 0 : -1;
}

static int pool_acquire(pool_t *p, int timeout_ms, const char *owner, int *idx) {
    struct timespec deadline = { 0, 0 };
    if (timeout_ms > 0) deadline = deadline_after_ms(timeout_ms);
    for (;;) {
        if (wait_permit(p, timeout_ms, &deadline)) return RES_TIMEOUT;
        pthread_mutex_lock(&p->mu);
        for (int i = 0; i < p->n; i++) if (p->state[i] == SLOT_FREE) {
            CRITICAL_WINDOW();
            p->state[i] = SLOT_BUSY;
            if (owner) snprintf(p->owner[i], OWNER_LEN, "%s", owner);
            pthread_mutex_unlock(&p->mu);
            *idx = i; return RES_OK;
        }
        pthread_mutex_unlock(&p->mu);
        /* Permis orphelin : une panne a retire une unite libre pendant que ce thread attendait le mutex
         * (voir pool_fail). Ce permis payait cette unite : on l'ecarte et on reattend, delai conserve. */
    }
}

static int pool_release(pool_t *p, int idx) {
    int rc = RES_OK;
    pthread_mutex_lock(&p->mu);
    if (idx < 0 || idx >= p->n) rc = RES_INVALID;
    else if (p->state[idx] == SLOT_BUSY) {
        p->state[idx] = SLOT_FREE; p->owner[idx][0] = 0;
        sem_post(&p->permits);
    } else if (p->state[idx] == SLOT_BUSY_FAILED) {
        p->state[idx] = SLOT_FAILED; p->owner[idx][0] = 0;   /* pas de sem_post : la capacite baisse */
    } else rc = RES_INVALID;
    pthread_mutex_unlock(&p->mu);
    return rc;
}

static int fail_locked(pool_t *p, int idx) {
    if (p->state[idx] == SLOT_FREE) {
        /* Retire son permis s'il est encore au compteur. Sinon un thread l'a deja pris et attend le mutex :
         * il constatera qu'aucune unite libre ne lui correspond et ecartera ce permis (pool_acquire). */
        sem_trywait(&p->permits);
        p->state[idx] = SLOT_FAILED;
        return RES_OK;
    }
    if (p->state[idx] == SLOT_BUSY) { p->state[idx] = SLOT_BUSY_FAILED; return RES_OK; }
    return RES_INVALID;
}

static int pool_fail(pool_t *p, int idx) {
    int rc;
    pthread_mutex_lock(&p->mu);
    rc = (idx < 0 || idx >= p->n) ? RES_INVALID : fail_locked(p, idx);
    pthread_mutex_unlock(&p->mu);
    return rc;
}

/* Panne d'une unite quelconque : une libre en priorite (aucun utilisateur n'est penalise). */
static int pool_fail_any(pool_t *p) {
    int found = RES_INVALID;
    pthread_mutex_lock(&p->mu);
    for (int i = 0; i < p->n && found < 0; i++) if (p->state[i] == SLOT_FREE) { fail_locked(p, i); found = i; }
    for (int i = 0; i < p->n && found < 0; i++) if (p->state[i] == SLOT_BUSY) { fail_locked(p, i); found = i; }
    pthread_mutex_unlock(&p->mu);
    return found;
}

static int pool_count(pool_t *p, int free_only) {
    int c = 0;
    pthread_mutex_lock(&p->mu);
    for (int i = 0; i < p->n; i++)
        if (p->state[i] == SLOT_FREE || (!free_only && p->state[i] == SLOT_BUSY)) c++;
    pthread_mutex_unlock(&p->mu);
    return c;
}

void resources_init(void) {
    if (g_ready) { pool_destroy(&g_gates); pool_destroy(&g_buses); }
    pool_init(&g_gates, NB_GATES);
    pool_init(&g_buses, NB_BUSES);
    g_ready = 1;
}

static int gate_index(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < NB_GATES; i++) if (!strcmp(GATE_DEF[i].name, name)) return i;
    return -1;
}

int gate_reserve(const char *flight, int timeout_ms, char gate_out[GATE_NAME_LEN]) {
    int idx, rc;
    if (!flight || !gate_out) return RES_INVALID;
    rc = pool_acquire(&g_gates, timeout_ms, flight, &idx);
    if (rc == RES_OK) snprintf(gate_out, GATE_NAME_LEN, "%s", GATE_DEF[idx].name);
    return rc;
}
int gate_release(const char *gate) {
    int idx = gate_index(gate);
    return idx < 0 ? RES_INVALID : pool_release(&g_gates, idx);
}
int gate_set_unavailable(const char *gate) {
    int idx = gate_index(gate);
    return idx < 0 ? RES_INVALID : pool_fail(&g_gates, idx);
}
int gates_free_count(void) { return pool_count(&g_gates, 1); }

int bus_reserve(int timeout_ms, int *bus_id) {
    int idx, rc;
    if (!bus_id) return RES_INVALID;
    rc = pool_acquire(&g_buses, timeout_ms, NULL, &idx);
    if (rc == RES_OK) *bus_id = idx + 1;
    return rc;
}
int bus_release(int bus_id) { return pool_release(&g_buses, bus_id - 1); }
int bus_breakdown(void) { int i = pool_fail_any(&g_buses); return i < 0 ? RES_INVALID : i + 1; }
int bus_set_capacity(int target) {
    if (target < 0) target = 0;
    while (pool_count(&g_buses, 0) > target && bus_breakdown() > 0) { }
    return pool_count(&g_buses, 0);
}
int buses_free_count(void) { return pool_count(&g_buses, 1); }
int buses_capacity(void) { return pool_count(&g_buses, 0); }
