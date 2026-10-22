#include "checkpoint.h"
#include "wal.h"

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ---------- Create snapshot ---------- */

int checkpoint_create(ConcurrentHashTable *ht, const char *snapshot_path) {
    /* Count entries first */
    int num_entries = 0;
    for (int i = 0; i < NUM_BUCKETS; i++) {
        KeyValueNode *node = ht->buckets[i];
        while (node) {
            num_entries++;
            node = node->next;
        }
    }

    /* Build the snapshot in memory */
    size_t buf_cap = 4096 + (size_t)num_entries * 256;
    uint8_t *buf = malloc(buf_cap);
    if (!buf) return -1;

    size_t off = 0;
    uint32_t magic = SNAPSHOT_MAGIC;
    memcpy(buf + off, &magic, 4); off += 4;
    uint32_t n = (uint32_t)num_entries;
    memcpy(buf + off, &n, 4); off += 4;

    for (int i = 0; i < NUM_BUCKETS; i++) {
        KeyValueNode *node = ht->buckets[i];
        while (node) {
            uint32_t key_len = (uint32_t)strlen(node->key);
            uint32_t val_len = (uint32_t)strlen(node->value);
            memcpy(buf + off, &key_len, 4); off += 4;
            memcpy(buf + off, &val_len, 4); off += 4;
            memcpy(buf + off, node->key, key_len); off += key_len;
            memcpy(buf + off, node->value, val_len); off += val_len;
            node = node->next;
        }
    }

    /* CRC32 over everything so far */
    uint32_t crc = crc32_compute(buf, off);
    memcpy(buf + off, &crc, 4); off += 4;

    /* Write to a temp file, then atomically rename */
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", snapshot_path);

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        free(buf);
        return -1;
    }

    ssize_t written = write(fd, buf, off);
    if (written != (ssize_t)off) {
        close(fd);
        free(buf);
        unlink(tmp_path);
        return -1;
    }

    fdatasync(fd);
    close(fd);
    free(buf);

    if (rename(tmp_path, snapshot_path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    return 0;
}

/* ---------- Load snapshot ---------- */

ConcurrentHashTable *checkpoint_load(const char *snapshot_path) {
    int fd = open(snapshot_path, O_RDONLY);
    if (fd < 0) return NULL;

    off_t file_size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    if (file_size < 12) { close(fd); return NULL; }

    uint8_t *data = malloc(file_size);
    if (!data) { close(fd); return NULL; }

    ssize_t bytes_read = read(fd, data, file_size);
    close(fd);
    if (bytes_read != file_size) { free(data); return NULL; }

    /* Verify magic */
    uint32_t magic;
    memcpy(&magic, data, 4);
    if (magic != SNAPSHOT_MAGIC) { free(data); return NULL; }

    /* Verify CRC32 */
    uint32_t stored_crc;
    memcpy(&stored_crc, data + file_size - 4, 4);
    uint32_t computed_crc = crc32_compute(data, file_size - 4);
    if (stored_crc != computed_crc) { free(data); return NULL; }

    uint32_t num_entries;
    memcpy(&num_entries, data + 4, 4);

    ConcurrentHashTable *ht = ht_create();
    if (!ht) { free(data); return NULL; }

    size_t off = 8;
    for (uint32_t i = 0; i < num_entries; i++) {
        if (off + 8 > (size_t)file_size - 4) break;
        uint32_t key_len, val_len;
        memcpy(&key_len, data + off, 4); off += 4;
        memcpy(&val_len, data + off, 4); off += 4;
        if (off + key_len + val_len > (size_t)file_size - 4) break;

        char *key = malloc(key_len + 1);
        memcpy(key, data + off, key_len);
        key[key_len] = '\0';
        off += key_len;

        char *val = malloc(val_len + 1);
        memcpy(val, data + off, val_len);
        val[val_len] = '\0';
        off += val_len;

        ht_put(ht, key, val);
        free(key);
        free(val);
    }

    free(data);
    return ht;
}

/* ---------- Full checkpoint: snapshot + WAL truncate ---------- */

int checkpoint_full(ConcurrentHashTable *ht, const char *snapshot_path, Wal *wal) {
    if (checkpoint_create(ht, snapshot_path) != 0) return -1;
    if (wal_truncate(wal) != 0) return -1;
    return 0;
}

/* ---------- Combined recovery: snapshot + WAL replay ---------- */

ConcurrentHashTable *checkpoint_recover(const char *snapshot_path, const char *wal_path) {
    ConcurrentHashTable *ht = NULL;

    /* Try loading snapshot first */
    ht = checkpoint_load(snapshot_path);
    if (!ht) {
    /* No snapshot — start from empty and replay full WAL */
        return wal_recover(wal_path);
    }

    /* Replay WAL on top of snapshot state */
    int fd = open(wal_path, O_RDONLY);
    if (fd < 0) return ht; /* no WAL → snapshot state is current */

    off_t file_size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    if (file_size == 0) { close(fd); return ht; }

    uint8_t *data = malloc(file_size);
    if (!data) { close(fd); return ht; }

    ssize_t bytes_read = read(fd, data, file_size);
    close(fd);
    if (bytes_read != file_size) { free(data); return ht; }

    size_t off = 0;
    while (off + 9 <= (size_t)file_size) {
        uint8_t op = data[off];
        uint32_t key_len, val_len;
        memcpy(&key_len, data + off + 1, 4);
        memcpy(&val_len, data + off + 5, 4);

        size_t record_len = 9 + key_len + val_len + 4;
        if (off + record_len > (size_t)file_size) break;

        /* Verify CRC32 */
        uint32_t stored_crc;
        memcpy(&stored_crc, data + off + 9 + key_len + val_len, 4);
        uint32_t computed_crc = crc32_compute(data + off, 9 + key_len + val_len);
        if (stored_crc != computed_crc) break;

        char *key = malloc(key_len + 1);
        memcpy(key, data + off + 9, key_len);
        key[key_len] = '\0';

        if (op == WAL_OP_PUT) {
            char *val = malloc(val_len + 1);
            memcpy(val, data + off + 9 + key_len, val_len);
            val[val_len] = '\0';
            ht_put(ht, key, val);
            free(val);
        } else if (op == WAL_OP_DEL) {
            ht_delete(ht, key);
        }
        free(key);
        off += record_len;
    }

    free(data);
    return ht;
}
