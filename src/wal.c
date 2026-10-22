#include "wal.h"

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ---------- CRC32 (IEEE 802.3) ---------- */

static uint32_t crc_table[256];
static int crc_table_initialized = 0;

static void init_crc_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
        crc_table[i] = crc;
    }
    crc_table_initialized = 1;
}

uint32_t crc32_compute(const uint8_t *data, size_t len) {
    if (!crc_table_initialized) init_crc_table();
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ crc_table[(crc ^ data[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFF;
}

/* ---------- Binary record serialization ---------- */

/* Serialize a WAL record into buf. Returns total bytes written, or -1 on error.
 * Format: op(1) + key_len(4) + val_len(4) + key + val + crc32(4) */
static int serialize_record(uint8_t *buf, size_t buf_cap, WalOp op,
                            const char *key, const char *value) {
    uint32_t key_len = (uint32_t)strlen(key);
    uint32_t val_len = value ? (uint32_t)strlen(value) : 0;
    size_t header_size = 1 + 4 + 4; /* op + key_len + val_len */
    size_t payload_size = key_len + val_len;
    size_t total = header_size + payload_size + 4; /* + CRC32 */

    if (total > buf_cap) return -1;

    size_t off = 0;
    buf[off++] = (uint8_t)op;
    memcpy(buf + off, &key_len, 4); off += 4;
    memcpy(buf + off, &val_len, 4); off += 4;
    memcpy(buf + off, key, key_len); off += key_len;
    if (val_len > 0) {
        memcpy(buf + off, value, val_len); off += val_len;
    }

    /* CRC32 over everything except the CRC field itself */
    uint32_t crc = crc32_compute(buf, off);
    memcpy(buf + off, &crc, 4); off += 4;

    return (int)off;
}

/* ---------- WAL open / close ---------- */

Wal *wal_open(const char *path) {
    Wal *wal = malloc(sizeof(Wal));
    if (!wal) return NULL;

    wal->fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (wal->fd < 0) {
        free(wal);
        return NULL;
    }
    strncpy(wal->path, path, sizeof(wal->path) - 1);
    wal->path[sizeof(wal->path) - 1] = '\0';

    wal->commit_buf = NULL;
    wal->commit_buf_used = 0;
    wal->commit_buf_capacity = 0;
    wal->commit_fd = wal->fd;
    wal->group_commit_enabled = 0;

    return wal;
}

void wal_close(Wal *wal) {
    if (!wal) return;
    wal_flush(wal);
    if (wal->fd >= 0) close(wal->fd);
    free(wal->commit_buf);
    free(wal);
}

/* ---------- Group commit ---------- */

void wal_enable_group_commit(Wal *wal, size_t buffer_capacity) {
    if (!wal || buffer_capacity == 0) return;
    wal->commit_buf = malloc(buffer_capacity);
    if (!wal->commit_buf) return;
    wal->commit_buf_capacity = buffer_capacity;
    wal->commit_buf_used = 0;
    wal->group_commit_enabled = 1;
}

int wal_flush(Wal *wal) {
    if (!wal || !wal->group_commit_enabled || wal->commit_buf_used == 0)
        return 0;

    ssize_t written = write(wal->fd, wal->commit_buf, wal->commit_buf_used);
    if (written != (ssize_t)wal->commit_buf_used) return -1;

    int rc = fdatasync(wal->fd);
    wal->commit_buf_used = 0;
    return rc;
}

/* ---------- Append ---------- */

int wal_append(Wal *wal, WalOp op, const char *key, const char *value) {
    if (!wal || !key) return -1;

    uint8_t record[1024];
    int record_len = serialize_record(record, sizeof(record), op, key, value);
    if (record_len < 0) return -1;

    if (wal->group_commit_enabled) {
        /* Buffer the write; flush if buffer is full */
        if (wal->commit_buf_used + (size_t)record_len > wal->commit_buf_capacity) {
            if (wal_flush(wal) < 0) return -1;
        }
        memcpy(wal->commit_buf + wal->commit_buf_used, record, record_len);
        wal->commit_buf_used += record_len;
        return 0;
    }

    /* Immediate write + fdatasync */
    ssize_t written = write(wal->fd, record, record_len);
    if (written != record_len) return -1;
    return fdatasync(wal->fd);
}

/* ---------- Truncate (after checkpoint) ---------- */

int wal_truncate(Wal *wal) {
    if (!wal) return -1;
    wal_flush(wal);
    close(wal->fd);
    /* Reopen with O_TRUNC to empty the file */
    wal->fd = open(wal->path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (wal->fd < 0) return -1;
    wal->commit_fd = wal->fd;
    return 0;
}

/* ---------- Recovery: replay binary WAL ---------- */

ConcurrentHashTable *wal_recover(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return ht_create(); /* no WAL yet → empty table */

    /* Read entire file into memory */
    off_t file_size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    if (file_size == 0) {
        close(fd);
        return ht_create();
    }

    uint8_t *data = malloc(file_size);
    if (!data) {
        close(fd);
        return NULL;
    }

    ssize_t bytes_read = read(fd, data, file_size);
    close(fd);
    if (bytes_read != file_size) {
        free(data);
        return NULL;
    }

    ConcurrentHashTable *ht = ht_create();
    if (!ht) {
        free(data);
        return NULL;
    }

    size_t off = 0;
    while (off + 9 <= (size_t)file_size) {
        uint8_t op = data[off];
        uint32_t key_len, val_len;
        memcpy(&key_len, data + off + 1, 4);
        memcpy(&val_len, data + off + 5, 4);

        size_t record_len = 9 + key_len + val_len + 4; /* header + payload + crc */
        if (off + record_len > (size_t)file_size) break; /* truncated record */

        /* Verify CRC32 */
        uint32_t stored_crc;
        memcpy(&stored_crc, data + off + 9 + key_len + val_len, 4);
        uint32_t computed_crc = crc32_compute(data + off, 9 + key_len + val_len);
        if (stored_crc != computed_crc) {
            /* Corrupt record — stop recovery here */
            break;
        }

        /* Extract key and value */
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
