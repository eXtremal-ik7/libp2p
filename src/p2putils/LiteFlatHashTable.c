#include "LiteFlatHashTable.h"
#include <ctype.h>
#include <stdlib.h>
 
static inline int isEos(char c, const char *eos, size_t eosNum)
{
  for (size_t i = 0; i < eosNum; i++) {
    if (c == eos[i])
      return 1;
  }

  return 0;
}

uint64_t hash64LowerCase(const char *data, const char *end, const char *eos, size_t eosNum, size_t *length)
{
  uint64_t result = 0;
  const char *p = data;
  unsigned shift = 0;
  while (p != end && !isEos(*p, eos, eosNum)) {
    result ^= (uint64_t)tolower(*p) << (8*shift);
    shift = (shift + 1) % 8;
    p++;
  }

  *length = p - data;
  return result ? result : (result | (1ULL << 63));
}

LiteFlatHashTable *buildLiteFlatHashTable(Terminal *terminals, size_t count)
{
  LiteFlatHashTable *hashTable = (LiteFlatHashTable*)malloc(sizeof(LiteFlatHashTable));

  size_t hashTableSize = 1;
  while(hashTableSize < count)
    hashTableSize *= 2;
  hashTableSize *= 2;

  unsigned *collisions = (unsigned*)calloc(hashTableSize, sizeof(unsigned));

  // get max collisions number
  unsigned maxCollisions = 0;
  for (size_t i = 0; i < count; i++) {
    size_t length;
    char eos = 0;
    uint64_t hash = hash64LowerCase(terminals[i].key, 0, &eos, 1, &length);
    size_t pos = hash % hashTableSize;
    if (++collisions[pos] > maxCollisions)
      maxCollisions = collisions[pos];
  }

  // build hash table
  LiteFlatHashEntry *data = (LiteFlatHashEntry*)calloc(hashTableSize * maxCollisions, sizeof(LiteFlatHashEntry));
  for (size_t i = 0; i < count; i++) {
    size_t length;
    char eos = 0;
    uint64_t hash = hash64LowerCase(terminals[i].key, 0, &eos, 1, &length);
    size_t pos = (hash % hashTableSize);

    unsigned offset = maxCollisions - collisions[pos];
    collisions[pos]--;

    size_t hashTablePos = pos*maxCollisions + offset;
    data[hashTablePos].hash = hash;
    data[hashTablePos].value = terminals[i].value;
  }

  free(collisions);
  hashTable->Size = hashTableSize;
  hashTable->CollisionsNum = maxCollisions;
  hashTable->Data = data;
  return hashTable;
}

ParserResultTy searchLiteFlatHashTable(const char **p, const char *end, const char *eos, size_t eosNum, LiteFlatHashTable *table, int *token)
{
  size_t length;
  uint64_t hash = hash64LowerCase(*p, end, eos, eosNum, &length);
  *p += length;
  if (*p == end)
    return ParserResultNeedMoreData;

  size_t pos = (hash % table->Size) * table->CollisionsNum;
  LiteFlatHashEntry *entry = &table->Data[pos];
  for (unsigned i = 0; i < table->CollisionsNum; i++) {
    if (entry[i].hash == hash) {
      *token = entry[i].value;
      return ParserResultOk;
    }
  }

  *token = -1;
  return ParserResultOk;
}
