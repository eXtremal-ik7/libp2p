#ifdef __cplusplus
extern "C" {
#endif

#ifndef __ASYNCIO_FMALLOC_H_
#define __ASYNCIO_FMALLOC_H_

#include <stddef.h>

typedef struct {
  const void *type;
  size_t blocksNumMax;
  size_t blocksNum;
  void **blocks;
} ObjectList;

typedef struct ObjectPool {
  ObjectList *elements;
  size_t elementsNum;
} ObjectPool;

//void initObjectPool(ObjectPool *pool);
void *objectGet(const void *type);
void objectRelease(void *ptr, const void *type);

#endif //__ASYNCIO_FMALLOC_H_

#ifdef __cplusplus
}
#endif
