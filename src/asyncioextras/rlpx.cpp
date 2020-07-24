#include "asyncioextras/rlpx.h"
#include "asyncio/objectPool.h"
#include "asyncio/api.h"
#include "macro.h"
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

enum RlpxOperationState {
  StInitialize = 0,

  // Connect
  StConnectWriteAuth,

  stFinished
};

__NO_PADDING_BEGIN
struct rlpxSocket {
  aioObjectRoot root;
  aioObject *plainSocket;
};

struct RlpxOperation {
  asyncOpRoot root;
  HostAddress address;
  RlpxOperationState state;
};
__NO_PADDING_END

static asyncOpRoot *alloc()
{
  return static_cast<asyncOpRoot*>(allocAsyncOp(sizeof(RlpxOperation)));
}

static void resumeConnectCb(AsyncOpStatus status, aioObject*, void *arg)
{
  resumeParent(static_cast<asyncOpRoot*>(arg), status);
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

static void connectFinish(asyncOpRoot *opptr)
{
  reinterpret_cast<rlpxConnectCb*>(opptr->callback)(opGetStatus(opptr), reinterpret_cast<rlpxSocket*>(opptr->object), opptr->arg);
}

RlpxOperation *initOp(aioExecuteProc *start,
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
  RlpxOperation *op = reinterpret_cast<RlpxOperation*>(opptr);
  return op;
}

RlpxOperation *initReadOp(aioExecuteProc *start,
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
  RlpxOperation *op = reinterpret_cast<RlpxOperation*>(opptr);
  return op;
}

RlpxOperation *initWriteOp(aioExecuteProc *start,
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
  __UNUSED(data);
  __UNUSED(size);
  asyncOpRoot *opptr =
    initAsyncOpRoot(poolId, timerPoolId, alloc, start, cancel, finish, &socket->root, reinterpret_cast<void*>(callback), arg, flags, opCode, timeout);
  RlpxOperation *op = reinterpret_cast<RlpxOperation*>(opptr);
  return op;
}

static void rlpxSocketDestructor(aioObjectRoot *object)
{
  deleteAioObject(reinterpret_cast<rlpxSocket*>(object)->plainSocket);
  objectRelease(object, rplxSocketPool);
}

static AsyncOpStatus startRlpxConnect(asyncOpRoot *opptr)
{
  RlpxOperation *op = reinterpret_cast<RlpxOperation*>(opptr);
  rlpxSocket *socket = reinterpret_cast<rlpxSocket*>(opptr->object);
  asyncOpRoot *childOp = nullptr;
  while (!childOp) {
    switch (op->state) {
      case StInitialize : {
        op->state = StConnectWriteAuth;
        aioConnect(socket->plainSocket, &op->address, 0, resumeConnectCb, op);
        return aosPending;
      }
      case StConnectWriteAuth : {
        // auth = auth-size || ecies.encrypt([sig, initiator-pubk, initiator-nonce, auth-vsn, ...])
      }

      default:
        return aosUnknownError;
    }
  }

  combinerPushOperation(childOp, aaStart);
  return aosPending;
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
  __UNUSED(opptr);
  return aosUnknownError;
}

void aioRlpxAccept(rlpxSocket *socket, AsyncFlags flags, uint64_t timeout, rlpxAcceptCb callback, void *arg)
{
  RlpxOperation *op =
    initOp(startRlpxAccept, acceptFinish, socket, flags, timeout, reinterpret_cast<void*>(callback), arg, rlpxOpAccept);
  combinerPushOperation(&op->root, aaStart);
}

void aioRlpxConnect(rlpxSocket *socket, HostAddress address, AsyncFlags flags, uint64_t timeout, rlpxConnectCb callback, void *arg)
{
  RlpxOperation *op = initOp(startRlpxConnect, connectFinish, socket, flags, timeout, reinterpret_cast<void*>(callback), arg, rlpxOpConnect);
  op->address = address;

}

ssize_t aioRlpxRecv(rlpxSocket *socket, xmstream &stream, size_t sizeLimit, AsyncFlags flags, uint64_t timeout, rlpxRecvCb callback, void *arg)
{
  __UNUSED(socket);
  __UNUSED(stream);
  __UNUSED(sizeLimit);
  __UNUSED(flags);
  __UNUSED(timeout);
  __UNUSED(callback);
  __UNUSED(arg);
  return 0;
}

ssize_t aioRlpxSend(rlpxSocket *socket, void *data, size_t size, AsyncFlags flags, uint64_t timeout, rlpxSendCb callback, void *arg)
{
  __UNUSED(socket);
  __UNUSED(data);
  __UNUSED(size);
  __UNUSED(flags);
  __UNUSED(timeout);
  __UNUSED(callback);
  __UNUSED(arg);
  return 0;
}

int ioRlpxAccept(rlpxSocket *socket, AsyncFlags flags, uint64_t timeout)
{
  __UNUSED(socket);
  __UNUSED(flags);
  __UNUSED(timeout);
  return 0;
}

int ioRlpxConnect(rlpxSocket *socket, HostAddress address, AsyncFlags flags, uint64_t timeout)
{
  __UNUSED(socket);
  __UNUSED(address);
  __UNUSED(flags);
  __UNUSED(timeout);
  return 0;
}

ssize_t ioRlpxRecv(rlpxSocket *socket, xmstream &stream, size_t sizeLimit, AsyncFlags flags, uint64_t timeout)
{
  __UNUSED(socket);
  __UNUSED(stream);
  __UNUSED(sizeLimit);
  __UNUSED(flags);
  __UNUSED(timeout);
  return 0;
}

ssize_t ioRlpxSend(rlpxSocket *socket, void *data, size_t size, AsyncFlags flags, uint64_t timeout)
{
  __UNUSED(socket);
  __UNUSED(data);
  __UNUSED(size);
  __UNUSED(flags);
  __UNUSED(timeout);
  return 0;
}
