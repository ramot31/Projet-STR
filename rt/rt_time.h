/* rt_time.h - Outils de temps du module temps reel (M3).
 * Toutes les mesures utilisent CLOCK_MONOTONIC : elle ne recule jamais (contrairement a CLOCK_REALTIME
 * qui peut etre corrigee par NTP), ce qui est indispensable pour mesurer des durees et des echeances. */
#ifndef RT_TIME_H
#define RT_TIME_H
#include <errno.h>
#include <stdint.h>
#include <time.h>

#define NS_PER_MS 1000000ULL
#define NS_PER_S  1000000000ULL

static inline uint64_t ts_to_ns(const struct timespec *t) {
    return (uint64_t)t->tv_sec * NS_PER_S + (uint64_t)t->tv_nsec;
}

static inline struct timespec ns_to_ts(uint64_t ns) {
    struct timespec t;
    t.tv_sec = (time_t)(ns / NS_PER_S);
    t.tv_nsec = (long)(ns % NS_PER_S);
    return t;
}

static inline uint64_t mono_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return ts_to_ns(&t);
}

/* Temps CPU consomme par le thread appelant : n'avance pas quand le thread est preempte.
 * C'est la bonne mesure de C (temps d'execution), distincte du temps de reponse R. */
static inline uint64_t thread_cpu_ns(void) {
    struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return ts_to_ns(&t);
}

/* Dort JUSQU'A la date absolue ns (TIMER_ABSTIME) : la date de reveil ne depend pas de l'heure a
 * laquelle on s'endort, donc les retards ne s'accumulent pas (pas de derive). */
static inline void sleep_until_ns(uint64_t ns) {
    struct timespec t = ns_to_ts(ns);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == EINTR) { }
}

/* Consomme ms millisecondes de CPU : simule le calcul reel d'une tache (son C).
 * Base sur le temps CPU du thread : si le thread est preempte, l'attente ne compte pas. */
static inline void burn_cpu_ms(double ms) {
    uint64_t end = thread_cpu_ns() + (uint64_t)(ms * (double)NS_PER_MS);
    while (thread_cpu_ns() < end) { }
}
#endif
