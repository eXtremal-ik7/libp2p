#include "asyncioextras/rlpx.h"
#include "asyncio/objectPool.h"
#include <stdlib.h>

static const char *rplxSocketPool = "rlpxSocket";
static const char *poolId = "rlpx";
static const char *timerPoolId = "rlpxTimer";

enum rlpxOpTy {
  rlpxOpAccept = OPCODE_READ,
  rlpxOpRecv,
  rlpxOpConnect = OPCODE_WRITE,
  rlpxOpSend
};

__NO_PADDING_BEGIN
struct rlpxSocket {
  aioObjectRoot root;
  aioObject *plainSocket;
};

struct rlpxOp {
  asyncOpRoot root;
};
__NO_PADDING_END

static asyncOpRoot *alloc()
{
  return static_cast<asyncOpRoot*>(__tagged_alloc(sizeof(rlpxOp)));
}

static int cancel(asyncOpRoot *opptr)
{
  rlpxSocket *socket = reinterpret_cast<rlpxSocket*>(opptr->object);
  cancelIo(reinterpret_cast<aioObjectRoot*>(socket->plainSocket));
  return 0;
}

static void acceptFinish(asyncOpRoot *opptr)
{
  reinterpret_cast<rlpxAcceptCb*>(opptr->callback)(opGetStatus(opptr), reinterpret_cast<rlpxSocket*>(opptr->object), opptr->arg);
}

rlpxOp *initOp(aioExecuteProc *start,
               aioFinishProc *finish,
               rlpxSocket *socket,
               AsyncFlags flags,
               uint64_t timeout,
               void *callback,
               void *arg,
               int opCode)
{
  asyncOpRoot *opptr =
    initAsyncOpRoot(poolId, timerPoolId, alloc, start, cancel, finish, &socket->root, reinterpret_cast<void*>(callback), arg, flags, opCode, timeout);
  rlpxOp *op = reinterpret_cast<rlpxOp*>(opptr);
  return op;
}

rlpxOp *initReadOp(aioExecuteProc *start,
                   aioFinishProc *finish,
                   rlpxSocket *socket,
                   AsyncFlags flags,
                   uint64_t timeout,
                   void *callback,
                   void *arg,
                   int opCode)
{
  asyncOpRoot *opptr =
    initAsyncOpRoot(poolId, timerPoolId, alloc, start, cancel, finish, &socket->root, reinterpret_cast<void*>(callback), arg, flags, opCode, timeout);
  rlpxOp *op = reinterpret_cast<rlpxOp*>(opptr);
  return op;
}

rlpxOp *initWriteOp(aioExecuteProc *start,
                    aioFinishProc *finish,
                    rlpxSocket *socket,
                    AsyncFlags flags,
                    uint64_t timeout,
                    void *callback,
                    void *arg,
                    int opCode,
                    void *data,
                    size_t size)
{
  asyncOpRoot *opptr =
    initAsyncOpRoot(poolId, timerPoolId, alloc, start, cancel, finish, &socket->root, reinterpret_cast<void*>(callback), arg, flags, opCode, timeout);
  rlpxOp *op = reinterpret_cast<rlpxOp*>(opptr);
  return op;
}

static void rlpxSocketDestructor(aioObjectRoot *object)
{
  deleteAioObject(reinterpret_cast<rlpxSocket*>(object)->plainSocket);
  objectRelease(object, rplxSocketPool);
}

rlpxSocket *rlpxSocketNew(asyncBase *base, aioObject *plainSocket)
{
  rlpxSocket *socket = static_cast<rlpxSocket*>(objectGet(rplxSocketPool));
  if (!socket)
    socket = static_cast<rlpxSocket*>(malloc(sizeof(rlpxSocket)));
  initObjectRoot(&socket->root, base, ioObjectUserDefined, rlpxSocketDestructor);
  socket->plainSocket = plainSocket;
  return socket;
}

void rlpxSocketDelete(rlpxSocket *socket)
{
  objectDelete(&socket->root);
}

aioObjectRoot *rlpxSocketHandle(rlpxSocket *socket)
{
  return &socket->root;
}

aioObject *rlpxGetPlainSocket(rlpxSocket *socket)
{
  return socket->plainSocket;
}

static AsyncOpStatus startRlpxAccept(asyncOpRoot *opptr)
{
  return aosUnknownError;
}

void aioRlpxAccept(rlpxSocket *socket, AsyncFlags flags, uint64_t timeout, rlpxAcceptCb callback, void *arg)
{
  rlpxOp *op =
    initOp(startRlpxAccept, acceptFinish, socket, flags, timeout, reinterpret_cast<void*>(callback), arg, rlpxOpAccept);
  opStart(&op->root);
}

void aioRlpxConnect(rlpxSocket *socket, const HostAddress *address, AsyncFlags flags, uint64_t timeout, rlpxConnectCb callback, void *arg)
{

}

ssize_t aioRlpxRecv(rlpxSocket *socket, xmstream &stream, size_t sizeLimit, AsyncFlags flags, uint64_t timeout, rlpxRecvCb callback, void *arg)
{

}

ssize_t aioRlpxSend(rlpxSocket *socket, void *data, size_t size, AsyncFlags flags, uint64_t timeout, rlpxSendCb callback, void *arg)
{

}

int ioRlpxAccept(rlpxSocket *socket, AsyncFlags flags, uint64_t timeout)
{

}

int ioRlpxConnect(rlpxSocket *socket, const HostAddress *address, AsyncFlags flags, uint64_t timeout)
{

}

ssize_t ioRlpxRecv(rlpxSocket *socket, xmstream &stream, size_t sizeLimit, AsyncFlags flags, uint64_t timeout)
{

}

ssize_t ioRlpxSend(rlpxSocket *socket, void *data, size_t size, AsyncFlags flags, uint64_t timeout)
{

}
