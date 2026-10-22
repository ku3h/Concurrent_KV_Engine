#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <pthread.h>
#include <stddef.h>

/* A single unit of work submitted to the pool. */
typedef void (*TaskFn)(void *arg);

typedef struct Task {
    TaskFn fn;
    void *arg;
    struct Task *next;
} Task;

typedef struct {
    pthread_t *threads;
    size_t num_threads;

    Task *queue_head;
    Task *queue_tail;
    size_t queue_size;
    size_t queue_capacity;

    pthread_mutex_t lock;
    pthread_cond_t task_available;  /* signaled when a task is enqueued   */
    pthread_cond_t task_completed;  /* signaled when a task is dequeued   */

    int shutdown;                   /* set to 1 to tell workers to exit   */
} ThreadPool;

/* Create a pool with `num_threads` workers and a bounded queue of `queue_capacity`. */
ThreadPool *tp_create(size_t num_threads, size_t queue_capacity);

/* Submit a task. Blocks (backpressure) if the queue is full. Returns 0 on success. */
int tp_submit(ThreadPool *pool, TaskFn fn, void *arg);

/* Signal all workers to finish remaining tasks, then join every thread. */
void tp_shutdown(ThreadPool *pool);

/* Free pool resources (call after tp_shutdown). */
void tp_destroy(ThreadPool *pool);

#endif
