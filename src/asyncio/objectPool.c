#include "asyncio/objectPool.h"
#include "asyncio/asyncioTypes.h"
#include <stdlib.h>
#include <string.h>

#define INITIAL_BLOCKS_NUM 8

static __tls ObjectPool pool;

ObjectList *getOrCreateElement(ObjectPool *pool, const void *type);

void *objectGet(const void *type)
{
  ObjectList *element = getOrCreateElement(&pool, type);
  if (element->blocksNum) {
    element->blocksNum -= 1;
    return element->blocks[element->blocksNum];
  } else {
    return 0;
  }
}

void objectRelease(void *ptr, const void *type)
{
  ObjectList *element = getOrCreateElement(&pool, type);
  if (element->blocksNum < element->blocksNumMax) {
    element->blocks[element->blocksNum] = ptr;
    element->blocksNum++;
  } else {
    element->blocksNumMax *= 2;
    element->blocks = realloc(element->blocks, sizeof(void*)*element->blocksNumMax);
    element->blocks[element->blocksNum] = ptr;
    element->blocksNum++;
  }
}

void initElement(ObjectList *element, const void *type)
{
  element->type = type;
  element->blocksNum = 0;
  element->blocksNumMax = INITIAL_BLOCKS_NUM;
  element->blocks = malloc(sizeof(void*)*INITIAL_BLOCKS_NUM);
}


size_t searchElement(ObjectPool *pool, const void *type)
{
   size_t lo = 0, hi = pool->elementsNum;
   while (lo < hi) {
      size_t mid = lo + (hi - lo)/2;
      if (pool->elements[mid].type < type)
         lo = mid + 1;
      else
         hi = mid;
   }
   return lo;
}

ObjectList *getOrCreateElement(ObjectPool *pool, const void *type)
{
  if (pool->elementsNum == 0) {
    pool->elementsNum = 1;
    pool->elements = (ObjectList*)malloc(sizeof(ObjectList));    
    initElement(&pool->elements[0], type);
    return &pool->elements[0];
  }
  
  size_t index = searchElement(pool, type);
  if (index < pool->elementsNum && pool->elements[index].type == type)
    return &pool->elements[index];
    
  size_t newElementsNum = pool->elementsNum+1;
  ObjectList *newElements = (ObjectList*)malloc(sizeof(ObjectList)*newElementsNum);
  if (index > 0)
    memcpy(newElements, pool->elements, index*sizeof(ObjectList));
  if (index < pool->elementsNum)
    memcpy(newElements+index+1, pool->elements+index, (pool->elementsNum-index)*sizeof(ObjectList));
  
  free(pool->elements);
  pool->elementsNum = newElementsNum;
  pool->elements = newElements;
  initElement(&pool->elements[index], type);
  return &pool->elements[index];
}
