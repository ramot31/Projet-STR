/* engine.h - Moteur temps reel concurrent du systeme (M3) : etat partage entre les threads.
 *
 *   T0 acquisition --(file q_flight)--> T1 surveillance vols (100 ms) --(q_ai)--> T3 prediction IA
 *        |                                                                           |
 *        +--(q_alert)--> ALERTE porte HS (D = 50 ms)                          (q_decision)
 *        +--(q_ground)--> T2 surveillance files (500 ms)                             v
 *                                                       T4 decision --> threads par vol (priorite IA)
 * Chaque file est un moniteur (msg_queue de M4 : mutex + variables de condition).
 * Les portes et les bus sont les ressources protegees de M4 (resources.c : semaphores + mutex). */
#ifndef RT_ENGINE_H
#define RT_ENGINE_H
#include <stdatomic.h>
#include "ai_policy.h"
#include "msg_queue.h"
#include "resources.h"
#include "rt_task.h"

#define MAX_FL   128
#define MAX_OVR  32
#define ID_LEN   16

typedef struct { char flight[ID_LEN], key[24], val[24]; } override_t;
typedef struct { char flight[ID_LEN]; double risk; } risk_override_t;

typedef struct {
    char id[ID_LEN];
    int delay, conn, time_left, arrival_rank, order, batch;
    double risk, boost;
    risk_level_t level;
    char source[8];                    /* "IA" ou "REPLI" */
    int fifo_prio, nice;
    char gate[GATE_NAME_LEN];
    int nb_bus, bus_id;
    /* horodatages CLOCK_MONOTONIC (ns) */
    uint64_t t_src, t_detect, t_risk, t_dispatch, t_start, t_gate, t_path, t_notify;
    int has_risk, launched, done;
    pthread_t th;
} flight_rec_t;

typedef struct {
    /* configuration (lue avant le demarrage des threads, jamais modifiee ensuite) */
    const char *scenario, *csv_prefix;
    policy_t policy;
    double speed, ai_cost_ms, duration_s;
    int load, window_ms, background;
    override_t ovr[MAX_OVR];         int novr;
    risk_override_t risk_ovr[MAX_OVR]; int nrisk;
    /* files d'evenements (producteur -> consommateur) */
    msg_queue_t q_flight, q_ground, q_alert, q_ai, q_decision;
    /* etat partage, protege par mu */
    pthread_mutex_t mu;
    flight_rec_t fl[MAX_FL];
    int nfl, ndispatched, ndone, nbatch;
    int sec_queue; double sec_slow;
    long bg_chunks;
    atomic_int stop, acq_done;
    uint64_t t0;
    /* taches temps reel et leurs mesures */
    rt_task_t t_acq, t_mon, t_pax, t_alert, t_ai, t_dec, t_bg;
} engine_t;

extern engine_t g_eng;

void          eng_log(const char *who, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
flight_rec_t *eng_flight_get(const char *id, int create);     /* g_eng.mu doit etre verrouille */
uint64_t      eng_payload_u64(const char *payload, const char *key);

/* acquisition.c : T0 */
int   acq_load(const char *path);
int   acq_count(void);
void *acq_main(void *arg);
/* tasks.c : T1, T2, alerte, T3, T4, analyse de fond */
void  mon_body(void *arg);
void  pax_body(void *arg);
void *alert_main(void *arg);
void *ai_main(void *arg);
void *decision_main(void *arg);
void *background_main(void *arg);
/* flights.c : ordonnancement intelligent des travaux de vol */
void  flights_dispatch(int *idx, int k);
void  flights_join_all(void);
void  flights_print_summary(FILE *out);
int   flights_write_csv(const char *path);
#endif
