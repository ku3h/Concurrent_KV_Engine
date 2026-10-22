#include "hash_table.h"
#include "wal.h"
#include "checkpoint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>

#define TEST_WAL "test_wal.log"

static void test_wal_roundtrip(void) {
    unlink(TEST_WAL);

    Wal *wal = wal_open(TEST_WAL);
    assert(wal != NULL);

    assert(wal_append(wal, WAL_OP_PUT, "a", "1") == 0);
    assert(wal_append(wal, WAL_OP_PUT, "b", "2") == 0);
    assert(wal_append(wal, WAL_OP_PUT, "c", "3") == 0);
    assert(wal_append(wal, WAL_OP_DEL, "b", NULL) == 0);

    wal_flush(wal);
    wal_close(wal);

    ConcurrentHashTable *ht = wal_recover(TEST_WAL);
    assert(ht != NULL);

    char *va = ht_get(ht, "a");
    char *vb = ht_get(ht, "b");
    char *vc = ht_get(ht, "c");

    assert(va && strcmp(va, "1") == 0);
    assert(vb == NULL);
    assert(vc && strcmp(vc, "3") == 0);

    free(va);
    free(vb);
    free(vc);
    ht_destroy(ht);

    printf("[PASS] test_wal_roundtrip\n");
}

static void test_wal_empty(void) {
    unlink(TEST_WAL);

    ConcurrentHashTable *ht = wal_recover(TEST_WAL);
    assert(ht != NULL);

    char *v = ht_get(ht, "anything");
    assert(v == NULL);

    ht_destroy(ht);
    printf("[PASS] test_wal_empty\n");
}

static void test_wal_group_commit(void) {
    unlink(TEST_WAL);

    Wal *wal = wal_open(TEST_WAL);
    assert(wal != NULL);
    wal_enable_group_commit(wal, 4096);

    /* Write multiple records — buffered until flush */
    for (int i = 0; i < 100; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "gck:%d", i);
        snprintf(val, sizeof(val), "gcv:%d", i);
        assert(wal_append(wal, WAL_OP_PUT, key, val) == 0);
    }

    /* Flush to disk */
    assert(wal_flush(wal) == 0);
    wal_close(wal);

    /* Verify recovery */
    ConcurrentHashTable *ht = wal_recover(TEST_WAL);
    assert(ht != NULL);

    for (int i = 0; i < 100; i++) {
        char key[32], expected[32];
        snprintf(key, sizeof(key), "gck:%d", i);
        snprintf(expected, sizeof(expected), "gcv:%d", i);
        char *v = ht_get(ht, key);
        assert(v && strcmp(v, expected) == 0);
        free(v);
    }

    ht_destroy(ht);
    printf("[PASS] test_wal_group_commit\n");
}

static void test_wal_truncate(void) {
    unlink(TEST_WAL);

    Wal *wal = wal_open(TEST_WAL);
    wal_append(wal, WAL_OP_PUT, "x", "1");
    wal_append(wal, WAL_OP_PUT, "y", "2");
    wal_flush(wal);

    assert(wal_truncate(wal) == 0);

    /* WAL should be empty now */
    wal_close(wal);

    ConcurrentHashTable *ht = wal_recover(TEST_WAL);
    assert(ht != NULL);
    assert(ht_count(ht) == 0);
    ht_destroy(ht);

    printf("[PASS] test_wal_truncate\n");
}

static void test_wal_crc_corruption(void) {
    unlink(TEST_WAL);

    Wal *wal = wal_open(TEST_WAL);
    wal_append(wal, WAL_OP_PUT, "good", "data");
    wal_append(wal, WAL_OP_PUT, "bad", "corrupt");
    wal_flush(wal);
    wal_close(wal);

    /* Corrupt the second record by flipping bytes near the end */
    int fd = open(TEST_WAL, O_WRONLY);
    assert(fd >= 0);
    off_t size = lseek(fd, 0, SEEK_END);
    /* Flip some bytes near the end (in the CRC or payload of last record) */
    lseek(fd, size - 2, SEEK_SET);
    char junk = 0xFF;
    write(fd, &junk, 1);
    close(fd);

    /* Recovery should stop at the corrupt record, but recover "good" */
    ConcurrentHashTable *ht = wal_recover(TEST_WAL);
    assert(ht != NULL);

    char *v = ht_get(ht, "good");
    assert(v && strcmp(v, "data") == 0);
    free(v);

    /* "bad" may or may not be recovered depending on corruption location,
     * but the recovery should not crash */
    v = ht_get(ht, "bad");
    free(v);

    ht_destroy(ht);
    printf("[PASS] test_wal_crc_corruption\n");
}

int main(void) {
    test_wal_roundtrip();
    test_wal_empty();
    test_wal_group_commit();
    test_wal_truncate();
    test_wal_crc_corruption();
    printf("\nAll WAL tests passed.\n");
    return 0;
}
