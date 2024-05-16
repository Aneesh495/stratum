#include <stddef.h>
#include <shared/errno.h>
#include <user/strat_api.h>
#include "ledger_store.h"

extern size_t strlen(const char *);
extern int strcmp(const char *, const char *);
extern void *memcpy(void *, const void *, size_t);

#define LOG_PATH "/ledger/.records-v1"
#define LOG_LIMIT 16384U
#define SLOT_COUNT (LOG_LIMIT * 2)

typedef struct {
    uint32_t checksum;
    char id[LEDGER_ID_SIZE];
    char value[LEDGER_VALUE_SIZE];
    uint8_t reserved[2];
} record_t;

_Static_assert(sizeof(record_t) == 160, "ledger record size");

struct ledger_store {
    int fd;
    int failed;
    unsigned count;
    uint32_t slots[SLOT_COUNT];
    record_t records[LOG_LIMIT];
};

static uint32_t fingerprint(const void *data, size_t length) {
    const uint8_t *bytes = data;
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < length; i++) hash = (hash ^ bytes[i]) * 16777619U;
    return hash;
}

static uint32_t checksum(const record_t *record) {
    const size_t offset = offsetof(record_t, id);
    return fingerprint((const uint8_t *)record + offset, sizeof(*record) - offset);
}

static int record_valid(const record_t *record) {
    if (!record->id[0] || record->id[LEDGER_ID_SIZE - 1] || !record->value[0] ||
        record->value[LEDGER_VALUE_SIZE - 1] || record->reserved[0] || record->reserved[1] ||
        checksum(record) != record->checksum) return -STRATUM_EIO;
    for (const char *p = record->id; *p; p++) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || *p == '-')) return -STRATUM_EIO;
    }
    for (const char *p = record->value; *p; p++) {
        if (*p < ' ' || *p > '~') return -STRATUM_EIO;
    }
    return 0;
}

static unsigned index_slot(const ledger_store_t *store, const char *id) {
    unsigned slot = fingerprint(id, strlen(id)) & (SLOT_COUNT - 1);
    while (store->slots[slot] && strcmp(store->records[store->slots[slot] - 1].id, id)) {
        slot = (slot + 1) & (SLOT_COUNT - 1);
    }
    return slot;
}

static int next_record(int fd, record_t *record) {
    int64_t result = read(fd, record, sizeof(*record));
    if (result <= 0) return (int)result;
    if (result != sizeof(*record) || record_valid(record)) return -STRATUM_EIO;
    return 1;
}

int ledger_store_open(ledger_store_t **out) {
    int fd = open(LOG_PATH, O_CREAT | O_RDWR, 0644);
    if (fd < 0) return fd;
    int64_t address = mmap_anonymous(sizeof(ledger_store_t), PROT_READ | PROT_WRITE);
    if (address < 0) {
        close(fd);
        return (int)address;
    }
    ledger_store_t *store = (void *)(uintptr_t)address;
    store->fd = fd;
    record_t record;
    int result;
    while ((result = next_record(fd, &record)) > 0) {
        unsigned slot = index_slot(store, record.id);
        if (store->count == LOG_LIMIT || store->slots[slot]) {
            result = -STRATUM_EIO;
            break;
        }
        store->records[store->count++] = record;
        store->slots[slot] = store->count;
    }
    if (result < 0) {
        ledger_store_close(store);
        return result;
    }
    *out = store;
    return 0;
}

int ledger_store_close(ledger_store_t *store) {
    int result = close(store->fd);
    int unmapped = munmap(store, sizeof(*store));
    return result ? result : unmapped;
}

int ledger_store_get(const ledger_store_t *store, const char *id, char value[LEDGER_VALUE_SIZE]) {
    if (store->failed) return -STRATUM_EIO;
    unsigned index = store->slots[index_slot(store, id)];
    if (!index) return -STRATUM_ENOENT;
    memcpy(value, store->records[index - 1].value, LEDGER_VALUE_SIZE);
    return 0;
}

int ledger_store_get_disk(const char *id, char value[LEDGER_VALUE_SIZE]) {
    int fd = open(LOG_PATH, O_RDONLY, 0);
    if (fd < 0) return fd;
    record_t record;
    int result;
    while ((result = next_record(fd, &record)) > 0) {
        if (!strcmp(id, record.id)) {
            memcpy(value, record.value, LEDGER_VALUE_SIZE);
            break;
        }
    }
    close(fd);
    return result == 1 ? 0 : (result ? result : -STRATUM_ENOENT);
}

int ledger_store_append(ledger_store_t *store, const char *id, const char *value) {
    if (store->failed) return -STRATUM_EIO;
    unsigned slot = index_slot(store, id);
    if (store->slots[slot]) {
        return strcmp(store->records[store->slots[slot] - 1].value, value) ? -STRATUM_EEXIST : 0;
    }
    if (store->count == LOG_LIMIT) return -STRATUM_ENOSPC;
    size_t id_size = strlen(id), value_size = strlen(value);
    if (!id_size || id_size >= LEDGER_ID_SIZE || !value_size || value_size >= LEDGER_VALUE_SIZE) {
        return -STRATUM_EINVAL;
    }

    record_t record = {0};
    memcpy(record.id, id, id_size);
    memcpy(record.value, value, value_size);
    record.checksum = checksum(&record);
    int64_t written = write(store->fd, &record, sizeof(record));
    if (written != sizeof(record)) {
        /* A short or failed write requires a reopen before using the index again. */
        store->failed = 1;
        return written < 0 ? (int)written : -STRATUM_EIO;
    }
    int result = fsync(store->fd);
    if (result) {
        store->failed = 1;
        return result;
    }
    store->records[store->count++] = record;
    store->slots[slot] = store->count;
    return 0;
}
