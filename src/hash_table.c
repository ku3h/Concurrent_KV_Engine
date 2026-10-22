#include "hash_table.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

/* ---------- Hash function ---------- */

static unsigned long hash_djb2(const char *str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }
    return hash;
}

static uint64_t now_epoch(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec;
}

/* ---------- LRU list management (protected by lru_lock) ---------- */

static void lru_remove(ConcurrentHashTable *ht, KeyValueNode *node) {
    if (node->lru_prev) {
        node->lru_prev->lru_next = node->lru_next;
    } else {
        ht->lru_head = node->lru_next;
    }
    if (node->lru_next) {
        node->lru_next->lru_prev = node->lru_prev;
    } else {
        ht->lru_tail = node->lru_prev;
    }
    node->lru_prev = NULL;
    node->lru_next = NULL;
}

static void lru_push_front(ConcurrentHashTable *ht, KeyValueNode *node) {
    node->lru_prev = NULL;
    node->lru_next = ht->lru_head;
    if (ht->lru_head) {
        ht->lru_head->lru_prev = node;
    }
    ht->lru_head = node;
    if (!ht->lru_tail) {
        ht->lru_tail = node;
    }
}

static void lru_touch(ConcurrentHashTable *ht, KeyValueNode *node) {
    if (ht->lru_head == node) return;
    lru_remove(ht, node);
    lru_push_front(ht, node);
}

/* ---------- Eviction (done OUTSIDE stripe locks) ----------
 *
 * Lock ordering: stripe wrlock → lru_lock (always in this order, never reversed).
 * Normal operations hold stripe lock then briefly acquire lru_lock.
 * Eviction acquires stripe wrlock first, then lru_lock — same order.
 * Resize acquires ALL stripe wrlocks but never touches lru_lock.
 * This ordering prevents deadlock.
 */

static void evict_if_needed(ConcurrentHashTable *ht) {
    if (ht->max_capacity <= 0) return;
    if (__atomic_load_n(&ht->count, __ATOMIC_RELAXED) <= ht->max_capacity) return;

    /* Identify victim under lru_lock */
    pthread_mutex_lock(&ht->lru_lock);
    KeyValueNode *victim = ht->lru_tail;
    if (!victim) {
        pthread_mutex_unlock(&ht->lru_lock);
        return;
    }
    /* Save victim's key to find its stripe */
    char victim_key[256];
    strncpy(victim_key, victim->key, sizeof(victim_key) - 1);
    victim_key[sizeof(victim_key) - 1] = '\0';
    pthread_mutex_unlock(&ht->lru_lock);

    unsigned long hash = hash_djb2(victim_key);

    /* Acquire victim's stripe wrlock with retry loop (handles resize) */
    int victim_stripe = -1;
    for (;;) {
        int num_buckets = __atomic_load_n(&ht->num_buckets, __ATOMIC_ACQUIRE);
        int bucket_idx = (int)(hash % (unsigned long)num_buckets);
        int stripe = bucket_idx % ht->num_stripes;

        pthread_rwlock_wrlock(&ht->stripe_locks[stripe]);

        if (__atomic_load_n(&ht->num_buckets, __ATOMIC_ACQUIRE) == num_buckets) {
            victim_stripe = stripe;
            break;
        }
        pthread_rwlock_unlock(&ht->stripe_locks[stripe]);
    }

    /* Re-check victim under lru_lock (we hold stripe wrlock → lru_lock order) */
    pthread_mutex_lock(&ht->lru_lock);
    victim = ht->lru_tail;

    if (victim && strcmp(victim->key, victim_key) == 0) {
        int idx = (int)(hash % (unsigned long)ht->num_buckets);
        KeyValueNode *n = ht->buckets[idx];
        KeyValueNode *prev = NULL;
        while (n) {
            if (n == victim) {
                if (prev) prev->next = n->next;
                else ht->buckets[idx] = n->next;
                break;
            }
            prev = n;
            n = n->next;
        }
        if (n) {
            lru_remove(ht, victim);
            free(victim->key);
            free(victim->value);
            free(victim);
            __atomic_fetch_sub(&ht->count, 1, __ATOMIC_RELAXED);
        }
    }

    pthread_mutex_unlock(&ht->lru_lock);
    pthread_rwlock_unlock(&ht->stripe_locks[victim_stripe]);
}

/* ---------- Resize (stop-the-world, but brief) ----------
 *
 * Acquires ALL stripe wrlocks in ascending order. While all locks are held,
 * no other thread can access the table, so the swap is safe.
 * Uses rehash_lock to ensure only one thread resizes at a time.
 */

static void maybe_resize(ConcurrentHashTable *ht) {
    int count = __atomic_load_n(&ht->count, __ATOMIC_RELAXED);
    double load_factor = (double)count / (double)ht->num_buckets;
    if (load_factor < LOAD_FACTOR_THRESHOLD) return;

    if (pthread_mutex_trylock(&ht->rehash_lock) != 0) return;

    count = __atomic_load_n(&ht->count, __ATOMIC_RELAXED);
    load_factor = (double)count / (double)ht->num_buckets;
    if (load_factor < LOAD_FACTOR_THRESHOLD) {
        pthread_mutex_unlock(&ht->rehash_lock);
        return;
    }

    int new_num_buckets = ht->num_buckets * 2;
    KeyValueNode **new_buckets = calloc(new_num_buckets, sizeof(KeyValueNode *));
    if (!new_buckets) {
        pthread_mutex_unlock(&ht->rehash_lock);
        return;
    }

    /* Acquire ALL stripe write locks in ascending order */
    for (int i = 0; i < ht->num_stripes; i++) {
        pthread_rwlock_wrlock(&ht->stripe_locks[i]);
    }

    /* Rehash all entries into new buckets */
    for (int i = 0; i < ht->num_buckets; i++) {
        KeyValueNode *node = ht->buckets[i];
        while (node) {
            KeyValueNode *next = node->next;
            unsigned long hash = hash_djb2(node->key);
            int new_idx = (int)(hash % (unsigned long)new_num_buckets);
            node->next = new_buckets[new_idx];
            new_buckets[new_idx] = node;
            node = next;
        }
    }

    /* Swap: update buckets pointer first, then num_buckets.
     * Both are written under all stripe locks, so no thread can observe
     * a stale combination. */
    KeyValueNode **old_buckets = ht->buckets;
    ht->buckets = new_buckets;
    __atomic_store_n(&ht->num_buckets, new_num_buckets, __ATOMIC_RELEASE);

    free(old_buckets);

    /* Release ALL stripe locks */
    for (int i = 0; i < ht->num_stripes; i++) {
        pthread_rwlock_unlock(&ht->stripe_locks[i]);
    }

    pthread_mutex_unlock(&ht->rehash_lock);
}

/* ---------- Bucket lookup (called under stripe lock) ---------- */

static KeyValueNode *find_node(ConcurrentHashTable *ht, const char *key,
                               KeyValueNode ***prev_next) {
    unsigned long hash = hash_djb2(key);
    int idx = (int)(hash % (unsigned long)ht->num_buckets);
    KeyValueNode *node = ht->buckets[idx];
    KeyValueNode *prev = NULL;
    while (node) {
        if (strcmp(node->key, key) == 0) {
            if (prev_next) *prev_next = prev ? &prev->next : &ht->buckets[idx];
            return node;
        }
        prev = node;
        node = node->next;
    }
    return NULL;
}

/* ---------- Create / Destroy ---------- */

ConcurrentHashTable *ht_create(void) {
    ConcurrentHashTable *ht = malloc(sizeof(ConcurrentHashTable));
    if (!ht) return NULL;

    ht->num_buckets = NUM_BUCKETS;
    ht->buckets = calloc(ht->num_buckets, sizeof(KeyValueNode *));

    ht->num_stripes = NUM_LOCK_STRIPES;
    ht->stripe_locks = malloc(ht->num_stripes * sizeof(pthread_rwlock_t));
    for (int i = 0; i < ht->num_stripes; i++) {
        pthread_rwlock_init(&ht->stripe_locks[i], NULL);
    }

    ht->new_buckets = NULL;
    ht->new_num_buckets = 0;
    ht->rehash_idx = -1;
    pthread_mutex_init(&ht->rehash_lock, NULL);

    ht->lru_head = NULL;
    ht->lru_tail = NULL;
    pthread_mutex_init(&ht->lru_lock, NULL);
    ht->max_capacity = 0;

    ht->count = 0;

    return ht;
}

void ht_destroy(ConcurrentHashTable *ht) {
    if (!ht) return;

    for (int i = 0; i < ht->num_buckets; i++) {
        KeyValueNode *node = ht->buckets[i];
        while (node) {
            KeyValueNode *next = node->next;
            free(node->key);
            free(node->value);
            free(node);
            node = next;
        }
    }
    free(ht->buckets);

    for (int i = 0; i < ht->num_stripes; i++) {
        pthread_rwlock_destroy(&ht->stripe_locks[i]);
    }
    free(ht->stripe_locks);
    pthread_mutex_destroy(&ht->rehash_lock);
    pthread_mutex_destroy(&ht->lru_lock);
    free(ht);
}

/* ---------- Extended API ---------- */

void ht_set_capacity(ConcurrentHashTable *ht, int max_capacity) {
    pthread_mutex_lock(&ht->lru_lock);
    ht->max_capacity = max_capacity;
    pthread_mutex_unlock(&ht->lru_lock);
}

int ht_count(ConcurrentHashTable *ht) {
    return __atomic_load_n(&ht->count, __ATOMIC_RELAXED);
}

/* ---------- Core operations ----------
 *
 * Each operation uses a retry loop: read num_buckets, acquire the
 * corresponding stripe lock, re-check num_buckets. If a resize happened,
 * unlock and retry. Once we hold the stripe lock and num_buckets is
 * stable, no resize can occur (resize needs ALL stripe locks).
 */

int ht_put_ttl(ConcurrentHashTable *ht, const char *key, const char *value, uint32_t ttl_seconds) {
    unsigned long hash = hash_djb2(key);
    int stripe_idx;

    /* Retry loop: handle concurrent resize */
    for (;;) {
        int num_buckets = __atomic_load_n(&ht->num_buckets, __ATOMIC_ACQUIRE);
        int bucket_idx = (int)(hash % (unsigned long)num_buckets);
        stripe_idx = bucket_idx % ht->num_stripes;

        pthread_rwlock_wrlock(&ht->stripe_locks[stripe_idx]);

        if (__atomic_load_n(&ht->num_buckets, __ATOMIC_ACQUIRE) == num_buckets) {
            break;
        }
        pthread_rwlock_unlock(&ht->stripe_locks[stripe_idx]);
    }

    /* Find existing node */
    KeyValueNode *node = find_node(ht, key, NULL);

    if (node) {
        free(node->value);
        node->value = strdup(value);
        node->expire_at = ttl_seconds > 0 ? now_epoch() + ttl_seconds : 0;

        pthread_mutex_lock(&ht->lru_lock);
        lru_touch(ht, node);
        pthread_mutex_unlock(&ht->lru_lock);

        pthread_rwlock_unlock(&ht->stripe_locks[stripe_idx]);
        return 0;
    }

    /* Insert new node */
    int bucket_idx = (int)(hash % (unsigned long)ht->num_buckets);
    KeyValueNode *new_node = malloc(sizeof(KeyValueNode));
    new_node->key = strdup(key);
    new_node->value = strdup(value);
    new_node->expire_at = ttl_seconds > 0 ? now_epoch() + ttl_seconds : 0;
    new_node->lru_prev = NULL;
    new_node->lru_next = NULL;
    new_node->next = ht->buckets[bucket_idx];
    ht->buckets[bucket_idx] = new_node;
    __atomic_fetch_add(&ht->count, 1, __ATOMIC_RELAXED);

    pthread_mutex_lock(&ht->lru_lock);
    lru_push_front(ht, new_node);
    pthread_mutex_unlock(&ht->lru_lock);

    pthread_rwlock_unlock(&ht->stripe_locks[stripe_idx]);

    /* Eviction and resize happen OUTSIDE the stripe lock */
    evict_if_needed(ht);
    maybe_resize(ht);

    return 1;
}

int ht_put(ConcurrentHashTable *ht, const char *key, const char *value) {
    return ht_put_ttl(ht, key, value, 0);
}

char *ht_get(ConcurrentHashTable *ht, const char *key) {
    unsigned long hash = hash_djb2(key);
    int stripe_idx;

    for (;;) {
        int num_buckets = __atomic_load_n(&ht->num_buckets, __ATOMIC_ACQUIRE);
        int bucket_idx = (int)(hash % (unsigned long)num_buckets);
        stripe_idx = bucket_idx % ht->num_stripes;

        pthread_rwlock_rdlock(&ht->stripe_locks[stripe_idx]);

        if (__atomic_load_n(&ht->num_buckets, __ATOMIC_ACQUIRE) == num_buckets) {
            break;
        }
        pthread_rwlock_unlock(&ht->stripe_locks[stripe_idx]);
    }

    KeyValueNode *node = find_node(ht, key, NULL);
    char *result = NULL;

    if (node) {
        if (node->expire_at > 0 && now_epoch() >= node->expire_at) {
            result = NULL;
        } else {
            result = strdup(node->value);
            pthread_mutex_lock(&ht->lru_lock);
            lru_touch(ht, node);
            pthread_mutex_unlock(&ht->lru_lock);
        }
    }

    pthread_rwlock_unlock(&ht->stripe_locks[stripe_idx]);
    return result;
}

int ht_delete(ConcurrentHashTable *ht, const char *key) {
    unsigned long hash = hash_djb2(key);
    int stripe_idx;

    for (;;) {
        int num_buckets = __atomic_load_n(&ht->num_buckets, __ATOMIC_ACQUIRE);
        int bucket_idx = (int)(hash % (unsigned long)num_buckets);
        stripe_idx = bucket_idx % ht->num_stripes;

        pthread_rwlock_wrlock(&ht->stripe_locks[stripe_idx]);

        if (__atomic_load_n(&ht->num_buckets, __ATOMIC_ACQUIRE) == num_buckets) {
            break;
        }
        pthread_rwlock_unlock(&ht->stripe_locks[stripe_idx]);
    }

    KeyValueNode **prev_next = NULL;
    KeyValueNode *node = find_node(ht, key, &prev_next);
    int found = 0;

    if (node && prev_next) {
        *prev_next = node->next;

        pthread_mutex_lock(&ht->lru_lock);
        lru_remove(ht, node);
        pthread_mutex_unlock(&ht->lru_lock);

        free(node->key);
        free(node->value);
        free(node);
        __atomic_fetch_sub(&ht->count, 1, __ATOMIC_RELAXED);
        found = 1;
    }

    pthread_rwlock_unlock(&ht->stripe_locks[stripe_idx]);
    return found;
}

/* ---------- Iteration ---------- */

void ht_foreach(ConcurrentHashTable *ht, HtCallback cb, void *user_data) {
    for (int i = 0; i < ht->num_stripes; i++) {
        pthread_rwlock_rdlock(&ht->stripe_locks[i]);
    }

    uint64_t now = now_epoch();

    for (int i = 0; i < ht->num_buckets; i++) {
        KeyValueNode *node = ht->buckets[i];
        while (node) {
            if (node->expire_at == 0 || now < node->expire_at) {
                cb(node->key, node->value, user_data);
            }
            node = node->next;
        }
    }

    for (int i = 0; i < ht->num_stripes; i++) {
        pthread_rwlock_unlock(&ht->stripe_locks[i]);
    }
}

void ht_rehash_step(ConcurrentHashTable *ht) {
    (void)ht;
}
