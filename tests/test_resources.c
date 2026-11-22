/* test_resources.c - Tests de comportement de resources.c et msg_queue.c (M4).
 * Compile sans NDEBUG : les assert sont les verifications. */
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../node_p/msg_queue.h"
#include "../node_p/resources.h"

static void test_gates_are_given_nearest_first_then_time_out(void) {
    char g[GATE_NAME_LEN];
    resources_init();
    assert(gate_reserve("TU701", 0, g) == RES_OK && !strcmp(g, "G12"));
    assert(gate_reserve("TU702", 0, g) == RES_OK && !strcmp(g, "G15"));
    assert(gate_reserve("TU703", 0, g) == RES_OK && !strcmp(g, "G18"));
    assert(gate_reserve("TU704", 50, g) == RES_TIMEOUT);
    assert(gate_release("G15") == RES_OK);
    assert(gate_reserve("TU704", 50, g) == RES_OK && !strcmp(g, "G15"));
}

static void test_unavailable_gate_is_never_given(void) {
    char g[GATE_NAME_LEN];
    resources_init();
    assert(gate_set_unavailable("G12") == RES_OK);
    assert(gates_free_count() == 2);
    assert(gate_reserve("TU701", 0, g) == RES_OK && !strcmp(g, "G15"));   /* scenario de l'enonce */
    assert(gate_reserve("TU702", 0, g) == RES_OK && !strcmp(g, "G18"));
    assert(gate_set_unavailable("G12") == RES_INVALID);
    assert(gate_release("G12") == RES_INVALID);
}

static void test_busy_gate_going_down_leaves_service_on_release(void) {
    char g[GATE_NAME_LEN];
    resources_init();
    assert(gate_reserve("TU701", 0, g) == RES_OK && !strcmp(g, "G12"));
    assert(gate_set_unavailable("G12") == RES_OK);
    assert(gates_free_count() == 2);
    assert(gate_release("G12") == RES_OK);
    assert(gates_free_count() == 2);                      /* pas de permis fantome apres liberation */
    assert(gate_reserve("TU702", 0, g) == RES_OK && !strcmp(g, "G15"));
    assert(gate_reserve("TU703", 0, g) == RES_OK && !strcmp(g, "G18"));
    assert(gate_reserve("TU704", 30, g) == RES_TIMEOUT);
}

static void test_invalid_arguments_and_double_release_are_rejected(void) {
    char g[GATE_NAME_LEN]; int b;
    resources_init();
    assert(gate_reserve(NULL, 0, g) == RES_INVALID);
    assert(gate_release("G99") == RES_INVALID && gate_release(NULL) == RES_INVALID);
    assert(gate_release("G12") == RES_INVALID);
    assert(bus_reserve(0, &b) == RES_OK);
    assert(bus_release(b) == RES_OK);
    assert(bus_release(b) == RES_INVALID);                /* double liberation : ne gonfle pas le semaphore */
    assert(bus_release(0) == RES_INVALID && bus_release(NB_BUSES + 1) == RES_INVALID);
    assert(buses_free_count() == NB_BUSES);
}

static void test_bus_breakdown_reduces_capacity_five_to_four(void) {
    int ids[NB_BUSES], b;
    resources_init();
    assert(buses_capacity() == 5 && buses_free_count() == 5);
    assert(bus_set_capacity(4) == 4);
    assert(buses_free_count() == 4);
    for (int i = 0; i < 4; i++) assert(bus_reserve(0, &ids[i]) == RES_OK);
    assert(bus_reserve(30, &b) == RES_TIMEOUT);
    assert(bus_set_capacity(4) == 4);
    assert(bus_set_capacity(9) == 4);                      /* pas de hausse sans reparation */
}

static void test_breakdown_of_a_busy_bus_takes_effect_on_release(void) {
    int ids[NB_BUSES], b;
    resources_init();
    for (int i = 0; i < NB_BUSES; i++) assert(bus_reserve(0, &ids[i]) == RES_OK);
    assert(bus_set_capacity(4) == 4);
    /* l'un des cinq est en panne : apres 5 liberations valides il reste exactement 4 bus libres */
    for (int i = 0; i < NB_BUSES; i++) assert(bus_release(ids[i]) == RES_OK);
    assert(buses_free_count() == 4);
    int got = 0; while (bus_reserve(0, &b) == RES_OK) got++;
    assert(got == 4);
}

static void *wait_for_bus(void *arg) {
    int *out = arg;
    *out = bus_reserve(2000, out + 1);
    return NULL;
}
static void test_waiting_thread_is_woken_by_a_release(void) {
    int ids[NB_BUSES], res[2] = { 99, 0 };
    resources_init();
    for (int i = 0; i < NB_BUSES; i++) assert(bus_reserve(0, &ids[i]) == RES_OK);
    pthread_t t; pthread_create(&t, NULL, wait_for_bus, res);
    usleep(100000);
    assert(res[0] == 99);
    assert(bus_release(ids[2]) == RES_OK);
    pthread_join(t, NULL);
    assert(res[0] == RES_OK && res[1] == ids[2]);
}

#define STRESS_THREADS 16
#ifndef STRESS_ITERS
#define STRESS_ITERS   3000
#endif
static atomic_int g_holders[NB_BUSES + 1], g_in_use, g_max_in_use, g_violations, g_completed;

static void *stress(void *arg) {
    unsigned seed = (unsigned)(uintptr_t)arg;
    for (int i = 0; i < STRESS_ITERS; i++) {
        int b;
        if (bus_reserve(-1, &b) != RES_OK) { if (buses_capacity() > 0) g_violations++; continue; }
        if (atomic_fetch_add(&g_holders[b], 1) != 0) g_violations++;
        int now = atomic_fetch_add(&g_in_use, 1) + 1, prev = atomic_load(&g_max_in_use);
        while (now > prev && !atomic_compare_exchange_weak(&g_max_in_use, &prev, now)) { }
        if (rand_r(&seed) % 4 == 0) sched_yield();
        atomic_fetch_sub(&g_in_use, 1);
        atomic_fetch_sub(&g_holders[b], 1);
        bus_release(b);
        atomic_fetch_add(&g_completed, 1);
    }
    return NULL;
}
/* La panne survient au tiers du travail, donc toujours en pleine contention, quelle que soit la vitesse de la machine */
static void *breaker(void *arg) {
    (void)arg;
    while (atomic_load(&g_completed) < STRESS_THREADS * STRESS_ITERS / 3) sched_yield();
    bus_set_capacity(4);
    return NULL;
}

static void test_buses_stay_exclusive_under_contention_and_breakdown(void) {
    pthread_t t[STRESS_THREADS + 1];
    resources_init();
    for (int i = 0; i < STRESS_THREADS; i++) pthread_create(&t[i], NULL, stress, (void *)(uintptr_t)(i + 1));
    pthread_create(&t[STRESS_THREADS], NULL, breaker, NULL);
    for (int i = 0; i <= STRESS_THREADS; i++) pthread_join(t[i], NULL);
    assert(atomic_load(&g_violations) == 0);
    assert(atomic_load(&g_max_in_use) <= NB_BUSES);
    assert(buses_capacity() == 4 && buses_free_count() == 4);     /* semaphore coherent avec la table d'etat */
    int b, got = 0; while (bus_reserve(0, &b) == RES_OK) got++;
    assert(got == 4);
}

static msg_queue_t q;
static void *consumer(void *arg) {
    int *count = arg; message_t m, prev = { .id = 0 };
    while (mq_pop(&q, &m) == 0) { assert(m.id == prev.id + 1); prev = m; (*count)++; }
    return NULL;
}
static void test_queue_is_fifo_bounded_and_wakes_consumers_on_close(void) {
    int count = 0; message_t m = { .type = EV_ACK };
    mq_init(&q);
    pthread_t c; pthread_create(&c, NULL, consumer, &count);
    for (int i = 1; i <= 1000; i++) { m.id = (uint64_t)i; assert(mq_push(&q, &m, -1) == 0); }
    mq_close(&q); pthread_join(c, NULL);
    assert(count == 1000);
    assert(mq_push(&q, &m, 0) == -1);
    mq_destroy(&q);

    mq_init(&q);
    for (int i = 0; i < MQ_CAPACITY; i++) assert(mq_push(&q, &m, 0) == 0);
    assert(mq_push(&q, &m, 30) == -1 && mq_size(&q) == MQ_CAPACITY);
    mq_destroy(&q);
}

#define RUN(t) do { t(); printf("ok - %s\n", #t); fflush(stdout); } while (0)

int main(void) {
    RUN(test_gates_are_given_nearest_first_then_time_out);
    RUN(test_unavailable_gate_is_never_given);
    RUN(test_busy_gate_going_down_leaves_service_on_release);
    RUN(test_invalid_arguments_and_double_release_are_rejected);
    RUN(test_bus_breakdown_reduces_capacity_five_to_four);
    RUN(test_breakdown_of_a_busy_bus_takes_effect_on_release);
    RUN(test_waiting_thread_is_woken_by_a_release);
    RUN(test_buses_stay_exclusive_under_contention_and_breakdown);
    RUN(test_queue_is_fifo_bounded_and_wakes_consumers_on_close);
    printf("test_resources OK\n");
    return 0;
}
