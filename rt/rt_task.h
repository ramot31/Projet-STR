/* rt_task.h - Tache temps reel (M3) : ses parametres (role, C, T, D, criticite), ses mesures,
 * le moteur de taches periodiques et la creation de threads avec priorite temps reel.
 *
 * Mesures par activation :
 *   C = temps CPU consomme (CLOCK_THREAD_CPUTIME_ID)
 *   R = fin - liberation (temps de reponse : attente + execution) ; faute si R > D
 *   J = debut - liberation (jitter / latence d'activation)
 * Priorites : SCHED_FIFO si le processus en a le droit (root ou CAP_SYS_NICE), sinon repli documente
 * sur SCHED_OTHER + nice (ordre "proportionnel" et non strict). */
#ifndef RT_TASK_H
#define RT_TASK_H
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

typedef enum { RT_PERIODIC = 0, RT_SPORADIC, RT_APERIODIC } rt_kind_t;

typedef struct {
    const char *name, *role, *criticity;
    rt_kind_t kind;
    int period_ms;     /* 0 si non periodique */
    int deadline_ms;   /* 0 = pas de deadline */
    int fifo_prio;     /* 1..99 ; 0 = SCHED_OTHER */
    int nice;          /* utilise seulement en repli */
} rt_task_def_t;

typedef struct {
    rt_task_def_t def;
    pthread_mutex_t mu;
    long n, misses;
    uint64_t c_sum, c_max, r_sum, r_max, j_sum, j_max;   /* nanosecondes */
} rt_task_t;

void rt_task_init(rt_task_t *t, const rt_task_def_t *def);
/* Enregistre une activation ; retourne 1 si la deadline est depassee. */
int  rt_task_record(rt_task_t *t, uint64_t release_ns, uint64_t start_ns, uint64_t end_ns, uint64_t cpu_ns);
void rt_task_print_table(FILE *out, rt_task_t *const *tasks, int n);
int  rt_task_write_csv(const char *path, const char *policy, rt_task_t *const *tasks, int n);

/* Moteur periodique : reveil a dates absolues (clock_nanosleep TIMER_ABSTIME), mesure de chaque
 * activation. A passer a pthread_create / rt_thread_start. */
typedef void (*rt_body_fn)(void *arg);
/* first_release_ns : date de la 1re activation (phase) ; 0 = maintenant + une periode. */
typedef struct { rt_task_t *task; rt_body_fn body; void *arg; atomic_int *stop; uint64_t first_release_ns; } rt_periodic_t;
void *rt_periodic_main(void *periodic);

/* Configuration de l'ordonnancement (une fois, au demarrage). cpu >= 0 : tous les threads crees par
 * rt_thread_start sont places sur ce processeur (hypothese monoprocesseur de l'etude RM/DM/EDF). */
int         rt_sched_setup(int want_fifo, int cpu);
int         rt_sched_fifo_active(void);
const char *rt_sched_mode(void);
/* Cree un thread qui applique lui-meme sa priorite (FIFO fifo_prio, ou nice en repli) puis appelle fn. */
int         rt_thread_start(pthread_t *th, int fifo_prio, int nice, void *(*fn)(void *), void *arg);
/* Mutex a heritage de priorite (evite l'inversion de priorite quand une tache forte attend une faible). */
void        rt_mutex_init_pi(pthread_mutex_t *mu);
#endif
