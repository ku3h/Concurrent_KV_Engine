#include "hash_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <math.h>

#define NUM_OPS_PER_THREAD 50000
#define MAX_THREADS 64

/* ---------- Latency tracking ---------- */

typedef struct {
    double *samples;
    size_t count;
} LatencyRecord;

static void lat_init(LatencyRecord *lr, size_t cap) {
    lr->samples = malloc(cap * sizeof(double));
    lr->count = 0;
}

static void lat_add(LatencyRecord *lr, double us) {
    lr->samples[lr->count++] = us;
}

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

static void lat_report(LatencyRecord *lr, const char *label) {
    if (lr->count == 0) return;
    qsort(lr->samples, lr->count, sizeof(double), cmp_double);
    double p50 = lr->samples[(size_t)(0.50 * lr->count)];
    double p95 = lr->samples[(size_t)(0.95 * lr->count)];
    double p99 = lr->samples[(size_t)(0.99 * lr->count)];
    printf("  %-6s p50=%.2fµs  p95=%.2fµs  p99=%.2fµs\n", label, p50, p95, p99);
}

static void lat_destroy(LatencyRecord *lr) {
    free(lr->samples);
}

/* ---------- Benchmark worker ---------- */

typedef struct {
    ConcurrentHashTable *ht;
    int thread_id;
    int read_ratio_pct;   /* 80 = 80% reads / 20% writes */
    LatencyRecord *lat;
} WorkerArgs;

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static void *benchmark_worker(void *arg) {
    WorkerArgs *wa = (WorkerArgs *)arg;
    char key[32];
    char val[32];
    unsigned int seed = (unsigned int)(wa->thread_id * 12345 + 1);

    for (int i = 0; i < NUM_OPS_PER_THREAD; i++) {
        int bucket = rand_r(&seed) % 10000;
        snprintf(key, sizeof(key), "key:%d", bucket);
        snprintf(val, sizeof(val), "val:%d", bucket);

        double start = now_us();

        if ((rand_r(&seed) % 100) < wa->read_ratio_pct) {
            char *res = ht_get(wa->ht, key);
            free(res);
        } else {
            ht_put(wa->ht, key, val);
        }

        double elapsed = now_us() - start;
        lat_add(wa->lat, elapsed);
    }
    return NULL;
}

/* ---------- Run a single concurrency level ---------- */

static void run_benchmark(int num_threads, int read_ratio_pct) {
    ConcurrentHashTable *ht = ht_create();

    pthread_t threads[MAX_THREADS];
    WorkerArgs args[MAX_THREADS];
    LatencyRecord lats[MAX_THREADS];

    for (int i = 0; i < num_threads; i++) {
        lat_init(&lats[i], NUM_OPS_PER_THREAD);
        args[i].ht = ht;
        args[i].thread_id = i;
        args[i].read_ratio_pct = read_ratio_pct;
        args[i].lat = &lats[i];
    }

    double start = now_us();

    for (int i = 0; i < num_threads; i++) {
        pthread_create(&threads[i], NULL, benchmark_worker, &args[i]);
    }
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    double elapsed_us = now_us() - start;
    double elapsed_s = elapsed_us / 1e6;
    long total_ops = (long)num_threads * NUM_OPS_PER_THREAD;
    double ops_per_sec = total_ops / elapsed_s;

    printf("Threads=%2d  |  %.0f ops/sec  |  %.3f s total\n",
           num_threads, ops_per_sec, elapsed_s);

    /* Merge latency samples for combined percentile report */
    LatencyRecord merged;
    lat_init(&merged, total_ops);
    for (int i = 0; i < num_threads; i++) {
        for (size_t j = 0; j < lats[i].count; j++) {
            lat_add(&merged, lats[i].samples[j]);
        }
        lat_destroy(&lats[i]);
    }
    lat_report(&merged, "ALL");
    lat_destroy(&merged);

    ht_destroy(ht);
}

int main(void) {
    int thread_counts[] = {1, 4, 8, 16, 32, 64};
    int num_levels = sizeof(thread_counts) / sizeof(thread_counts[0]);

    printf("=== Concurrent KV Engine — Throughput Benchmark ===\n");
    printf("Workload: 80%% read / 20%% write, %d ops/thread\n\n", NUM_OPS_PER_THREAD);

    for (int i = 0; i < num_levels; i++) {
        run_benchmark(thread_counts[i], 80);
        printf("\n");
    }

    printf("=== Done ===\n");
    return 0;
}
