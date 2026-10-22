#ifndef WAL_H
#define WAL_H

#include "hash_table.h"
#include <stddef.h>
#include <stdint.h>

typedef enum {
    WAL_OP_PUT = 0,
    WAL_OP_DEL = 1
} WalOp;

/* Binary record format:
 *   uint8_t  op
 *   uint32_t key_len
 *   uint32_t val_len
 *   uint8_t  key[key_len]
 *   uint8_t  val[val_len]     (0 length for DEL)
 *   uint32_t crc32             (over all preceding bytes in the record)
 */
#define WAL_MAGIC 0x57414C00  /* "WAL\0" */

typedef struct {
    int fd;
    char path[256];

    /* Group commit buffer */
    uint8_t *commit_buf;
    size_t commit_buf_used;
    size_t commit_buf_capacity;
    int commit_fd;
    int group_commit_enabled;
} Wal;

/* Open (or create) a WAL file. Returns a Wal* on success, NULL on error. */
Wal *wal_open(const char *path);

/* Append a single mutation record in binary format with CRC32.
 * If group commit is enabled, buffers the write and flushes on batch boundary.
 * Returns 0 on success, -1 on error. */
int wal_append(Wal *wal, WalOp op, const char *key, const char *value);

/* Flush any pending group-commit buffer to disk and fdatasync. */
int wal_flush(Wal *wal);

/* Enable group commit with a given buffer capacity (bytes).
 * When enabled, wal_append buffers writes and only fdatasyncs on flush. */
void wal_enable_group_commit(Wal *wal, size_t buffer_capacity);

/* Replay the WAL into a fresh hash table, restoring state after a crash.
 * Returns a populated ConcurrentHashTable*, or NULL on failure. */
ConcurrentHashTable *wal_recover(const char *path);

/* Truncate the WAL (called after a snapshot checkpoint succeeds).
 * Returns 0 on success, -1 on error. */
int wal_truncate(Wal *wal);

/* Close the WAL and free resources. */
void wal_close(Wal *wal);

/* CRC32 computation (IEEE 802.3 polynomial) */
uint32_t crc32_compute(const uint8_t *data, size_t len);

#endif
