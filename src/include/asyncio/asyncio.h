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


asyncOp *newUserEvent(asyncBase *base, asyncCb callback, void *arg);
void userEventStartTimer(asyncOp *event, uint64_t usTimeout, int counter);
void userEventStopTimer(asyncOp *event);
void userEventActivate(asyncOp *event);

void asyncConnect(aioObject *op,
                  const HostAddress *address,
                  uint64_t usTimeout,
                  asyncCb callback,
                  void *arg);

void asyncAccept(aioObject *op,
                 uint64_t usTimeout,
                 asyncCb callback,
                 void *arg);

void asyncRead(aioObject *op,
               void *buffer,
               size_t size,
               AsyncFlags flags,
               uint64_t usTimeout,
               asyncCb callback,
               void *arg);

void asyncReadMsg(aioObject *op,
                  dynamicBuffer *buffer,
                  uint64_t usTimeout,
                  asyncCb callback,
                  void *arg);

void asyncWrite(aioObject *op,
                void *buffer,
                size_t size,
                AsyncFlags flags,
                uint64_t usTimeout,
                asyncCb callback,
                void *arg);

void asyncWriteMsg(aioObject *op,
                   const HostAddress *address,
                   void *buffer,
                   size_t size,
                   AsyncFlags flags,
                   uint64_t usTimeout,
                   asyncCb callback,
                   void *arg);

asyncOp *asyncMonitor(aioObject *op, asyncCb callback, void *arg);
void asyncMonitorStop(asyncOp *op);
void asyncLoop(asyncBase *base);
void postQuitOperation(asyncBase *base);

void *queryObject(asyncBase *base, const void *type);
void releaseObject(asyncBase *base, void *object, const void *type);

#ifdef __cplusplus
}
#endif
