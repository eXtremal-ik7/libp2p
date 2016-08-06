#ifdef __cplusplus
extern "C" {
#endif

#include "asyncio/asyncioTypes.h"
#include "asyncio/dynamicBuffer.h"
#include <stddef.h>
#include <stdint.h>
  
intptr_t argAsInteger(void *arg);
void *intArg(intptr_t id);
socketTy aioObjectSocket(aioObject *object);
iodevTy aioObjectDevice(aioObject *object);


asyncBase *createAsyncBase(AsyncMethod method);
aioObject *newSocketIo(asyncBase *base, socketTy hSocket);
aioObject *newDeviceIo(asyncBase *base, iodevTy hDevice);
aioObject *newSocketSynIo(asyncBase *base, socketTy hSocket);

int getCoroutineMode(asyncBase *base);
void setCoroutineMode(asyncBase *base, int enabled);

asyncOp *newUserEvent(asyncBase *base, asyncCb callback, void *arg);
void userEventStartTimer(asyncOp *event, uint64_t usTimeout, int counter);
void userEventStopTimer(asyncOp *event);
void userEventActivate(asyncOp *event);

void aioConnect(aioObject *op,
                const HostAddress *address,
                uint64_t usTimeout,
                asyncCb callback,
                void *arg);

void aioAccept(aioObject *op,
               uint64_t usTimeout,
               asyncCb callback,
               void *arg);

void aioRead(aioObject *op,
             void *buffer,
             size_t size,
             AsyncFlags flags,
             uint64_t usTimeout,
             asyncCb callback,
             void *arg);

void aioReadMsg(aioObject *op,
                dynamicBuffer *buffer,
                uint64_t usTimeout,
                asyncCb callback,
                void *arg);

void aioWrite(aioObject *op,
              void *buffer,
              size_t size,
              AsyncFlags flags,
              uint64_t usTimeout,
              asyncCb callback,
              void *arg);

void aioWriteMsg(aioObject *op,
                 const HostAddress *address,
                 void *buffer,
                 size_t size,
                 AsyncFlags flags,
                 uint64_t usTimeout,
                 asyncCb callback,
                 void *arg);

void ioConnect(aioObject *op, const HostAddress *address, uint64_t usTimeout);
socketTy ioAccept(aioObject *op, uint64_t usTimeout);
ssize_t ioRead(aioObject *op, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout);
ssize_t ioReadMsg(aioObject *op, dynamicBuffer *buffer, uint64_t usTimeout);
ssize_t ioWrite(aioObject *op, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout);
ssize_t ioWriteMsg(aioObject *op, const HostAddress *address, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout);

// asyncOp *asyncMonitor(aioObject *op, asyncCb callback, void *arg);
// void asyncMonitorStop(asyncOp *op);

void asyncLoop(asyncBase *base);
void postQuitOperation(asyncBase *base);

void *queryObject(asyncBase *base, const void *type);
void releaseObject(asyncBase *base, void *object, const void *type);

#ifdef __cplusplus
}
#endif
