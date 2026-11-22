/* node_p.c - Noeud Passagers (M4), port 5004 : file de securite, groupes de correspondance, bus.
 *
 * Entrees :  SIM --QUEUE_UPDATE queue=N-->            P   (taille de la file de securite)
 *            SIM --SECURITY_SLOWDOWN factor=K-->      P   (attente x K)
 *            SIM --BUS_BREAKDOWN buses=N-->           P   (capacite des bus ramenee a N)
 *            C   --TRAITER_GROUPE flight,conn,level,src_ts--> P
 * Sorties :  P --ACK flight,bus,status,wait_ms,src_ts--> C   (status = OK | NO_BUS)
 *            P --QUEUE_UPDATE queue,security_wait_min,buses_available--> B   (contexte du modele IA, cles de ai/node_b.py)
 *
 * Concurrence : les threads de connexion (un par client, voir net.c) ne font que lire le message ;
 * TRAITER_GROUPE est depose dans une file moniteur (msg_queue) consommee par un petit pool de workers.
 * Un worker reserve un bus (semaphore compteur, avec timeout), "transporte" le groupe, puis le libere.
 * Quand les 5 bus sont pris, les groupes suivants attendent : c'est la file d'attente du cahier des charges. */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include "msg_queue.h"
#include "net.h"
#include "resources.h"

#define MAX_WORKERS 64
#define PUSH_TIMEOUT_MS 200

static int    g_workers = 8;
static int    g_trip_ms = 300;
static int    g_bus_timeout_ms = 2000;
static double g_throughput = 10.0;      /* pax/min : 120 en file -> 12 min, comme l'enonce */
static int    g_idle_ms = 0;

typedef struct {
    int    queue;
    double slowdown;
} security_t;

static security_t g_sec = { 0, 1.0 };
static struct { int ok, no_bus, rejected, inflight; double wait_sum_ms, wait_max_ms; } g_stats;
static uint64_t g_t0, g_last_msg;
static volatile sig_atomic_t g_stop;
static msg_queue_t g_q;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER, g_logmu = PTHREAD_MUTEX_INITIALIZER;

static void say(const char *fmt, ...) {
    pthread_mutex_lock(&g_logmu);
    if (!g_t0) g_t0 = now_ns();
    printf("[P +%8.1f ms] ", (now_ns() - g_t0) / 1e6);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n"); fflush(stdout);
    pthread_mutex_unlock(&g_logmu);
}

/* attente estimee au controle de securite = variable "security_wait_min" du vecteur d'entree de l'IA */
static double security_wait_min_locked(void) { return g_sec.queue / g_throughput * g_sec.slowdown; }

static void publish_context(void) {
    pthread_mutex_lock(&g_mu);
    int queue = g_sec.queue; double wait = security_wait_min_locked();
    pthread_mutex_unlock(&g_mu);
    message_t o; msg_init(&o, EV_QUEUE_UPDATE, "NODE_P");
    snprintf(o.payload, sizeof o.payload, "queue=%d,security_wait_min=%.1f,buses_available=%d", queue, wait, buses_free_count());
    if (net_send(PORT_NODE_B, &o)) say("B injoignable : contexte (file=%d, attente=%.1f min) non transmis", queue, wait);
}

static void handle_group(const message_t *m) {
    char flight[16], level[12], status[8];
    kv_get(m->payload, "flight", flight, sizeof flight);
    if (kv_get(m->payload, "level", level, sizeof level)) snprintf(level, sizeof level, "?");
    long conn = kv_get_long(m->payload, "conn", 0);
    unsigned long long src = (unsigned long long)kv_get_double(m->payload, "src_ts", 0);

    uint64_t t0 = now_ns(); int bus = 0;
    int rc = bus_reserve(g_bus_timeout_ms, &bus);
    double wait_ms = (now_ns() - t0) / 1e6;

    snprintf(status, sizeof status, "%s", rc == RES_OK ? "OK" : "NO_BUS");
    message_t ack; msg_init(&ack, EV_ACK, "NODE_P");
    snprintf(ack.payload, sizeof ack.payload, "flight=%s,bus=%d,status=%s,wait_ms=%.1f,src_ts=%llu", flight, bus, status, wait_ms, src);
    net_send(PORT_NODE_C, &ack);

    pthread_mutex_lock(&g_mu);
    if (rc == RES_OK) g_stats.ok++; else g_stats.no_bus++;
    g_stats.wait_sum_ms += wait_ms; if (wait_ms > g_stats.wait_max_ms) g_stats.wait_max_ms = wait_ms;
    pthread_mutex_unlock(&g_mu);

    if (rc != RES_OK) { say("GROUPE %s (%ld correspondances, %s) : AUCUN BUS apres %.0f ms -> abandon", flight, conn, level, wait_ms); return; }
    say("GROUPE %s (%ld correspondances, %s) : bus %d obtenu apres %.1f ms d'attente (bus libres restants : %d)", flight, conn, level, bus, wait_ms, buses_free_count());
    usleep((useconds_t)g_trip_ms * 1000);
    bus_release(bus);
    say("GROUPE %s : arrive, bus %d libere", flight, bus);
}

static void *worker(void *arg) {
    (void)arg; message_t m;
    while (mq_pop(&g_q, &m) == 0) {
        handle_group(&m);
        pthread_mutex_lock(&g_mu); g_stats.inflight--; pthread_mutex_unlock(&g_mu);
    }
    return NULL;
}

static void on_msg(const message_t *m, void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_mu); g_last_msg = now_ns(); pthread_mutex_unlock(&g_mu);
    switch (m->type) {
    case EV_QUEUE_UPDATE: {
        long q = kv_get_long(m->payload, "queue", -1);
        if (q < 0) { say("QUEUE_UPDATE invalide ignore (%s)", m->payload); break; }
        pthread_mutex_lock(&g_mu); g_sec.queue = (int)q; double w = security_wait_min_locked(); pthread_mutex_unlock(&g_mu);
        say("FILE DE SECURITE : %ld personnes -> attente estimee %.1f min", q, w);
        publish_context(); break; }
    case EV_SECURITY_SLOWDOWN: {
        double f = kv_get_double(m->payload, "factor", 0.0);
        if (f < 1.0) { say("SECURITY_SLOWDOWN invalide ignore (%s)", m->payload); break; }
        pthread_mutex_lock(&g_mu); g_sec.slowdown = f; double w = security_wait_min_locked(); pthread_mutex_unlock(&g_mu);
        say("CONTROLE RALENTI x%.1f -> attente estimee %.1f min", f, w);
        publish_context(); break; }
    case EV_BUS_BREAKDOWN: {
        int cap = bus_set_capacity((int)kv_get_long(m->payload, "buses", buses_capacity() - 1));
        say("PANNE DE BUS : capacite %d bus (libres : %d)", cap, buses_free_count());
        publish_context(); break; }
    case EV_TRAITER_GROUPE:
        pthread_mutex_lock(&g_mu); g_stats.inflight++; pthread_mutex_unlock(&g_mu);
        if (mq_push(&g_q, m, PUSH_TIMEOUT_MS)) {     /* echec bruyant, jamais de perte silencieuse */
            pthread_mutex_lock(&g_mu); g_stats.inflight--; g_stats.rejected++; pthread_mutex_unlock(&g_mu);
            say("FILE PLEINE : TRAITER_GROUPE rejete (%s)", m->payload);
        }
        break;
    default: say("message ignore : %s (%s)", event_name(m->type), m->payload);
    }
}

static void summary(void) {
    pthread_mutex_lock(&g_mu);
    int n = g_stats.ok + g_stats.no_bus;
    printf("\n=========== RESUME NOEUD P ===========\n");
    printf("Groupes traites : %d (bus obtenu : %d, sans bus : %d, rejetes : %d)\n", n, g_stats.ok, g_stats.no_bus, g_stats.rejected);
    printf("Attente d'un bus : moyenne %.1f ms, max %.1f ms | bus en etat : %d/%d | file de securite : %d pers. (x%.1f)\n",
           n ? g_stats.wait_sum_ms / n : 0.0, g_stats.wait_max_ms, buses_capacity(), NB_BUSES, g_sec.queue, g_sec.slowdown);
    pthread_mutex_unlock(&g_mu);
}

static void on_sig(int s) { (void)s; g_stop = 1; }

static int usage(const char *p) {
    fprintf(stderr, "usage: %s [--workers N] [--trip-ms N] [--bus-timeout-ms N] [--throughput PAX_PAR_MIN] [--idle-exit S]\n", p);
    return 1;
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i + 1 >= argc) return usage(argv[0]);
        if (!strcmp(argv[i], "--workers")) g_workers = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trip-ms")) g_trip_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bus-timeout-ms")) g_bus_timeout_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--throughput")) g_throughput = atof(argv[++i]);
        else if (!strcmp(argv[i], "--idle-exit")) g_idle_ms = (int)(atof(argv[++i]) * 1000);
        else return usage(argv[0]);
    }
    if (g_workers < 1 || g_workers > MAX_WORKERS || g_trip_ms < 0 || g_throughput <= 0) return usage(argv[0]);

    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    resources_init(); mq_init(&g_q);
    pthread_t w[MAX_WORKERS];
    for (int i = 0; i < g_workers; i++) pthread_create(&w[i], NULL, worker, NULL);
    if (net_serve_start(PORT_NODE_P, on_msg, NULL)) return 1;
    say("Noeud P pret (port %d) : %d bus, %d workers, trajet %d ms, debit securite %.0f pers/min", PORT_NODE_P, buses_capacity(), g_workers, g_trip_ms, g_throughput);

    while (!g_stop) {
        usleep(100000);
        pthread_mutex_lock(&g_mu);
        int handled = g_stats.ok + g_stats.no_bus + g_stats.rejected > 0;
        int idle = g_idle_ms && handled && g_stats.inflight == 0 && (now_ns() - g_last_msg) / 1000000 > (uint64_t)g_idle_ms;
        pthread_mutex_unlock(&g_mu);
        if (idle) break;
    }
    mq_close(&g_q);                              /* les workers finissent les groupes deja recus */
    for (int i = 0; i < g_workers; i++) pthread_join(w[i], NULL);
    summary();
    return 0;
}
