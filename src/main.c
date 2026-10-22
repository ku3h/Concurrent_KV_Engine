#include "hash_table.h"
#include "wal.h"
#include "checkpoint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAL_PATH "engine.wal"
#define SNAPSHOT_PATH "snapshot.bin"

static void run_demo(void) {
    /* Combined recovery: load snapshot first, then replay WAL */
    ConcurrentHashTable *ht = checkpoint_recover(SNAPSHOT_PATH, WAL_PATH);
    Wal *wal = wal_open(WAL_PATH);

    /* Enable group commit for better write throughput */
    wal_enable_group_commit(wal, 4096);

    /* Seed a few keys */
    ht_put(ht, "user:1001", "Alice");
    wal_append(wal, WAL_OP_PUT, "user:1001", "Alice");

    ht_put(ht, "user:1002", "Bob");
    wal_append(wal, WAL_OP_PUT, "user:1002", "Bob");

    /* Read back */
    char *v1 = ht_get(ht, "user:1001");
    char *v2 = ht_get(ht, "user:1002");
    printf("user:1001 => %s\n", v1 ? v1 : "(nil)");
    printf("user:1002 => %s\n", v2 ? v2 : "(nil)");
    free(v1);
    free(v2);

    /* Delete a key */
    ht_delete(ht, "user:1001");
    wal_append(wal, WAL_OP_DEL, "user:1001", NULL);

    char *v3 = ht_get(ht, "user:1001");
    printf("After delete, user:1001 => %s\n", v3 ? v3 : "(nil)");
    free(v3);

    /* Flush any pending group-commit writes */
    wal_flush(wal);
    wal_close(wal);
    ht_destroy(ht);

    /* Demonstrate crash recovery: reload from snapshot + WAL */
    printf("\n--- Crash Recovery Simulation ---\n");
    ConcurrentHashTable *recovered = checkpoint_recover(SNAPSHOT_PATH, WAL_PATH);
    char *rv1 = ht_get(recovered, "user:1001");
    char *rv2 = ht_get(recovered, "user:1002");
    printf("Recovered user:1001 => %s\n", rv1 ? rv1 : "(nil)");
    printf("Recovered user:1002 => %s\n", rv2 ? rv2 : "(nil)");
    free(rv1);
    free(rv2);

    /* Demonstrate checkpointing: create snapshot, truncate WAL */
    printf("\n--- Checkpoint Demo ---\n");
    int rc = checkpoint_full(recovered, SNAPSHOT_PATH, wal_open(WAL_PATH));
    printf("Checkpoint created: %s\n", rc == 0 ? "success" : "failed");

    ht_destroy(recovered);
}

int main(void) {
    run_demo();
    return 0;
}
