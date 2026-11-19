#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include "net.h"

static const char *NAMES[] = { "", "FLIGHT_DELAY", "GATE_UNAVAILABLE", "QUEUE_UPDATE", "BUS_BREAKDOWN",
  "SECURITY_SLOWDOWN", "RISK_RESULT", "RECHERCHE_PORTE", "GATE_ASSIGNED", "TRAITER_GROUPE", "ACK", "HEARTBEAT" };
#define NNAMES (int)(sizeof NAMES / sizeof NAMES[0])

const char *event_name(event_type_t t) { return (t > 0 && (int)t < NNAMES) ? NAMES[t] : "?"; }
int event_from_name(const char *n) { for (int i = 1; i < NNAMES; i++) if (!strcmp(NAMES[i], n)) return i; return -1; }

static uint64_t g_id = 0;
static pthread_mutex_t g_id_mu = PTHREAD_MUTEX_INITIALIZER;
void msg_init(message_t *m, event_type_t t, const char *origin) {
    memset(m, 0, sizeof *m);
    pthread_mutex_lock(&g_id_mu); m->id = ++g_id; pthread_mutex_unlock(&g_id_mu);
    m->type = t; m->timestamp_ns = now_ns();
    strncpy(m->origin, origin, sizeof m->origin - 1);
}

int kv_get(const char *p, const char *key, char *out, size_t n) {
    size_t kl = strlen(key);
    for (const char *s = p; s && *s; ) {
        if (!strncmp(s, key, kl) && s[kl] == '=') {
            const char *v = s + kl + 1; size_t i = 0;
            while (v[i] && v[i] != ',' && i + 1 < n) { out[i] = v[i]; i++; }
            out[i] = 0; return 0;
        }
        s = strchr(s, ','); if (s) s++;
    }
    return -1;
}
long kv_get_long(const char *p, const char *k, long d) { char b[48]; return kv_get(p, k, b, sizeof b) ? d : atol(b); }
double kv_get_double(const char *p, const char *k, double d) { char b[48]; return kv_get(p, k, b, sizeof b) ? d : atof(b); }

int net_send(int port, const message_t *m) {
    signal(SIGPIPE, SIG_IGN);
    char line[MSG_MAX]; int len = msg_format(m, line, sizeof line);
    const char *host = getenv("AIRPORT_HOST"); if (!host) host = "127.0.0.1";
    for (int attempt = 0; attempt < 3; attempt++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) return -1;
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
        inet_pton(AF_INET, host, &a.sin_addr);
        if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
            int ok = write(fd, line, len) == len; close(fd); if (ok) return 0;
        } else close(fd);
        usleep(20000);
    }
    return -1;
}

typedef struct { int fd; msg_handler_t cb; void *arg; } conn_t;
typedef struct { int lfd; msg_handler_t cb; void *arg; } srv_t;

static void *conn_thread(void *p) {
    conn_t c = *(conn_t *)p; free(p);
    FILE *f = fdopen(c.fd, "r"); char line[MSG_MAX]; message_t m;
    while (f && fgets(line, sizeof line, f)) if (msg_parse(line, &m) == 0) c.cb(&m, c.arg);
    if (f) fclose(f);
    return NULL;
}
static void *accept_thread(void *p) {
    srv_t s = *(srv_t *)p; free(p);
    for (;;) {
        int fd = accept(s.lfd, NULL, NULL); if (fd < 0) continue;
        conn_t *c = malloc(sizeof *c); *c = (conn_t){ fd, s.cb, s.arg };
        pthread_t t; pthread_create(&t, NULL, conn_thread, c); pthread_detach(t);
    }
    return NULL;
}
int net_serve_start(int port, msg_handler_t cb, void *arg) {
    signal(SIGPIPE, SIG_IGN);
    int fd = socket(AF_INET, SOCK_STREAM, 0); int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 64) < 0) { perror("bind/listen"); return -1; }
    srv_t *s = malloc(sizeof *s); *s = (srv_t){ fd, cb, arg };
    pthread_t t; pthread_create(&t, NULL, accept_thread, s); pthread_detach(t);
    return 0;
}
