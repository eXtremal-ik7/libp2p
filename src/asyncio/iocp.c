#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>

#include "asyncioImpl.h"
#include "atomic.h"
#include <time.h>


typedef struct iocpOp iocpOp;

typedef enum iocpOpStateTy {
  iocpStatePending = 0,
  iocpStateRunning,
  iocpStatePacketWaiting
} iocpOpStateTy;

typedef struct recvFromData {
  struct sockaddr_in addr;
  INT size;
} recvFromData;

typedef struct iocpBase {
  asyncBase B;
  HANDLE completionPort;
  LPFN_CONNECTEX ConnectExPtr;
} iocpBase;

typedef struct iocpOp {
  asyncOp info;
  iocpOpStateTy state;
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
void iocpDeleteObject(aioObject *op);
void iocpInitializeTimer(asyncBase *base, asyncOpRoot *op);
void iocpStartTimer(asyncOpRoot *op, uint64_t usTimeout, int periodic);
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

static HANDLE getHandle(aioObject *object)
{
  switch (object->root.type) {
  case ioObjectDevice:
    return object->hDevice;
  case ioObjectSocket:
    return (HANDLE)object->hSocket;
  default:
    return INVALID_HANDLE_VALUE;
  }
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
      else if (error == ERROR_NETNAME_DELETED)
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
    signalTime.QuadPart = -(int64_t)(event->timeout * 10);
    SetWaitableTimer(timer->hTimer, &signalTime, 0, userEventTimerCb, timer, FALSE);
  }
}


static VOID CALLBACK ioFinishedTimerCb(LPVOID lpArgToCompletionRoutine, DWORD dwTimerLowValue, DWORD dwTimerHighValue)
{
  aioTimer *timer;
  tag_t timerTag;
  __tagged_pointer_decode(lpArgToCompletionRoutine, (void**)&timer, &timerTag);
  opCancel(timer->op, opEncodeTag(timer->op, timerTag), aosTimeout);
}

static void opRun(asyncOpRoot *op, List *list)
{
  eqPushBack(list, op);
  if (op->timeout) {
    asyncBase *base = op->object->base;
    if (op->flags & afRealtime) {
      // start timer for this operation
      base->methodImpl.startTimer(op, op->timeout, op->opCode == actUserEvent);
    } else {
      // add operation to timeout grid
      op->endTime = ((uint64_t)time(0)) * 1000000ULL + op->timeout;
      addToTimeoutQueue(base, op);
    }
  }
}

void processAction(asyncOpRoot *opptr, AsyncOpActionTy actionType, List *finished, tag_t *needStart)
{
  List *list = 0;
  tag_t tag = 0;
  int restart = 0;
  aioObjectRoot *object = opptr->object;
  int isIocp = object->type == ioObjectDevice || object->type == ioObjectSocket;
  if (opptr->opCode & OPCODE_WRITE) {
    list = &object->writeQueue;
    tag = TAG_WRITE;
  } else {
    list = &object->readQueue;
    tag = TAG_READ;
  }

  asyncOpRoot *queueHead = list->head;

  switch (actionType) {
    case aaStart : {
      opRun(opptr, list);
      if (object->type != ioObjectUserDefined) {
        iocpOp *op = (iocpOp*)opptr;
        memset(&op->overlapped, 0, sizeof(op->overlapped));
        op->state = iocpStatePending;
      }

      break;
    }

    case aaFinish : {
      if (isIocp) {
        iocpOp *op = (iocpOp*)opptr;
        if (op->state == iocpStateRunning) {
          CancelIoEx(getHandle((aioObject*)object), &op->overlapped);
          op->state = iocpStatePacketWaiting;
        } else {
          opRelease(opptr, opGetStatus(opptr), list, finished);
        }
      } else {
        opRelease(opptr, opGetStatus(opptr), list, finished);
      }
      break;
    }

    case aaIOCPPacket : {
      opRelease(opptr, opGetStatus(opptr), list, finished);
      break;
    }

    case aaIOCPRestart : {
      iocpOp *op = (iocpOp*)opptr;
      if (op->state == iocpStateRunning) {
        memset(&op->overlapped, 0, sizeof(op->overlapped));
        op->state = iocpStatePending;
        restart = 1;
      } else if (op->state == iocpStatePacketWaiting) {
        opRelease(opptr, opGetStatus(opptr), list, finished);
      }
      break;
    }
  }

  *needStart |= (list->head && (list->head != queueHead || restart)) ? tag : 0;
}

static void iocpCancelOperationList(List *list, List *finished, AsyncOpStatus status)
{
  iocpOp *op = (iocpOp*)list->head;
  while (op) {
    asyncOpRoot *opptr = &op->info.root;
    iocpOp *next = (iocpOp*)opptr->executeQueue.next;
    if (opSetStatus(opptr, opGetGeneration(opptr), status)) {
      if (op->state == iocpStateRunning) {
        CancelIoEx(getHandle((aioObject*)opptr->object), &op->overlapped);
        op->state = iocpStatePacketWaiting;
      } else {
        opRelease(opptr, aosCanceled, list, finished);
      }
    }

    op = next;
  }
}

void combiner(aioObjectRoot *object, tag_t tag, asyncOpRoot *op, AsyncOpActionTy actionType)
{
  tag_t currentTag = tag;
  asyncOpRoot *newOp = op;

  while (currentTag) {
    if (currentTag & TAG_CANCELIO) {
      if (object->type == ioObjectDevice || object->type == ioObjectSocket) {
        iocpCancelOperationList(&object->readQueue, &threadLocalQueue, aosCanceled);
        iocpCancelOperationList(&object->writeQueue, &threadLocalQueue, aosCanceled);
      } else {
        cancelOperationList(&object->readQueue, &threadLocalQueue, aosCanceled);
        cancelOperationList(&object->writeQueue, &threadLocalQueue, aosCanceled);
      }
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
        processOperationList(object, &threadLocalQueue, &needStart, processAction, &enqueuedOperationsNum);
    }

    if (needStart & TAG_READ_MASK)
      executeOperationList(&object->readQueue, &threadLocalQueue);
    if (needStart & TAG_WRITE_MASK)
      executeOperationList(&object->writeQueue, &threadLocalQueue);

    // Try exit combiner
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4146)
#endif
    tag_t processed = __tag_make_processed(currentTag, enqueuedOperationsNum);
    currentTag = __tag_atomic_fetch_and_add(&object->tag, -processed);
    currentTag -= processed;
#ifdef _MSC_VER
#pragma warning(pop)
#endif
  }
}

void postEmptyOperation(asyncBase *base)
{
  iocpBase *localBase = (iocpBase*)base;
  PostQueuedCompletionStatus(localBase->completionPort, 0, 0, 0);
}


asyncBase *iocpNewAsyncBase()
{
  iocpBase *base = malloc(sizeof(iocpBase));
  if (base) {
    SOCKET tmpSocket;
    DWORD numBytes = 0;
    GUID guid = WSAID_CONNECTEX;

    base->completionPort =
      CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0);

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
      TRUE);

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
      } else if (entry->lpOverlapped) {
        iocpOp *op = (iocpOp*)(((uint8_t*)entry->lpOverlapped) - offsetof(struct iocpOp, overlapped));
        AsyncOpStatus result = iocpGetOverlappedResult(op);
        if (result == aosSuccess) {
          op->info.bytesTransferred += entry->dwNumberOfBytesTransferred;
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
            if ((op->info.root.flags & afWaitAll) && op->info.bytesTransferred < op->info.transactionSize) {
              combinerCall(op->info.root.object, 1, &op->info.root, aaIOCPRestart);
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
        combinerCall(op->info.root.object, 1, &op->info.root, aaIOCPPacket);
      } else {
        while (threadLocalQueue.head)
          executeThreadLocalQueue();
        __uint_atomic_fetch_and_add(&base->messageLoopThreadCounter, -1);
        return;
      }
    }
  }
}


aioObject *iocpNewAioObject(asyncBase *base, IoObjectTy type, void *data)
{
  iocpBase *localBase = (iocpBase*)base;
  aioObject *object = malloc(sizeof(aioObject));
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

  free(object);
}

void iocpInitializeTimer(asyncBase *base, asyncOpRoot *op)
{
  aioTimer *timer = __tagged_alloc(sizeof(aioTimer));
  timer->op = op;
  timer->hTimer = CreateWaitableTimer(NULL, FALSE, NULL);
  op->timerId = timer;
}

void iocpStartTimer(asyncOpRoot *op, uint64_t usTimeout, int periodic)
{
  LARGE_INTEGER signalTime;
  signalTime.QuadPart = -(int64_t)(usTimeout * 10);

  PTIMERAPCROUTINE timerCb;
  switch (op->opCode) {
  case actUserEvent:
    timerCb = userEventTimerCb;
    break;
  default:
    timerCb = ioFinishedTimerCb;
    break;
  }

  aioTimer *timer = (aioTimer*)op->timerId;
  SetWaitableTimer(timer->hTimer, &signalTime, 0, timerCb, __tagged_pointer_make(timer, opGetGeneration(op)), FALSE);
}


void iocpStopTimer(asyncOpRoot *op)
{
  CancelWaitableTimer((HANDLE)op->timerId);
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

  int result = AcceptEx(object->hSocket,
                        op->info.acceptSocket,
                        op->info.internalBuffer,
                        0,
                        sizeof(struct sockaddr_in) + 16,
                        sizeof(struct sockaddr_in) + 16,
                        NULL,
                        &op->overlapped);

  if (result == 0 || WSAGetLastError() == WSA_IO_PENDING) {
    op->state = iocpStateRunning;
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
  DWORD flags = 0;
  // TODO: correct processing >4Gb data blocks
  wsabuf.buf = op->info.buffer;
  wsabuf.len = (ULONG)op->info.transactionSize;
  int result = WSARecv(object->hSocket, &wsabuf, 1, NULL, &flags, &op->overlapped, NULL);
  if (result == 0 || WSAGetLastError() == WSA_IO_PENDING) {
    op->state = iocpStateRunning;
    return aosPending;
  } else {
    return aosUnknownError;
  }
}


AsyncOpStatus iocpAsyncWrite(asyncOpRoot *opptr)
{
  WSABUF wsabuf;
  iocpOp *op = (iocpOp*)opptr;
  aioObject *object = getObject(op);
  // TODO: correct processing >4Gb data blocks
  wsabuf.buf = op->info.buffer;
  wsabuf.len = (ULONG)op->info.transactionSize;
  int result = WSASend(object->hSocket, &wsabuf, 1, NULL, 0, &op->overlapped, NULL);
  if (result == 0 || WSAGetLastError() == WSA_IO_PENDING) {
    op->state = iocpStateRunning;
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

  int result = WSARecvFrom(object->hSocket, &wsabuf, 1, NULL, &flags, (SOCKADDR*)&rf->addr, &rf->size, &op->overlapped, NULL);
  if (result == 0 || WSAGetLastError() == WSA_IO_PENDING) {
    op->state = iocpStateRunning;
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
  int result = WSASendTo(object->hSocket, &wsabuf, 1, NULL, 0, (struct sockaddr*)&remoteAddress, sizeof(remoteAddress), &op->overlapped, NULL);
  if (result == 0 || WSAGetLastError() == WSA_IO_PENDING) {
    op->state = iocpStateRunning;
    return aosPending;
  } else {
    return aosUnknownError;
  }
}
