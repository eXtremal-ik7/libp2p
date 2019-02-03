#include "asyncio/objectPool.h"
#include "asyncio/asyncioTypes.h"
#include <stdlib.h>
#include <string.h>

#define INITIAL_BLOCKS_NUM 8

void *objectGet(ObjectPool *pool)
{
  if (pool->blocksNum) {
    pool->blocksNum -= 1;
    return pool->blocks[pool->blocksNum];
  } else {
    return 0;
  }
}

void objectRelease(void *ptr, ObjectPool *pool)
{
  if (pool->blocksNum < pool->blocksNumMax) {
    pool->blocks[pool->blocksNum] = ptr;
    pool->blocksNum++;
  } else {
    if (pool->blocksNumMax == 0) {
      pool->blocksNum = 0;
      pool->blocksNumMax = INITIAL_BLOCKS_NUM;
      pool->blocks = malloc(sizeof(void*)*INITIAL_BLOCKS_NUM);
    } else {
      pool->blocksNumMax *= 2;
      pool->blocks = realloc(pool->blocks, sizeof(void*)*pool->blocksNumMax);
    }

    pool->blocks[pool->blocksNum] = ptr;
    pool->blocksNum++;
  }
}

