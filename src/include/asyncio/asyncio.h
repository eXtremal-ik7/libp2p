#ifdef __cplusplus
extern "C" {
#endif

#include "asyncio/asyncioTypes.h"
#include "asyncio/dynamicBuffer.h"
#include "asyncio/socket.h"
#include <stddef.h>
#include <stdint.h>
  
typedef void aioEventCb(asyncBase *base, aioObject *event, void *arg);
typedef void aioConnectCb(AsyncOpStatus status, asyncBase *base, aioObject *object, void *arg);
typedef void aioAcceptCb(AsyncOpStatus status, asyncBase *base, aioObject *listener, HostAddress client, socketTy socket, void *arg);
typedef void aioCb(AsyncOpStatus status, asyncBase *base, aioObject *object, size_t transferred, void *arg);
  
intptr_t argAsInteger(void *arg);
void *intArg(intptr_t id);
socketTy aioObjectSocket(aioObject *object);
iodevTy aioObjectDevice(aioObject *object);


asyncBase *createAsyncBase(AsyncMethod method);
aioObject *newSocketIo(asyncBase *base, socketTy hSocket);
aioObject *newDeviceIo(asyncBase *base, iodevTy hDevice);
// aioObject *newSocketSynIo(asyncBase *base, socketTy hSocket);
void deleteAioObject(aioObject *object);

aioObject *newUserEvent(asyncBase *base, aioEventCb callback, void *arg);
void userEventStartTimer(aioObject *event, uint64_t usTimeout, int counter);
void userEventStopTimer(aioObject *event);
void userEventActivate(aioObject *event);

void aioConnect(asyncBase *base, 
                aioObject *op,
                const HostAddress *address,
                uint64_t usTimeout,
                aioConnectCb callback,
                void *arg);

void aioAccept(asyncBase *base, 
               aioObject *op,
               uint64_t usTimeout,
               aioAcceptCb callback,
               void *arg);

void aioRead(asyncBase *base, 
             aioObject *op,
             void *buffer,
             size_t size,
             AsyncFlags flags,
             uint64_t usTimeout,
             aioCb callback,
             void *arg);

void aioReadMsg(asyncBase *base, 
                aioObject *op,
                dynamicBuffer *buffer,
                uint64_t usTimeout,
                aioCb callback,
                void *arg);

void aioWrite(asyncBase *base, 
              aioObject *op,
              void *buffer,
              size_t size,
              AsyncFlags flags,
              uint64_t usTimeout,
              aioCb callback,
              void *arg);

void aioWriteMsg(asyncBase *base, 
                 aioObject *op,
                 const HostAddress *address,
                 void *buffer,
                 size_t size,
                 AsyncFlags flags,
                 uint64_t usTimeout,
                 aioCb callback,
                 void *arg);


int ioConnect(asyncBase *base, aioObject *op, const HostAddress *address, uint64_t usTimeout);
socketTy ioAccept(asyncBase *base, aioObject *op, uint64_t usTimeout);
ssize_t ioRead(asyncBase *base, aioObject *op, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout);
ssize_t ioReadMsg(asyncBase *base, aioObject *op, dynamicBuffer *buffer, uint64_t usTimeout);
ssize_t ioWrite(asyncBase *base, aioObject *op, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout);
ssize_t ioWriteMsg(asyncBase *base, aioObject *op, const HostAddress *address, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout);
void ioSleep(aioObject *event, uint64_t usTimeout);

// asyncOp *asyncMonitor(aioObject *op, asyncCb callback, void *arg);
// void asyncMonitorStop(asyncOp *op);

void asyncLoop(asyncBase *base);
void postQuitOperation(asyncBase *base);

void *queryObject(asyncBase *base, const void *type);
void releaseObject(asyncBase *base, void *object, const void *type);

#ifdef __cplusplus
}
#endif
