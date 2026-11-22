/* msg_queue.c - Voir msg_queue.h. */
#define _GNU_SOURCE
#include <errno.h>
#include <time.h>
#include "msg_queue.h"

void mq_init(msg_queue_t *q) {
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
    q->head = q->count = q->closed = 0;
}

void mq_destroy(msg_queue_t *q) {
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

int mq_push(msg_queue_t *q, const message_t *m, int timeout_ms) {
    struct timespec dl;
    int rc = 0;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec += timeout_ms / 1000; dl.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    }
    pthread_mutex_lock(&q->mu);
    /* while et non if : un reveil ne garantit pas qu'une place est encore libre */
    while (q->count == MQ_CAPACITY && !q->closed && rc == 0) {
        if (timeout_ms < 0) pthread_cond_wait(&q->not_full, &q->mu);
        else if (timeout_ms == 0) rc = ETIMEDOUT;
        else rc = pthread_cond_timedwait(&q->not_full, &q->mu, &dl);
    }
    if (q->closed || q->count == MQ_CAPACITY) { pthread_mutex_unlock(&q->mu); return -1; }
    q->buf[(q->head + q->count) % MQ_CAPACITY] = *m;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mu);
    return 0;
}

int mq_pop(msg_queue_t *q, message_t *m) {
    pthread_mutex_lock(&q->mu);
    while (q->count == 0 && !q->closed) pthread_cond_wait(&q->not_empty, &q->mu);
    if (q->count == 0) { pthread_mutex_unlock(&q->mu); return -1; }
    *m = q->buf[q->head];
    q->head = (q->head + 1) % MQ_CAPACITY;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mu);
    return 0;
}

void mq_close(msg_queue_t *q) {
    pthread_mutex_lock(&q->mu);
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mu);
}

int mq_size(msg_queue_t *q) {
    pthread_mutex_lock(&q->mu);
    int n = q->count;
    pthread_mutex_unlock(&q->mu);
    return n;
}
