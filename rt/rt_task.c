/* rt_task.c - Voir rt_task.h. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "rt_task.h"
#include "rt_time.h"

#define RT_STACK_SIZE (256 * 1024)

static int  g_fifo;
static int  g_cpu = -1;
static char g_mode[160] = "SCHED_OTHER (non configure)";

/* ------------------------------------------------------------------ mesures */

void rt_task_init(rt_task_t *t, const rt_task_def_t *def) {
    memset(t, 0, sizeof *t);
    t->def = *def;
    pthread_mutex_init(&t->mu, NULL);
}

int rt_task_record(rt_task_t *t, uint64_t release_ns, uint64_t start_ns, uint64_t end_ns, uint64_t cpu_ns) {
    uint64_t r = end_ns > release_ns ? end_ns - release_ns : 0;
    uint64_t j = start_ns > release_ns ? start_ns - release_ns : 0;
    int miss = t->def.deadline_ms > 0 && r > (uint64_t)t->def.deadline_ms * NS_PER_MS;
    pthread_mutex_lock(&t->mu);
    t->n++;
    t->misses += miss;
    t->c_sum += cpu_ns; if (cpu_ns > t->c_max) t->c_max = cpu_ns;
    t->r_sum += r;      if (r > t->r_max) t->r_max = r;
    t->j_sum += j;      if (j > t->j_max) t->j_max = j;
    pthread_mutex_unlock(&t->mu);
    return miss;
}

static const char *kind_name(rt_kind_t k) {
    return k == RT_PERIODIC ? "periodique" : k == RT_SPORADIC ? "sporadique" : "aperiodique";
}
static double avg_ms(uint64_t sum, long n) { return n ? (double)sum / (double)n / (double)NS_PER_MS : 0.0; }
static double to_ms(uint64_t ns) { return (double)ns / (double)NS_PER_MS; }

static void fmt_param(char *buf, size_t n, int ms) {
    if (ms > 0) snprintf(buf, n, "%d", ms); else snprintf(buf, n, "-");
}

static void fmt_prio(char *buf, size_t n, const rt_task_def_t *d) {
    if (g_fifo && d->fifo_prio > 0) snprintf(buf, n, "FIFO %d", d->fifo_prio);
    else snprintf(buf, n, "nice %d", d->nice);
}

void rt_task_print_table(FILE *out, rt_task_t *const *tasks, int n) {
    fprintf(out, "%-7s %-11s %5s %5s %-8s %-9s %6s %8s %8s %8s %8s %8s %8s %6s\n",
            "tache", "type", "T(ms)", "D(ms)", "prio", "critic.", "N", "Cmoy", "Cmax", "Rmoy", "Rmax",
            "Jmoy", "Jmax", "fautes");
    for (int i = 0; i < n; i++) {
        rt_task_t *t = tasks[i]; char T[12], D[12], P[16];
        pthread_mutex_lock(&t->mu);
        fmt_param(T, sizeof T, t->def.period_ms); fmt_param(D, sizeof D, t->def.deadline_ms);
        fmt_prio(P, sizeof P, &t->def);
        fprintf(out, "%-7s %-11s %5s %5s %-8s %-9s %6ld %8.3f %8.3f %8.3f %8.3f %8.3f %8.3f %6ld\n",
                t->def.name, kind_name(t->def.kind), T, D, P, t->def.criticity, t->n,
                avg_ms(t->c_sum, t->n), to_ms(t->c_max), avg_ms(t->r_sum, t->n), to_ms(t->r_max),
                avg_ms(t->j_sum, t->n), to_ms(t->j_max), t->misses);
        pthread_mutex_unlock(&t->mu);
    }
    fprintf(out, "(temps en ms ; C = temps CPU, R = temps de reponse depuis la liberation, J = jitter d'activation)\n");
}

int rt_task_write_csv(const char *path, const char *policy, rt_task_t *const *tasks, int n) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return -1; }
    fprintf(f, "policy,task,role,type,T_ms,D_ms,fifo_prio,nice,criticity,n,c_avg_ms,c_max_ms,"
               "r_avg_ms,r_max_ms,jitter_avg_ms,jitter_max_ms,misses\n");
    for (int i = 0; i < n; i++) {
        rt_task_t *t = tasks[i];
        pthread_mutex_lock(&t->mu);
        fprintf(f, "%s,%s,%s,%s,%d,%d,%d,%d,%s,%ld,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%ld\n", policy,
                t->def.name, t->def.role, kind_name(t->def.kind), t->def.period_ms, t->def.deadline_ms,
                t->def.fifo_prio, t->def.nice, t->def.criticity, t->n, avg_ms(t->c_sum, t->n),
                to_ms(t->c_max), avg_ms(t->r_sum, t->n), to_ms(t->r_max), avg_ms(t->j_sum, t->n),
                to_ms(t->j_max), t->misses);
        pthread_mutex_unlock(&t->mu);
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ moteur periodique */

void *rt_periodic_main(void *arg) {
    rt_periodic_t *p = arg;
    uint64_t period = (uint64_t)p->task->def.period_ms * NS_PER_MS;
    uint64_t release = p->first_release_ns ? p->first_release_ns : mono_ns() + period;
    while (!atomic_load(p->stop)) {
        sleep_until_ns(release);                  /* reveil a date ABSOLUE : pas de derive */
        if (atomic_load(p->stop)) break;
        uint64_t start = mono_ns(), cpu0 = thread_cpu_ns();
        p->body(p->arg);
        uint64_t end = mono_ns();
        rt_task_record(p->task, release, start, end, thread_cpu_ns() - cpu0);
        release += period;
        /* Depassement : les activations deja perdues sont sautees (pas de rafale de rattrapage). */
        while (release + period <= end) release += period;
    }
    return NULL;
}

/* ------------------------------------------------------------------ ordonnancement */

int rt_sched_setup(int want_fifo, int cpu) {
    g_cpu = cpu;
    g_fifo = 0;
    const char *why = "desactive par --no-fifo";
    if (want_fifo) {
        struct sched_param sp = { .sched_priority = 1 };
        int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);   /* sonde des droits */
        if (rc == 0) {
            g_fifo = 1;
            sp.sched_priority = 0;
            pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
            /* memoire verrouillee : pas de defaut de page imprevisible pendant une tache critique */
            if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) perror("mlockall (ignore)");
        } else {
            why = rc == EPERM ? "droits insuffisants, lancer avec sudo" : strerror(rc);
        }
    }
    char cpu_s[48];
    if (cpu >= 0) snprintf(cpu_s, sizeof cpu_s, "tous les threads sur le CPU %d", cpu);
    else snprintf(cpu_s, sizeof cpu_s, "multiprocesseur");
    if (g_fifo) snprintf(g_mode, sizeof g_mode, "SCHED_FIFO (priorites strictes), %s", cpu_s);
    else snprintf(g_mode, sizeof g_mode, "SCHED_OTHER + nice (repli : %s), %s", why, cpu_s);
    return g_fifo;
}

int rt_sched_fifo_active(void) { return g_fifo; }
const char *rt_sched_mode(void) { return g_mode; }

typedef struct { void *(*fn)(void *); void *arg; int fifo_prio, nice; } tramp_t;

static void apply_self_sched(int fifo_prio, int nice) {
    if (g_cpu >= 0) {
        cpu_set_t set; CPU_ZERO(&set); CPU_SET(g_cpu, &set);
        int rc = pthread_setaffinity_np(pthread_self(), sizeof set, &set);
        if (rc) fprintf(stderr, "affinite CPU %d refusee : %s\n", g_cpu, strerror(rc));
    }
    if (g_fifo && fifo_prio > 0) {
        struct sched_param sp = { .sched_priority = fifo_prio };
        int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (rc) fprintf(stderr, "SCHED_FIFO %d refuse : %s\n", fifo_prio, strerror(rc));
        return;
    }
    /* Linux : setpriority sur le TID agit sur ce seul thread */
    if (setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), nice) != 0 && nice > 0)
        perror("setpriority");
}

static void *trampoline(void *p) {
    tramp_t t = *(tramp_t *)p;
    free(p);
    apply_self_sched(t.fifo_prio, t.nice);
    return t.fn(t.arg);
}

int rt_thread_start(pthread_t *th, int fifo_prio, int nice, void *(*fn)(void *), void *arg) {
    tramp_t *t = malloc(sizeof *t);
    if (!t) return ENOMEM;
    *t = (tramp_t){ fn, arg, fifo_prio, nice };
    /* Pile bornee : avec mlockall(MCL_FUTURE) chaque page de pile est verrouillee a la creation ;
     * la pile par defaut (8 Mo) couterait des dizaines de ms au thread createur. */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, RT_STACK_SIZE);
    int rc = pthread_create(th, &attr, trampoline, t);
    pthread_attr_destroy(&attr);
    if (rc) { free(t); fprintf(stderr, "pthread_create : %s\n", strerror(rc)); }
    return rc;
}

void rt_mutex_init_pi(pthread_mutex_t *mu) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
#if defined(_POSIX_THREAD_PRIO_INHERIT) && _POSIX_THREAD_PRIO_INHERIT > 0
    pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
#endif
    pthread_mutex_init(mu, &a);
    pthread_mutexattr_destroy(&a);
}
