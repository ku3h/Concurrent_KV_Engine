#include "hash_table.h"
#include "wal.h"
#include "checkpoint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>

/* ---------- Basic single-threaded correctness ---------- */

static void test_put_get(void) {
    ConcurrentHashTable *ht = ht_create();
    assert(ht != NULL);

    ht_put(ht, "alpha", "one");
    ht_put(ht, "beta", "two");
    ht_put(ht, "gamma", "three");

    char *v = ht_get(ht, "alpha");
    assert(v && strcmp(v, "one") == 0);
    free(v);

    v = ht_get(ht, "beta");
    assert(v && strcmp(v, "two") == 0);
    free(v);

    v = ht_get(ht, "missing");
    assert(v == NULL);

    ht_destroy(ht);
    printf("[PASS] test_put_get\n");
}

static void test_overwrite(void) {
    ConcurrentHashTable *ht = ht_create();
    ht_put(ht, "k", "v1");
    ht_put(ht, "k", "v2");

    char *v = ht_get(ht, "k");
    assert(v && strcmp(v, "v2") == 0);
    free(v);
    ht_destroy(ht);
    printf("[PASS] test_overwrite\n");
}

static void test_delete(void) {
    ConcurrentHashTable *ht = ht_create();
    ht_put(ht, "del", "gone");
    assert(ht_delete(ht, "del") == 1);
    char *v = ht_get(ht, "del");
    assert(v == NULL);
    assert(ht_delete(ht, "del") == 0);
    ht_destroy(ht);
    printf("[PASS] test_delete\n");
}

/* ---------- Rehashing test ---------- */

static void test_rehash(void) {
    ConcurrentHashTable *ht = ht_create();
    /* Insert enough keys to trigger rehash (1024 buckets * 0.75 = 768) */
    char key[32];
    for (int i = 0; i < 2000; i++) {
        snprintf(key, sizeof(key), "key_%d", i);
        ht_put(ht, key, "val");
    }
    assert(ht_count(ht) == 2000);

    /* Verify all keys are retrievable after rehash */
    for (int i = 0; i < 2000; i++) {
        snprintf(key, sizeof(key), "key_%d", i);
        char *v = ht_get(ht, key);
        assert(v && strcmp(v, "val") == 0);
        free(v);
    }

    ht_destroy(ht);
    printf("[PASS] test_rehash\n");
}

/* ---------- TTL test ---------- */

static void test_ttl(void) {
    ConcurrentHashTable *ht = ht_create();

    /* Set key with 1 second TTL */
    ht_put_ttl(ht, "temp", "data", 1);
    char *v = ht_get(ht, "temp");
    assert(v && strcmp(v, "data") == 0);
    free(v);

    /* Wait for expiry */
    sleep(2);

    v = ht_get(ht, "temp");
    assert(v == NULL); /* should be expired */

    ht_destroy(ht);
    printf("[PASS] test_ttl\n");
}

/* ---------- LRU eviction test ---------- */

static void test_lru_eviction(void) {
    ConcurrentHashTable *ht = ht_create();
    ht_set_capacity(ht, 3); /* only 3 entries allowed */

    ht_put(ht, "a", "1");
    ht_put(ht, "b", "2");
    ht_put(ht, "c", "3");

    assert(ht_count(ht) == 3);

    /* Access "a" to make it recently used */
    char *v = ht_get(ht, "a");
    free(v);

    /* Insert "d" — should evict "b" (least recently used) */
    ht_put(ht, "d", "4");
    assert(ht_count(ht) == 3);

    v = ht_get(ht, "b");
    assert(v == NULL); /* evicted */
    free(v);

    v = ht_get(ht, "a");
    assert(v && strcmp(v, "1") == 0); /* still present */
    free(v);

    v = ht_get(ht, "d");
    assert(v && strcmp(v, "4") == 0);
    free(v);

    ht_destroy(ht);
    printf("[PASS] test_lru_eviction\n");
}

/* ---------- Concurrency stress test (run under ThreadSanitizer) ---------- */

#define CONC_THREADS 8
#define CONC_OPS 20000

typedef struct {
    ConcurrentHashTable *ht;
    int id;
} ConcArg;

static void *conc_worker(void *arg) {
    ConcArg *ca = (ConcArg *)arg;
    char key[32];
    char val[32];
    unsigned int seed = (unsigned int)(ca->id * 991 + 17);

    for (int i = 0; i < CONC_OPS; i++) {
        int k = rand_r(&seed) % 1000;
        snprintf(key, sizeof(key), "k:%d", k);
        snprintf(val, sizeof(val), "v:%d:%d", ca->id, i);

        if (rand_r(&seed) % 2) {
            ht_put(ca->ht, key, val);
        } else {
            char *res = ht_get(ca->ht, key);
            free(res);
        }
    }
    return NULL;
}

static void test_concurrent_stress(void) {
    ConcurrentHashTable *ht = ht_create();
    ht_set_capacity(ht, 0); /* unlimited for stress test */
    pthread_t threads[CONC_THREADS];
    ConcArg args[CONC_THREADS];

    for (int i = 0; i < CONC_THREADS; i++) {
        args[i].ht = ht;
        args[i].id = i;
        pthread_create(&threads[i], NULL, conc_worker, &args[i]);
    }
    for (int i = 0; i < CONC_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    ht_destroy(ht);
    printf("[PASS] test_concurrent_stress\n");
}

int main(void) {
    test_put_get();
    test_overwrite();
    test_delete();
    test_rehash();
    test_ttl();
    test_lru_eviction();
    test_concurrent_stress();
    printf("\nAll concurrency tests passed.\n");
    return 0;
}
