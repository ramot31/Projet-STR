/* net.h - Communication minimale TCP (fournie par M1 pour pouvoir tout tester seul).
 * M6 peut la remplacer par sa bibliotheque en gardant ces signatures. */
#ifndef NET_H
#define NET_H
#include "protocol.h"

typedef void (*msg_handler_t)(const message_t *m, void *arg);

int      net_serve_start(int port, msg_handler_t cb, void *arg); /* thread d'ecoute, retourne 0 ou -1 */
int      net_send(int port, const message_t *m);                 /* 0 si envoye */
void     msg_init(message_t *m, event_type_t t, const char *origin);
const char *event_name(event_type_t t);
int      event_from_name(const char *name);                      /* -1 si inconnu */

/* payload "cle=valeur,cle=valeur" */
int      kv_get(const char *payload, const char *key, char *out, size_t n);
long     kv_get_long(const char *payload, const char *key, long def);
double   kv_get_double(const char *payload, const char *key, double def);
#endif
