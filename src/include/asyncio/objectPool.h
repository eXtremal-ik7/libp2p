#ifdef __cplusplus
extern "C" {
#endif

#ifndef __ASYNCIO_OBJECTPOOL_H_
#define __ASYNCIO_OBJECTPOOL_H_

#include <stddef.h>

typedef struct ObjectPool {
  const void *type;
  size_t blocksNumMax;
  size_t blocksNum;
  void **blocks;
} ObjectPool;

void *objectGet(ObjectPool *poolId);
void objectRelease(void *ptr, ObjectPool *poolId);

#endif //__ASYNCIO_OBJECTPOOL_H_

#ifdef __cplusplus
}
#endif
