/* msg_queue.h - File de messages bornee, thread-safe : un MONITEUR (mutex + 2 variables de condition).
 * Producteurs : threads de connexion reseau. Consommateurs : threads de travail du noeud P.
 * Bornee a dessein : une rafale ne doit pas faire grossir la memoire sans limite, le producteur
 * attend (contre-pression) puis echoue bruyamment au lieu de perdre un message en silence. */
#ifndef MSG_QUEUE_H
#define MSG_QUEUE_H
#include <pthread.h>
#include "protocol.h"

#define MQ_CAPACITY 256

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t  not_empty, not_full;
    message_t       buf[MQ_CAPACITY];
    int             head, count, closed;
} msg_queue_t;

void mq_init(msg_queue_t *q);
void mq_destroy(msg_queue_t *q);
/* 0 si ajoute ; -1 si file pleine apres timeout_ms (< 0 : sans limite) ou file fermee. */
int  mq_push(msg_queue_t *q, const message_t *m, int timeout_ms);
/* Bloque jusqu'a un message. 0 si recu ; -1 si la file est fermee ET videe. */
int  mq_pop(msg_queue_t *q, message_t *m);
/* Reveille tous les consommateurs ; les messages restants sont encore distribues. */
void mq_close(msg_queue_t *q);
int  mq_size(msg_queue_t *q);
#endif
