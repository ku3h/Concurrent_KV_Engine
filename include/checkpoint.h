#ifndef CHECKPOINT_H
#define CHECKPOINT_H

#include "hash_table.h"
#include "wal.h"

/* Snapshot file format:
 *   uint32_t magic        (0x534E4150 = "SNAP")
 *   uint32_t num_entries
 *   For each entry:
 *     uint32_t key_len
 *     uint32_t val_len
 *     uint8_t  key[key_len]
 *     uint8_t  val[val_len]
 *   uint32_t crc32        (over all preceding bytes)
 */

#define SNAPSHOT_MAGIC 0x534E4150

/* Serialize the current hash table state into a binary snapshot file.
 * Returns 0 on success, -1 on error. */
int checkpoint_create(ConcurrentHashTable *ht, const char *snapshot_path);

/* Load hash table state from a snapshot file.
 * Returns a populated ConcurrentHashTable*, or NULL on failure. */
ConcurrentHashTable *checkpoint_load(const char *snapshot_path);

/* Perform a full checkpoint: create snapshot, then truncate the WAL.
 * This is the function a background checkpointing worker would call.
 * Returns 0 on success, -1 on error. */
int checkpoint_full(ConcurrentHashTable *ht, const char *snapshot_path, Wal *wal);

/* Combined recovery: load snapshot first (if exists), then replay WAL
 * on top of it. Returns a ConcurrentHashTable*, or NULL on failure. */
ConcurrentHashTable *checkpoint_recover(const char *snapshot_path, const char *wal_path);

#endif
