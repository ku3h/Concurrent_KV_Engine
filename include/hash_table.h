#ifndef HASH_TABLE_H
#define HASH_TABLE_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define NUM_BUCKETS 1024
#define NUM_LOCK_STRIPES 32
#define LOAD_FACTOR_THRESHOLD 0.75
#define LRU_DEFAULT_CAPACITY 100000

typedef struct KeyValueNode {
    char *key;
    char *value;
    struct KeyValueNode *next;       /* hash chain (singly linked) */

    /* LRU doubly-linked list */
    struct KeyValueNode *lru_prev;
    struct KeyValueNode *lru_next;

    /* TTL: expiration timestamp in seconds since epoch; 0 = no expiry */
    uint64_t expire_at;
} KeyValueNode;

typedef struct {
    /* Primary bucket array */
    KeyValueNode **buckets;
    int num_buckets;

    /* Incremental rehashing: secondary bucket array */
    KeyValueNode **new_buckets;
    int new_num_buckets;
    int rehash_idx;       /* -1 = not rehashing; 0..num_buckets = next bucket to migrate */

    /* Locking */
    pthread_rwlock_t *stripe_locks;
    int num_stripes;
    pthread_mutex_t rehash_lock; /* guards rehash_idx / new_buckets */

    /* LRU eviction */
    KeyValueNode *lru_head;   /* most recently used */
    KeyValueNode *lru_tail;   /* least recently used */
    pthread_mutex_t lru_lock;
    int max_capacity;         /* 0 = unlimited */

    /* Entry count */
    int count;
} ConcurrentHashTable;

/* ---------- Core API ---------- */

ConcurrentHashTable *ht_create(void);
void ht_destroy(ConcurrentHashTable *ht);
int ht_put(ConcurrentHashTable *ht, const char *key, const char *value);
char *ht_get(ConcurrentHashTable *ht, const char *key);
int ht_delete(ConcurrentHashTable *ht, const char *key);

/* ---------- Extended API ---------- */

/* Put with TTL (seconds from now). ttl=0 means no expiry. */
int ht_put_ttl(ConcurrentHashTable *ht, const char *key, const char *value, uint32_t ttl_seconds);

/* Set max capacity for LRU eviction. 0 = unlimited. */
void ht_set_capacity(ConcurrentHashTable *ht, int max_capacity);

/* Get current entry count. */
int ht_count(ConcurrentHashTable *ht);

/* Callback-based iteration (acquires all stripe locks in read mode). */
typedef void (*HtCallback)(const char *key, const char *value, void *user_data);
void ht_foreach(ConcurrentHashTable *ht, HtCallback cb, void *user_data);

/* ---------- Internal helpers (used by checkpoint.c) ---------- */

/* Trigger incremental rehash step if needed. Called internally by put/delete. */
void ht_rehash_step(ConcurrentHashTable *ht);

#endif
