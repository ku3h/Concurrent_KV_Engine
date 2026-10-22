#include "thread_pool.h"

#include <stdlib.h>
#include <stdio.h>

static void *worker_thread(void *arg) {
    ThreadPool *pool = (ThreadPool *)arg;

    for (;;) {
        pthread_mutex_lock(&pool->lock);

        /* Wait until a task is available or shutdown is signaled */
        while (pool->queue_size == 0 && !pool->shutdown) {
            pthread_cond_wait(&pool->task_available, &pool->lock);
        }

        if (pool->shutdown && pool->queue_size == 0) {
            pthread_mutex_unlock(&pool->lock);
            pthread_exit(NULL);
        }

        /* Dequeue the front task */
        Task *task = pool->queue_head;
        pool->queue_head = task->next;
        if (!pool->queue_head) pool->queue_tail = NULL;
        pool->queue_size--;

        /* Tell any blocked producer that space freed up */
        pthread_cond_signal(&pool->task_completed);
        pthread_mutex_unlock(&pool->lock);

        /* Execute outside the lock */
        task->fn(task->arg);
        free(task);
    }

    return NULL;
}

ThreadPool *tp_create(size_t num_threads, size_t queue_capacity) {
    ThreadPool *pool = malloc(sizeof(ThreadPool));
    if (!pool) return NULL;

    pool->num_threads = num_threads;
    pool->queue_head = NULL;
    pool->queue_tail = NULL;
    pool->queue_size = 0;
    pool->queue_capacity = queue_capacity;
    pool->shutdown = 0;

    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->task_available, NULL);
    pthread_cond_init(&pool->task_completed, NULL);

    pool->threads = malloc(num_threads * sizeof(pthread_t));
    if (!pool->threads) {
        free(pool);
        return NULL;
    }

    for (size_t i = 0; i < num_threads; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_thread, pool) != 0) {
            perror("pthread_create");
            pool->num_threads = i;
            break;
        }
    }

    return pool;
}

int tp_submit(ThreadPool *pool, TaskFn fn, void *arg) {
    pthread_mutex_lock(&pool->lock);

    /* Backpressure: block if queue is full (unless shutting down) */
    while (pool->queue_size >= pool->queue_capacity && !pool->shutdown) {
        pthread_cond_wait(&pool->task_completed, &pool->lock);
    }

    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->lock);
        return -1;
    }

    Task *task = malloc(sizeof(Task));
    task->fn = fn;
    task->arg = arg;
    task->next = NULL;

    if (pool->queue_tail) {
        pool->queue_tail->next = task;
    } else {
        pool->queue_head = task;
    }
    pool->queue_tail = task;
    pool->queue_size++;

    pthread_cond_signal(&pool->task_available);
    pthread_mutex_unlock(&pool->lock);
    return 0;
}

void tp_shutdown(ThreadPool *pool) {
    pthread_mutex_lock(&pool->lock);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->task_available);
    pthread_mutex_unlock(&pool->lock);

    for (size_t i = 0; i < pool->num_threads; i++) {
        pthread_join(pool->threads[i], NULL);
    }
}

void tp_destroy(ThreadPool *pool) {
    if (!pool) return;

    /* Drain any leftover tasks in the queue */
    Task *task = pool->queue_head;
    while (task) {
        Task *next = task->next;
        free(task);
        task = next;
    }

    free(pool->threads);
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->task_available);
    pthread_cond_destroy(&pool->task_completed);
    free(pool);
}
