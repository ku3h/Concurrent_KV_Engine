#include "hash_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>

static void test_lru_basic(void) {
    ConcurrentHashTable *ht = ht_create();
    ht_set_capacity(ht, 5);

    for (int i = 0; i < 5; i++) {
        char key[16], val[16];
        snprintf(key, sizeof(key), "k%d", i);
        snprintf(val, sizeof(val), "v%d", i);
        ht_put(ht, key, val);
    }
    assert(ht_count(ht) == 5);

    /* Insert 6th — should evict LRU (k0) */
    ht_put(ht, "k5", "v5");
    assert(ht_count(ht) == 5);

    char *v = ht_get(ht, "k0");
    assert(v == NULL);
    free(v);

    v = ht_get(ht, "k5");
    assert(v && strcmp(v, "v5") == 0);
    free(v);

    ht_destroy(ht);
    printf("[PASS] test_lru_basic\n");
}

static void test_lru_access_order(void) {
    ConcurrentHashTable *ht = ht_create();
    ht_set_capacity(ht, 3);

    ht_put(ht, "a", "1");
    ht_put(ht, "b", "2");
    ht_put(ht, "c", "3");

    /* Access "a" to make it most recently used */
    char *v = ht_get(ht, "a");
    free(v);

    /* Insert "d" — should evict "b" (now LRU) */
    ht_put(ht, "d", "4");

    v = ht_get(ht, "b");
    assert(v == NULL);
    free(v);

    v = ht_get(ht, "a");
    assert(v && strcmp(v, "1") == 0);
    free(v);

    v = ht_get(ht, "c");
    assert(v && strcmp(v, "3") == 0);
    free(v);

    v = ht_get(ht, "d");
    assert(v && strcmp(v, "4") == 0);
    free(v);

    ht_destroy(ht);
    printf("[PASS] test_lru_access_order\n");
}

static void test_ttl_expiry(void) {
    ConcurrentHashTable *ht = ht_create();

    /* 1 second TTL */
    ht_put_ttl(ht, "temp", "value", 1);

    char *v = ht_get(ht, "temp");
    assert(v && strcmp(v, "value") == 0);
    free(v);

    sleep(2);

    v = ht_get(ht, "temp");
    assert(v == NULL);
    free(v);

    ht_destroy(ht);
    printf("[PASS] test_ttl_expiry\n");
}

static void test_ttl_no_expiry(void) {
    ConcurrentHashTable *ht = ht_create();

    ht_put_ttl(ht, "perm", "data", 0); /* 0 = no expiry */

    char *v = ht_get(ht, "perm");
    assert(v && strcmp(v, "data") == 0);
    free(v);

    ht_destroy(ht);
    printf("[PASS] test_ttl_no_expiry\n");
}

static void test_unlimited_capacity(void) {
    ConcurrentHashTable *ht = ht_create();
    ht_set_capacity(ht, 0); /* unlimited */

    for (int i = 0; i < 1000; i++) {
        char key[16], val[16];
        snprintf(key, sizeof(key), "key_%d", i);
        snprintf(val, sizeof(val), "val_%d", i);
        ht_put(ht, key, val);
    }
    assert(ht_count(ht) == 1000);

    ht_destroy(ht);
    printf("[PASS] test_unlimited_capacity\n");
}

int main(void) {
    test_lru_basic();
    test_lru_access_order();
    test_ttl_expiry();
    test_ttl_no_expiry();
    test_unlimited_capacity();
    printf("\nAll eviction tests passed.\n");
    return 0;
}
