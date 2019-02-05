#ifdef __cplusplus
extern "C" {
#endif

#ifndef __ASYNCIO_OBJECTPOOL_H_
#define __ASYNCIO_OBJECTPOOL_H_

#include <stddef.h>

typedef struct {
  const char *type;
  size_t blocksNumMax;
  size_t blocksNum;
  void **blocks;
} ObjectList;

typedef struct ObjectPool {
  ObjectList *elements;
  size_t elementsNum;
} ObjectPool;

void *objectGet(const char *type);
void objectRelease(void *ptr, const char *type);

#endif //__ASYNCIO_OBJECTPOOL_H_

#ifdef __cplusplus
}
#endif
