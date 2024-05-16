#ifndef STRATUM_USER_LEDGER_STORE_H
#define STRATUM_USER_LEDGER_STORE_H

#include <stdint.h>

#define LEDGER_ID_SIZE 25
#define LEDGER_VALUE_SIZE 129

typedef struct ledger_store ledger_store_t;

int ledger_store_open(ledger_store_t **out);
int ledger_store_close(ledger_store_t *store);
int ledger_store_append(ledger_store_t *store, const char *id, const char *value);
int ledger_store_get(const ledger_store_t *store, const char *id, char value[LEDGER_VALUE_SIZE]);
int ledger_store_get_disk(const char *id, char value[LEDGER_VALUE_SIZE]);

#endif
