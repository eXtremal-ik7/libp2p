#ifndef __ASYNCIO_SIMPLEBUFFER_H_
#define __ASYNCIO_SIMPLEBUFFER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

enum SeekTy {
  SeekSet = 0,
  SeekCur,
  SeekEnd
};

typedef enum SeekTy SeekTy;


typedef struct dynamicBuffer {
  void *data;
  size_t size;
  size_t allocatedSize;
  size_t offset;
  int foreign;
} dynamicBuffer;


typedef struct dynamicBuffer dynamicBuffer;

void dynamicBufferInit(dynamicBuffer *buffer, size_t initialSize);
void dynamicBufferInitForeign(dynamicBuffer *buffer, void *data, size_t size);
void dynamicBufferFree(dynamicBuffer *buffer);

void *dynamicBufferAlloc(dynamicBuffer *buffer, size_t size);
void dynamicBufferClear(dynamicBuffer *buffer);
void *dynamicBufferPtr(dynamicBuffer *buffer);
size_t dynamicBufferRemaining(dynamicBuffer *buffer);

void dynamicBufferSeek(dynamicBuffer *buffer, SeekTy type, size_t offset);

#ifdef __cplusplus
}
#endif

#endif //__ASYNCIO_SIMPLEBUFFER_H_
