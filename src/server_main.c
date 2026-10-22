#include "hash_table.h"
#include "wal.h"
#include "checkpoint.h"
#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_PORT 8080
#define WAL_PATH "engine.wal"
#define SNAPSHOT_PATH "snapshot.bin"

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[1]);
            return 1;
        }
    }

    /* Recover state from snapshot + WAL */
    ConcurrentHashTable *ht = checkpoint_recover(SNAPSHOT_PATH, WAL_PATH);
    if (!ht) {
        fprintf(stderr, "Failed to recover state\n");
        return 1;
    }

    Wal *wal = wal_open(WAL_PATH);
    if (!wal) {
        fprintf(stderr, "Failed to open WAL\n");
        ht_destroy(ht);
        return 1;
    }

    /* Enable group commit for better write throughput */
    wal_enable_group_commit(wal, 4096);

    printf("Recovered %d keys from checkpoint/WAL\n", ht_count(ht));

    /* Run the epoll server (blocks) */
    int rc = server_run(port, ht, wal);

    /* Shutdown: flush WAL, create final checkpoint */
    wal_flush(wal);
    checkpoint_full(ht, SNAPSHOT_PATH, wal);
    wal_close(wal);
    ht_destroy(ht);

    return rc;
}
