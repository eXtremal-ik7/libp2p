#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>
#include "asyncioImpl.h"
#include "atomic.h"
#include <time.h>

static const char *socketPool = "aioObject";

typedef struct iocpOp iocpOp;

typedef struct recvFromData {
  struct sockaddr_in addr;
  INT size;
} recvFromData;

typedef struct iocpBase {
  asyncBase B;
  HANDLE completionPort;
  HANDLE timerThread;
  LPFN_CONNECTEX ConnectExPtr;
} iocpBase;

typedef struct iocpOp {
  asyncOp info;
  OVERLAPPED overlapped;
} iocpOp;

typedef struct aioTimer {
  asyncOpRoot *op;
  HANDLE hTimer;
} aioTimer;

void combiner(aioObjectRoot *object, tag_t tag, asyncOpRoot *op, AsyncOpActionTy actionType);
void postEmptyOperation(asyncBase *base);
void iocpNextFinishedOperation(asyncBase *base);
aioObject *iocpNewAioObject(asyncBase *base, IoObjectTy type, void *data);
asyncOpRoot *iocpNewAsyncOp();
int iocpCancelAsyncOp(asyncOpRoot *opptr);
void iocpDeleteObject(aioObject *op);
void iocpInitializeTimer(asyncBase *base, asyncOpRoot *op);
void iocpStartTimer(asyncOpRoot *op);
void iocpStopTimer(asyncOpRoot *op);
void iocpActivate(aioUserEvent *event);
AsyncOpStatus iocpAsyncConnect(asyncOpRoot *op);
AsyncOpStatus iocpAsyncAccept(asyncOpRoot *op);
AsyncOpStatus iocpAsyncRead(asyncOpRoot *op);
AsyncOpStatus iocpAsyncWrite(asyncOpRoot *op);
AsyncOpStatus iocpAsyncReadMsg(asyncOpRoot *op);
AsyncOpStatus iocpAsyncWriteMsg(asyncOpRoot *op);

static struct asyncImpl iocpImpl = {
  combiner,
  postEmptyOperation,
  iocpNextFinishedOperation,
  iocpNewAioObject,
  iocpNewAsyncOp,
  iocpCancelAsyncOp,
  iocpDeleteObject,
  iocpInitializeTimer,
  iocpStartTimer,
  iocpStopTimer,
  iocpActivate,
  iocpAsyncConnect,
  iocpAsyncAccept,
  iocpAsyncRead,
  iocpAsyncWrite,
  iocpAsyncReadMsg,
  iocpAsyncWriteMsg
};

static aioObject *getObject(iocpOp *op)
{
  return (aioObject*)op->info.root.object;
}

static DWORD WINAPI timerThreadProc(LPVOID lpParameter)
{
  while (1)
    SleepEx(INFINITE, TRUE);
  return 0;
}

static AsyncOpStatus iocpGetOverlappedResult(iocpOp *op)
{
  DWORD bytesTransferred;
  DWORD flags;
  BOOL result;
  aioObject *object = getObject(op);
  if (object->root.type == ioObjectSocket) {
    result = WSAGetOverlappedResult(object->hSocket, &op->overlapped, &bytesTransferred, FALSE, &flags);
    if (result == TRUE) {
      // Check for disconnect
      if ((op->info.root.opCode == actRead || op->info.root.opCode == actWrite) && bytesTransferred == 0 && op->info.transactionSize > 0) {
        return aosDisconnected;
      }
      return aosSuccess;
    } else {
      int error = WSAGetLastError();
      if (error == WSAEMSGSIZE)
        return aosBufferTooSmall;
      else if (error == WSAECONNRESET || error == ERROR_NETNAME_DELETED)
        return aosDisconnected;
      else if (error == ERROR_OPERATION_ABORTED)
        return aosCanceled;
      else
        return aosUnknownError;
    }
  }
  else {
    result = GetOverlappedResult(object->hDevice, &op->overlapped, &bytesTransferred, FALSE);
    return result == TRUE ? aosSuccess : aosUnknownError;
  }
}

static VOID CALLBACK userEventTimerCb(LPVOID lpArgToCompletionRoutine, DWORD dwTimerLowValue, DWORD dwTimerHighValue)
{
  aioTimer *timer;
  tag_t timerTag;
  __tagged_pointer_decode(lpArgToCompletionRoutine, (void**)&timer, &timerTag);

  int needReactivate = 1;
  aioUserEvent *event = (aioUserEvent*)timer->op;
  iocpBase *localBase = (iocpBase*)event->base;
  PostQueuedCompletionStatus(localBase->completionPort, 0, (ULONG_PTR)event, 0);

  if (event->counter > 0) {
    if (--event->counter == 0)
      needReactivate = 0;
  }

  if (needReactivate) {
    LARGE_INTEGER signalTime;
    signalTime.QuadPart = -(int64_t)(event->root.timeout * 10);
    SetWaitableTimer(timer->hTimer, &signalTime, 0, userEventTimerCb, timer, FALSE);
  }
}


static VOID CALLBACK ioFinishedTimerCb(LPVOID lpArgToCompletionRoutine, DWORD dwTimerLowValue, DWORD dwTimerHighValue)
{
  aioTimer *timer;
  tag_t timerTag;
  __tagged_pointer_decode(lpArgToCompletionRoutine, (void**)&timer, &timerTag);

  if (opSetStatus(timer->op, opEncodeTag(timer->op, timerTag), aosTimeout)) {
    iocpBase *localBase = (iocpBase*)timer->op->object->base;
    PostQueuedCompletionStatus(localBase->completionPort, 0, (ULONG_PTR)timer->op, 0);
  }
}

void combiner(aioObjectRoot *object, tag_t tag, asyncOpRoot *op, AsyncOpActionTy actionType)
{
  tag_t currentTag = tag;
  asyncOpRoot *newOp = op;

  while (currentTag) {
    if (currentTag & TAG_CANCELIO) {
      cancelOperationList(&object->readQueue, &threadLocalQueue, aosCanceled);
      cancelOperationList(&object->writeQueue, &threadLocalQueue, aosCanceled);
    }

    // Check for delete
    if (currentTag & TAG_DELETE) {
      // Perform delete and exit combiner
      object->destructor(object);
      return;
    }

    // Check for pending operations
    tag_t pendingOperationsNum;
    tag_t enqueuedOperationsNum = 0;
    tag_t needStart = 0;
    if ((pendingOperationsNum = __tag_get_opcount(currentTag))) {
      if (newOp) {
        processAction(newOp, actionType, &threadLocalQueue, &needStart);
        enqueuedOperationsNum = 1;
        newOp = 0;
      }

      while (enqueuedOperationsNum < pendingOperationsNum)
        processOperationList(object, &threadLocalQueue, &needStart, &enqueuedOperationsNum);
    }

    if (needStart & TAG_READ_MASK)
      executeOperationList(&object->readQueue, &threadLocalQueue);
    if (needStart & TAG_WRITE_MASK)
      executeOperationList(&object->writeQueue, &threadLocalQueue);

    // Try exit combiner
    tag_t processed = __tag_make_processed(currentTag, enqueuedOperationsNum);
    currentTag = __tag_atomic_fetch_and_add(&object->tag, ((tag_t)0)-processed);
    currentTag -= processed;
  }
}

void postEmptyOperation(asyncBase *base)
{
  PostQueuedCompletionStatus(((iocpBase*)base)->completionPort, 0, 0, 0);
}


asyncBase *iocpNewAsyncBase()
{
  iocpBase *base = malloc(sizeof(iocpBase));
  if (base) {
    SOCKET tmpSocket;
    DWORD numBytes = 0;
    GUID guid = WSAID_CONNECTEX;

    DWORD tid;
    base->completionPort =
      CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0);
    base->timerThread =
      CreateThread(NULL, 0x10000, timerThreadProc, NULL, THREAD_PRIORITY_NORMAL, &tid);
    base->ConnectExPtr = 0;
    tmpSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    WSAIoctl(tmpSocket,
      SIO_GET_EXTENSION_FUNCTION_POINTER,
      &guid,
      sizeof(guid),
      &base->ConnectExPtr,
      sizeof(base->ConnectExPtr),
      &numBytes,
      NULL,
      NULL);
    CloseHandle((HANDLE)tmpSocket);

    base->B.methodImpl = iocpImpl;
  }

  return (asyncBase*)base;
}


void iocpNextFinishedOperation(asyncBase *base)
{
  OVERLAPPED_ENTRY entries[128];
  const int maxEntriesNum = sizeof(entries) / sizeof(OVERLAPPED_ENTRY);
  iocpBase *localBase = (iocpBase*)base;
  messageLoopThreadId = __uint_atomic_fetch_and_add(&base->messageLoopThreadCounter, 1);

  while (1) {
    ULONG N, i;
    executeThreadLocalQueue();
    BOOL status = GetQueuedCompletionStatusEx(localBase->completionPort,
      entries,
      maxEntriesNum,
      &N,
      threadLocalQueue.head ? 0 : 500,
      FALSE);

    time_t currentTime = time(0);
    if (currentTime % base->messageLoopThreadCounter == messageLoopThreadId)
      processTimeoutQueue(base, currentTime);

    // ignore false status
    if (status == FALSE)
      continue;

    for (i = 0; i < N; i++) {
      OVERLAPPED_ENTRY *entry = &entries[i];
      if (entry->lpCompletionKey) {
        asyncOpRoot *op = (asyncOpRoot*)entry->lpCompletionKey;
        if (op->opCode == actUserEvent)
          op->finishMethod(op);
        else
          combinerCall(op->object, 1, op, aaCancel);
      } else if (entry->lpOverlapped) {
        iocpOp *op = (iocpOp*)(((uint8_t*)entry->lpOverlapped) - offsetof(struct iocpOp, overlapped));
        AsyncOpStatus result = iocpGetOverlappedResult(op);
        if (result == aosSuccess) {
          aioObject *object = (aioObject*)op->info.root.object;
          int isBuffered = op->info.root.opCode == actRead && op->info.transactionSize < object->buffer.totalSize;
          if (!isBuffered)
            op->info.bytesTransferred += entry->dwNumberOfBytesTransferred;
          else
            object->buffer.dataSize = entry->dwNumberOfBytesTransferred;
          if (op->info.root.opCode == actAccept) {
            struct sockaddr_in *localAddr = 0;
            struct sockaddr_in *remoteAddr = 0;
            INT localAddrLength;
            INT remoteAddrLength;
            GetAcceptExSockaddrs(op->info.internalBuffer,
              entry->dwNumberOfBytesTransferred,
              sizeof(struct sockaddr_in) + 16,
              sizeof(struct sockaddr_in) + 16,
              (struct sockaddr**)&localAddr, &localAddrLength,
              (struct sockaddr**)&remoteAddr, &remoteAddrLength);
            if (localAddr && remoteAddr) {
              op->info.host.family = remoteAddr->sin_family;
              op->info.host.ipv4 = remoteAddr->sin_addr.s_addr;
              op->info.host.port = remoteAddr->sin_port;
            } else {
              result = aosUnknownError;
            }
          } else if (op->info.root.opCode == actRead || op->info.root.opCode == actWrite) {
            if (isBuffered || ((op->info.root.flags & afWaitAll) && op->info.bytesTransferred < op->info.transactionSize)) {
              combinerCall(op->info.root.object, 1, &op->info.root, aaContinue);
              continue;
            }
          } else if (op->info.root.opCode == actReadMsg) {
            struct recvFromData *rf = op->info.internalBuffer;
            op->info.host.family = rf->addr.sin_family;
            op->info.host.ipv4 = rf->addr.sin_addr.s_addr;
            op->info.host.port = rf->addr.sin_port;
          }
        }

        opSetStatus(&op->info.root, opGetGeneration(&op->info.root), result);
        combinerCall(op->info.root.object, 1, &op->info.root, aaFinish);
      } else {
        while (threadLocalQueue.head)
          executeThreadLocalQueue();
        unsigned threadsRunning = __uint_atomic_fetch_and_add(&base->messageLoopThreadCounter, -1) - 1;
        if (threadsRunning)
          PostQueuedCompletionStatus(((iocpBase*)base)->completionPort, 0, 0, 0);
        return;
      }
    }
  }
}


aioObject *iocpNewAioObject(asyncBase *base, IoObjectTy type, void *data)
{
  iocpBase *localBase = (iocpBase*)base;
  aioObject *object = (aioObject*)objectGet(socketPool);
  if (!object) {
    object = malloc(sizeof(aioObject));
    object->buffer.ptr = 0;
    object->buffer.totalSize = 0;
  }

  initObjectRoot(&object->root, base, type, (aioObjectDestructor*)iocpDeleteObject);
  switch (type) {
    case ioObjectDevice:
      object->hDevice = *(iodevTy *)data;
      CreateIoCompletionPort(object->hDevice, localBase->completionPort, 0, 1);
      break;
    case ioObjectSocket:
      object->hSocket = *(socketTy *)data;
      CreateIoCompletionPort(object->hDevice, localBase->completionPort, 0, 1);
      break;
    default:
      break;
  }

  object->buffer.offset = 0;
  object->buffer.dataSize = 0;
  return object;
}


asyncOpRoot *iocpNewAsyncOp()
{
  iocpOp *op;
  op = malloc(sizeof(iocpOp));
  if (op)
    memset(op, 0, sizeof(iocpOp));

  return (asyncOpRoot*)op;
}

int iocpCancelAsyncOp(asyncOpRoot *opptr)
{
  aioObject *object = (aioObject*)opptr->object;
  iocpOp *op = (iocpOp*)opptr;
  switch (object->root.type) {
    case ioObjectDevice:
      CancelIoEx(object->hDevice, &op->overlapped);
      break;
    case ioObjectSocket:
      CancelIoEx((HANDLE)object->hSocket, &op->overlapped);
      break;
    default:
      break;
  }

  return 0;
}

void iocpDeleteObject(aioObject *object)
{
  switch (object->root.type) {
    case ioObjectDevice:
      CloseHandle(object->hDevice);
      break;
    case ioObjectSocket:
      closesocket(object->hSocket);
      break;
    default:
      break;
  }

  objectRelease(object, socketPool);
}

void iocpInitializeTimer(asyncBase *base, asyncOpRoot *op)
{
  aioTimer *timer = __tagged_alloc(sizeof(aioTimer));
  timer->op = op;
  timer->hTimer = CreateWaitableTimer(NULL, FALSE, NULL);
  op->timerId = timer;
}

static VOID CALLBACK timerStartProc(ULONG_PTR dwParam)
{
  asyncOpRoot *op = (asyncOpRoot*)dwParam;
  PTIMERAPCROUTINE timerCb;
  LARGE_INTEGER signalTime;

  if (op->opCode == actUserEvent)
    timerCb = userEventTimerCb;
  else
    timerCb = ioFinishedTimerCb;

  aioTimer *timer = (aioTimer*)op->timerId;
  signalTime.QuadPart = -(int64_t)(op->timeout * 10);
  SetWaitableTimer(timer->hTimer, &signalTime, 0, timerCb, __tagged_pointer_make(timer, opGetGeneration(op)), FALSE);
}

void iocpStartTimer(asyncOpRoot *op)
{
  iocpBase *base = op->opCode == actUserEvent ?
    (iocpBase*)(((aioUserEvent*)op)->base) : (iocpBase*)op->object->base;
  QueueUserAPC(timerStartProc, base->timerThread, (ULONG_PTR)op);
}


void iocpStopTimer(asyncOpRoot *op)
{
  CancelWaitableTimer(((aioTimer*)op->timerId)->hTimer);
}


void iocpActivate(aioUserEvent *event)
{
  iocpBase *localBase = (iocpBase*)event->base;
  PostQueuedCompletionStatus(localBase->completionPort, 0, (ULONG_PTR)&event->root, 0);
}


AsyncOpStatus iocpAsyncConnect(asyncOpRoot *opptr)
{
  iocpOp *op = (iocpOp*)opptr;
  aioObject *object = getObject(op);
  iocpBase *localBase = (iocpBase*)object->root.base;

  struct sockaddr_in localAddress;
  localAddress.sin_family = op->info.host.family;
  localAddress.sin_addr.s_addr = op->info.host.ipv4;
  localAddress.sin_port = op->info.host.port;
  memset(&op->overlapped, 0, sizeof(op->overlapped));
  int result = localBase->ConnectExPtr(object->hSocket, (const struct sockaddr*)&localAddress, sizeof(struct sockaddr_in), NULL, 0, NULL, &op->overlapped);
  return (result == 0 || WSAGetLastError() == WSA_IO_PENDING) ?
    aosPending :
    aosUnknownError;
}


AsyncOpStatus iocpAsyncAccept(asyncOpRoot *opptr)
{
  iocpOp *op = (iocpOp*)opptr;
  aioObject *object = getObject(op);

  const size_t acceptResultSize = 2 * (sizeof(struct sockaddr_in) + 16);
  if (op->info.internalBuffer == 0) {
    op->info.internalBuffer = malloc(acceptResultSize);
    op->info.internalBufferSize = acceptResultSize;
  } else if (op->info.internalBufferSize < acceptResultSize) {
    op->info.internalBuffer = realloc(op->info.internalBuffer, acceptResultSize);
    op->info.internalBufferSize = acceptResultSize;
  }

  u_long arg = 1;
  op->info.acceptSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
  ioctlsocket(op->info.acceptSocket, FIONBIO, &arg);

  memset(&op->overlapped, 0, sizeof(op->overlapped));
  int result = AcceptEx(object->hSocket,
                        op->info.acceptSocket,
                        op->info.internalBuffer,
                        0,
                        sizeof(struct sockaddr_in) + 16,
                        sizeof(struct sockaddr_in) + 16,
                        NULL,
                        &op->overlapped);

  if (result == 0 || WSAGetLastError() == WSA_IO_PENDING) {
    return aosPending;
  } else {
    return aosUnknownError;
  }
}


AsyncOpStatus iocpAsyncRead(asyncOpRoot *opptr)
{
  WSABUF wsabuf;
  iocpOp *op = (iocpOp*)opptr;
  aioObject *object = getObject(op);
  struct ioBuffer *sb = &object->buffer;
  DWORD flags = 0;

  if (copyFromBuffer(op->info.buffer, &op->info.bytesTransferred, sb, op->info.transactionSize))
    return aosSuccess;

  if (op->info.transactionSize <= object->buffer.totalSize) {
    wsabuf.buf = sb->ptr;
    wsabuf.len = (ULONG)sb->totalSize;
    memset(&op->overlapped, 0, sizeof(op->overlapped));
    int result = WSARecv(object->hSocket, &wsabuf, 1, NULL, &flags, &op->overlapped, NULL);
    if (result == 0 || WSAGetLastError() == WSA_IO_PENDING)
      return aosPending;
    else
      return aosUnknownError;
  } else {
    wsabuf.buf = (CHAR*)op->info.buffer + op->info.bytesTransferred;
    wsabuf.len = (ULONG)(op->info.transactionSize - op->info.bytesTransferred);
    memset(&op->overlapped, 0, sizeof(op->overlapped));
    int result = WSARecv(object->hSocket, &wsabuf, 1, NULL, &flags, &op->overlapped, NULL);
    if (result == 0 || WSAGetLastError() == WSA_IO_PENDING)
      return aosPending;
    else
      return aosUnknownError;
  }
}


AsyncOpStatus iocpAsyncWrite(asyncOpRoot *opptr)
{
  WSABUF wsabuf;
  iocpOp *op = (iocpOp*)opptr;
  aioObject *object = getObject(op);
  // TODO: correct processing >4Gb data blocks
  wsabuf.buf = (CHAR*)op->info.buffer + op->info.bytesTransferred;
  wsabuf.len = wsabuf.len = (ULONG)(op->info.transactionSize - op->info.bytesTransferred);
  memset(&op->overlapped, 0, sizeof(op->overlapped));
  int result = WSASend(object->hSocket, &wsabuf, 1, NULL, 0, &op->overlapped, NULL);
  if (result == 0 || WSAGetLastError() == WSA_IO_PENDING) {
    return aosPending;
  } else {
    return aosUnknownError;
  }
}


AsyncOpStatus iocpAsyncReadMsg(asyncOpRoot *opptr)
{
  WSABUF wsabuf;
  iocpOp *op = (iocpOp*)opptr;
  aioObject *object = getObject(op);

  const size_t acceptResultSize = sizeof(recvFromData);
  if (op->info.internalBuffer == 0) {
    op->info.internalBuffer = malloc(acceptResultSize);
    op->info.internalBufferSize = acceptResultSize;
  }
  else if (op->info.internalBufferSize < acceptResultSize) {
    op->info.internalBuffer = realloc(op->info.internalBuffer, acceptResultSize);
    op->info.internalBufferSize = acceptResultSize;
  }

  recvFromData *rf = op->info.internalBuffer;
  rf->size = sizeof(rf->addr);
  DWORD flags = 0;
  // TODO: correct processing >4Gb data blocks
  wsabuf.buf = op->info.buffer;
  wsabuf.len = (ULONG)op->info.transactionSize;

  memset(&op->overlapped, 0, sizeof(op->overlapped));
  int result = WSARecvFrom(object->hSocket, &wsabuf, 1, NULL, &flags, (SOCKADDR*)&rf->addr, &rf->size, &op->overlapped, NULL);
  if (result == 0 || WSAGetLastError() == WSA_IO_PENDING) {
    return aosPending;
  } else {
    return aosUnknownError;
  }
}


AsyncOpStatus iocpAsyncWriteMsg(asyncOpRoot *opptr)
{
  WSABUF wsabuf;
  iocpOp *op = (iocpOp*)opptr;
  struct sockaddr_in remoteAddress;
  aioObject *object = getObject(op);

  // TODO: correct processing >4Gb data blocks
  wsabuf.buf = op->info.buffer;
  wsabuf.len = (ULONG)op->info.transactionSize;
  remoteAddress.sin_family = op->info.host.family;
  remoteAddress.sin_addr.s_addr = op->info.host.ipv4;
  remoteAddress.sin_port = op->info.host.port;
  memset(&op->overlapped, 0, sizeof(op->overlapped));
  int result = WSASendTo(object->hSocket, &wsabuf, 1, NULL, 0, (struct sockaddr*)&remoteAddress, sizeof(remoteAddress), &op->overlapped, NULL);
  if (result == 0 || WSAGetLastError() == WSA_IO_PENDING) {
    return aosPending;
  } else {
    return aosUnknownError;
  }
}
