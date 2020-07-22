#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "p2putils/CommonParse.h"
#include <stdint.h>

typedef struct Terminal {
  const char *key;
  int value;
} Terminal;

typedef struct LiteFlatHashEntry {
  uint64_t hash;
  int value;
} LiteFlatHashEntry;

typedef struct LiteFlatHashTable {
  size_t Size;
  size_t CollisionsNum;
  LiteFlatHashEntry *Data;
} LiteFlatHashTable;

LiteFlatHashTable *buildLiteFlatHashTable(Terminal *terminals, size_t count);
ParserResultTy searchLiteFlatHashTable(const char **p, const char *end, const char *eos, size_t eosNum, LiteFlatHashTable *table, int *token);

#ifdef __cplusplus
}
#endif
