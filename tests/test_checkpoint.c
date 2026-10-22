#include "hash_table.h"
#include "wal.h"
#include "checkpoint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define TEST_SNAPSHOT "test_snapshot.bin"
#define TEST_WAL "test_checkpoint_wal.log"

static void test_checkpoint_create_load(void) {
    unlink(TEST_SNAPSHOT);

    ConcurrentHashTable *ht = ht_create();
    ht_put(ht, "a", "1");
    ht_put(ht, "b", "2");
    ht_put(ht, "c", "3");

    assert(checkpoint_create(ht, TEST_SNAPSHOT) == 0);

    /* Load from snapshot */
    ConcurrentHashTable *loaded = checkpoint_load(TEST_SNAPSHOT);
    assert(loaded != NULL);

    char *va = ht_get(loaded, "a");
    char *vb = ht_get(loaded, "b");
    char *vc = ht_get(loaded, "c");

    assert(va && strcmp(va, "1") == 0);
    assert(vb && strcmp(vb, "2") == 0);
    assert(vc && strcmp(vc, "3") == 0);

    free(va); free(vb); free(vc);
    ht_destroy(loaded);
    ht_destroy(ht);

    printf("[PASS] test_checkpoint_create_load\n");
}

static void test_checkpoint_full(void) {
    unlink(TEST_SNAPSHOT);
    unlink(TEST_WAL);

    ConcurrentHashTable *ht = ht_create();
    Wal *wal = wal_open(TEST_WAL);

    /* Write some data via WAL */
    ht_put(ht, "x", "100");
    wal_append(wal, WAL_OP_PUT, "x", "100");
    ht_put(ht, "y", "200");
    wal_append(wal, WAL_OP_PUT, "y", "200");
    wal_flush(wal);

    /* Checkpoint: snapshot + truncate WAL */
    assert(checkpoint_full(ht, TEST_SNAPSHOT, wal) == 0);

    /* WAL should be empty now; snapshot should have the data */
    wal_close(wal);

    ConcurrentHashTable *recovered = checkpoint_recover(TEST_SNAPSHOT, TEST_WAL);
    assert(recovered != NULL);

    char *vx = ht_get(recovered, "x");
    char *vy = ht_get(recovered, "y");
    assert(vx && strcmp(vx, "100") == 0);
    assert(vy && strcmp(vy, "200") == 0);

    free(vx); free(vy);
    ht_destroy(recovered);
    ht_destroy(ht);

    printf("[PASS] test_checkpoint_full\n");
}

static void test_checkpoint_recover_with_wal(void) {
    unlink(TEST_SNAPSHOT);
    unlink(TEST_WAL);

    /* Phase 1: create checkpoint with some data */
    ConcurrentHashTable *ht = ht_create();
    ht_put(ht, "old", "data");
    checkpoint_create(ht, TEST_SNAPSHOT);
    ht_destroy(ht);

    /* Phase 2: write more data to WAL */
    Wal *wal = wal_open(TEST_WAL);
    wal_append(wal, WAL_OP_PUT, "new", "wal_data");
    wal_append(wal, WAL_OP_DEL, "old", NULL);
    wal_flush(wal);
    wal_close(wal);

    /* Phase 3: recover — should get snapshot state + WAL changes */
    ConcurrentHashTable *recovered = checkpoint_recover(TEST_SNAPSHOT, TEST_WAL);
    assert(recovered != NULL);

    /* "old" should be deleted by WAL */
    char *vold = ht_get(recovered, "old");
    assert(vold == NULL);
    free(vold);

    /* "new" should exist from WAL */
    char *vnew = ht_get(recovered, "new");
    assert(vnew && strcmp(vnew, "wal_data") == 0);
    free(vnew);

    ht_destroy(recovered);
    printf("[PASS] test_checkpoint_recover_with_wal\n");
}

int main(void) {
    test_checkpoint_create_load();
    test_checkpoint_full();
    test_checkpoint_recover_with_wal();
    printf("\nAll checkpoint tests passed.\n");
    return 0;
}
