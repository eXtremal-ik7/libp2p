#ifdef __cplusplus
extern "C" {
#endif

#include "asyncio/api.h"
#include <stddef.h>
#include <stdint.h>
  
typedef void aioEventCb(aioUserEvent*, void*);
typedef void aioConnectCb(AsyncOpStatus, aioObject*, void*);
typedef void aioAcceptCb(AsyncOpStatus, aioObject*, HostAddress, socketTy, void*);
typedef void aioCb(AsyncOpStatus, aioObject*, size_t, void*);
typedef void aioReadMsgCb(AsyncOpStatus, aioObject*, HostAddress, size_t, void*);
  
socketTy aioObjectSocket(aioObject *object);
iodevTy aioObjectDevice(aioObject *object);

asyncBase *createAsyncBase(AsyncMethod method);
aioObject *newSocketIo(asyncBase *base, socketTy hSocket);
aioObject *newDeviceIo(asyncBase *base, iodevTy hDevice);
void deleteAioObject(aioObject *object);
asyncBase *aioGetBase(aioObject *object);

aioUserEvent *newUserEvent(asyncBase *base, aioEventCb callback, void *arg);
void userEventStartTimer(aioUserEvent *event, uint64_t usTimeout, int counter);
void userEventStopTimer(aioUserEvent *event);
void userEventActivate(aioUserEvent *event);
void deleteUserEvent(aioUserEvent *event);

void aioConnect(aioObject *object,
                const HostAddress *address,
                uint64_t usTimeout,
                aioConnectCb callback,
                void *arg);

void aioAccept(aioObject *object,
               uint64_t usTimeout,
               aioAcceptCb callback,
               void *arg);

void aioRead(aioObject *object,
             void *buffer,
             size_t size,
             AsyncFlags flags,
             uint64_t usTimeout,
             aioCb callback,
             void *arg);

void aioReadMsg(aioObject *object,
                void *buffer,
                size_t size,
                AsyncFlags flags,
                uint64_t usTimeout,
                aioReadMsgCb callback,
                void *arg);

void aioWrite(aioObject *object,
              void *buffer,
              size_t size,
              AsyncFlags flags,
              uint64_t usTimeout,
              aioCb callback,
              void *arg);

void aioWriteMsg(aioObject *object,
                 const HostAddress *address,
                 void *buffer,
                 size_t size,
                 AsyncFlags flags,
                 uint64_t usTimeout,
                 aioCb callback,
                 void *arg);


int ioConnect(aioObject *object, const HostAddress *address, uint64_t usTimeout);
socketTy ioAccept(aioObject *object, uint64_t usTimeout);
ssize_t ioRead(aioObject *object, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout);
ssize_t ioReadMsg(aioObject *object, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout);
ssize_t ioWrite(aioObject *object, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout);
ssize_t ioWriteMsg(aioObject *object, const HostAddress *address, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout);
void ioSleep(aioUserEvent *event, uint64_t usTimeout);

void asyncLoop(asyncBase *base);
void postQuitOperation(asyncBase *base);

#ifdef __cplusplus
}
#endif
