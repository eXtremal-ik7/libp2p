#include <Winsock2.h>
#include <mswsock.h>
#include <Windows.h>
#include "asyncioInternal.h"
#include "redblacktree.h"
#include "asyncio/dynamicBuffer.h"
#include <stdio.h>
#include <assert.h>

#define SET_CHANGED WAIT_OBJECT_0
#define SOCK_READY_IO ((WAIT_OBJECT_0) + 1)
#define WAITING_EVENTS 2

static const size_t minimalBufferSize = 1024;

typedef struct asyncOp asyncOp;

typedef struct overlappedExtra {
  OVERLAPPED data;
  asyncOp *op;
  size_t bufferSize;
} overlappedExtra;

typedef struct iocpBase {
  asyncBase B;
  HANDLE completionPort;
  HANDLE timerThread;
  LPFN_CONNECTEX ConnectExPtr;
  asyncOp *lastReturned;
  /* For synchronous sockets */
  HANDLE threadSockSyn;
  HANDLE mutexSockSyn;
  HANDLE sckReadyIO;
  HANDLE setChanged;
  /*                         */
  TreePtr socketTree;
} iocpBase;

struct asyncOp {
  aioInfo info;
  HANDLE hTimer;
  LARGE_INTEGER signalTime;
  int counter;
  overlappedExtra *overlapped;
};


void postEmptyOperation(asyncBase *base);
void iocpNextFinishedOperation(asyncBase *base);
aioObject *iocpNewAioObject(asyncBase *base, IoObjectTy type, void *data);
asyncOp *iocpNewAsyncOp(asyncBase *base);
void iocpDeleteOp(asyncOp *op);
void iocpStartTimer(asyncOp *op, uint64_t usTimeout, int count);
void iocpStopTimer(asyncOp *op);
void iocpActivate(asyncOp *op);
void iocpAsyncConnect(asyncOp *op, const HostAddress *address, uint64_t usTimeout);
void iocpAsyncAccept(asyncOp *op, uint64_t usTimeout);
void iocpAsyncRead(asyncOp *op, uint64_t usTimeout);
void iocpAsyncWrite(asyncOp *op, uint64_t usTimeout);
void iocpAsyncReadMsg(asyncOp *op, uint64_t usTimeout);
void iocpAsyncWriteMsg(asyncOp *op, const HostAddress *address, uint64_t usTimeout);
void iocpMonitor(asyncOp *op);
void iocpMonitorStop(asyncOp *op);

static struct asyncImpl iocpImpl = {
  postEmptyOperation,
  iocpNextFinishedOperation,
  iocpNewAioObject,
  iocpNewAsyncOp,
  iocpDeleteOp,
  iocpStartTimer,
  iocpStopTimer,
  iocpActivate,
  iocpAsyncConnect,
  iocpAsyncAccept,
  iocpAsyncRead,
  iocpAsyncWrite,
  iocpAsyncReadMsg,
  iocpAsyncWriteMsg,
  iocpMonitor,
  iocpMonitorStop
};


static DWORD WINAPI timerThreadProc(LPVOID lpParameter)
{
  while (1)
    SleepEx(INFINITE, TRUE);
  return 0;
}

static DWORD WINAPI threadSocketSyn(LPVOID lpParameter)
{
  int waitRet, bytes;
  HANDLE eventArray[WAITING_EVENTS];
  treeNodePtr iter;
  TreePtr test;
  iocpBase *base = (iocpBase*)lpParameter;
  eventArray[0] = base->setChanged;
  eventArray[1] = base->sckReadyIO;

  while (TRUE) {
    waitRet = WaitForMultipleObjects(
      WAITING_EVENTS,
      eventArray,
      FALSE,
      INFINITE
      );
    if (waitRet == WAIT_OBJECT_0) {
      fprintf(stderr, "State changed\n");
    } else {
      if (WaitForSingleObject(base->mutexSockSyn, INFINITE) == WAIT_OBJECT_0) {
        //mutex
        test = base->socketTree;
        assert(!treeIsEmpty(base->socketTree));
        for(iter = getBegin(base->socketTree); iter != getEnd(base->socketTree); iter = nextNode(iter)) {
          ioctlsocket(iter->keyValue.sock_, FIONREAD, &bytes);
          if (bytes > 0) {
            fprintf(stderr, "Socket %u bytes: %u\n", iter->keyValue.sock_, bytes);
            PostQueuedCompletionStatus(base->completionPort, 0, (ULONG_PTR)iter->keyValue.data, 0);
          }
        }
        ReleaseMutex(base->mutexSockSyn);
      }
    }

  }
}


static void *queryBuffer(asyncOp *op, size_t size)
{
  if (!op->overlapped || op->overlapped->bufferSize < size) {
    size_t newBufferSize =
      (size < minimalBufferSize) ? minimalBufferSize : size;

    if (op->overlapped)
      free(op->overlapped);
    
    op->overlapped = malloc(sizeof(overlappedExtra) + newBufferSize);
    op->overlapped->op = op;
    op->overlapped->bufferSize = newBufferSize;
    memset(op->overlapped, 0, sizeof(OVERLAPPED));
  }

  return ((uint8_t*)op->overlapped) + sizeof(overlappedExtra);
}


static void deleteAsyncOperation(asyncOp *op)
{
  if (op->hTimer)
    CloseHandle(op->hTimer);
  free(op);  
}


static VOID CALLBACK userEventTimerCb(LPVOID lpArgToCompletionRoutine,
                                      DWORD dwTimerLowValue,
                                      DWORD dwTimerHighValue)
{
  int needReactivate = 1;
  asyncOp *op = lpArgToCompletionRoutine;
  iocpBase *localBase = (iocpBase*)op->info.object->base;
  PostQueuedCompletionStatus(localBase->completionPort, 0, (ULONG_PTR)op, 0);

  if (op->counter > 0) {
    if (--op->counter == 0)
      needReactivate = 0;
  }

  if (needReactivate) {
    SetWaitableTimer(op->hTimer, &op->signalTime, 0,
                     userEventTimerCb, op, FALSE);
  }
}


static VOID CALLBACK ioFinishedTimerCb(LPVOID lpArgToCompletionRoutine,
                                       DWORD dwTimerLowValue,
                                       DWORD dwTimerHighValue)
{
  // Принудительное завершение асинхронной операции
  asyncOp *op = lpArgToCompletionRoutine;
  iocpBase *localBase = (iocpBase*)op->info.object->base;

  PostQueuedCompletionStatus(localBase->completionPort, 0, (ULONG_PTR)op, 0);
}


static VOID CALLBACK timerStartProc(ULONG_PTR dwParam)
{
  asyncOp *op = (asyncOp*)dwParam;
  PTIMERAPCROUTINE timerCb;
  
  switch (op->info.object->type) {
    case ioObjectUserEvent :
      timerCb = userEventTimerCb;
      break;
    default:
      timerCb = ioFinishedTimerCb;
      break;
  }

  SetWaitableTimer(op->hTimer, &op->signalTime, 0, timerCb, op, FALSE);
}


static VOID CALLBACK timerCancelProc(ULONG_PTR dwParam)
{
  asyncOp *op = (asyncOp*)dwParam;
  CancelWaitableTimer(op->hTimer);
}


static void startOrContinueAction(asyncOp *op, size_t size)
{
  WSABUF wsabuf;
  iocpBase *localBase = (iocpBase*)op->info.object->base;
  descrStruct treeStruct;
  void *opBuffer = ((uint8_t*)op->overlapped) + sizeof(overlappedExtra);

  switch (op->info.object->type) {
    case ioObjectSocket :
      switch (op->info.currentAction) {
        case ioConnect :
          if (localBase->ConnectExPtr)
            localBase->ConnectExPtr(op->info.object->hSocket,
                                    opBuffer,
                                    size,
                                    NULL,
                                    0,
                                    NULL,
                                    &op->overlapped->data);
          break;
        case ioAccept :
          op->info.acceptSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
          AcceptEx(op->info.object->hSocket,
                   op->info.acceptSocket,
                   opBuffer,
                   size,
                   sizeof(struct sockaddr_in)+16,
                   sizeof(struct sockaddr_in)+16,
                   NULL,
                   &op->overlapped->data);
          break;
        case ioRead :
        case ioReadMsg : {
          DWORD flags = 0;
          wsabuf.buf = opBuffer;
          wsabuf.len = size;
          WSARecv(op->info.object->hSocket, &wsabuf, 1, NULL, &flags, &op->overlapped->data, NULL);
          break;
        }
        case ioWrite :
          wsabuf.buf = opBuffer;
          wsabuf.len = size;
          WSASend(op->info.object->hSocket, &wsabuf, 1, NULL, 0, &op->overlapped->data, NULL);
          break;

        case ioWriteMsg : {
          struct sockaddr_in remoteAddress;
          wsabuf.buf = opBuffer;
          wsabuf.len = size;

          remoteAddress.sin_family = op->info.host.family;
          remoteAddress.sin_addr.s_addr = op->info.host.ipv4;
          remoteAddress.sin_port = op->info.host.port;

          WSASendTo(op->info.object->hSocket, &wsabuf, 1, NULL, 0, (struct sockaddr*)&remoteAddress, sizeof(remoteAddress), &op->overlapped->data, NULL);  
          break;
        }

        default :
          return;
      }
      break;

    case ioObjectDevice :
      switch (op->info.currentAction) {
        case ioRead :
        case ioReadMsg : {
          BOOL result = ReadFile(op->info.object->hDevice,
                                 opBuffer, size, NULL,
                                 &op->overlapped->data);
          break;
       }
        case ioWrite :
          WriteFile(op->info.object->hDevice, opBuffer, size, NULL, &op->overlapped->data);
          break;
        default :
          return;
      }
      break;
    case ioObjectSocketSyn :
      switch (op->info.currentAction) {
        case ioMonitor : { 
          if (WaitForSingleObject(localBase->mutexSockSyn, INFINITE) == WAIT_OBJECT_0) {   
            WSAEventSelect(
              op->info.object->hSocket,
              localBase->sckReadyIO,
              FD_READ
              );
            treeStruct.data = (void*)op;
            treeStruct.sock_ = op->info.object->hSocket;
            insertValue(localBase->socketTree, treeStruct);

            ReleaseMutex(localBase->mutexSockSyn);
            SetEvent(localBase->setChanged);
          }
        }
        break;
        case ioMonitorStop : {
          if (WaitForSingleObject(localBase->mutexSockSyn, INFINITE) == WAIT_OBJECT_0) {
            WSAEventSelect(
              op->info.object->hSocket,
              localBase->sckReadyIO,
              0
              );
            treeStruct.data = (void*)op;
            treeStruct.sock_ = op->info.object->hSocket;
            deleteValue(localBase->socketTree, treeStruct);
            cqueue_ptr_push(&localBase->B.asyncOps, op);

 
            ReleaseMutex(localBase->mutexSockSyn);
            SetEvent(localBase->setChanged);
          }
        }
        default:
          return;
      }

    
    default :
      return;

  }
}


static void newAction(asyncOp *op,
                      IoActionTy actionType,
                      void *buffer,
                      size_t size,
                      uint64_t usTimeout)
{
  void *opBuffer = queryBuffer(op, size);
  if (actionType == ioConnect ||
      actionType == ioWrite ||
      actionType == ioWriteMsg)
    memcpy(opBuffer, buffer, size);

  op->info.currentAction = actionType;
  if (op->info.currentAction == ioMonitor)
    op->info.status = aosMonitoring;
  else
    op->info.status = aosPending;

  startOrContinueAction(op, size);
  if (usTimeout != 0) {
    iocpBase *localBase = (iocpBase*)op->info.object->base;
    op->signalTime.QuadPart = -(int64_t)(usTimeout*10);
    QueueUserAPC(timerStartProc, localBase->timerThread, (ULONG_PTR)op);
  }
}


static void dispatchOp(asyncOp *op)
{
  iocpBase *localBase = (iocpBase*)op->info.object->base;
  if (op->info.callback)
    op->info.callback(&op->info);
  
  if (op->info.object->type != ioObjectUserEvent &&
      op->info.object->type != ioObjectSocketSyn) {
    QueueUserAPC(timerCancelProc, localBase->timerThread, (ULONG_PTR)op);
    cqueue_ptr_push(&localBase->B.asyncOps, op);
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
    DWORD tid;
    
    cqueue_ptr_init(&base->B.asyncOps, 64);
    
    base->completionPort =
      CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0);
    base->timerThread =
      CreateThread(NULL, 0x10000, timerThreadProc, NULL, THREAD_PRIORITY_NORMAL, &tid);
    base->threadSockSyn = 
      CreateThread(NULL, 0x10000, threadSocketSyn, (LPVOID)base, THREAD_PRIORITY_NORMAL, &tid);

    base->socketTree = malloc(sizeof(Tree));
    initTree(base->socketTree);
    
    base->mutexSockSyn = CreateMutex(
      NULL,
      FALSE,
      NULL
      );

    base->setChanged = CreateEvent(
      NULL,
      FALSE,
      FALSE,
      TEXT("Set of sockets changed")
      );

    base->sckReadyIO = CreateEvent(
      NULL,
      FALSE,
      FALSE,
      TEXT("socketReadyIO")
      );

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
    base->lastReturned = 0;
  }

  return (asyncBase*)base;
}


void iocpNextFinishedOperation(asyncBase *base)
{
  iocpBase *localBase = (iocpBase*)base;
  DWORD bytesNum = 0;
  ULONG_PTR key = 0;
  overlappedExtra *overlapped = 0;
  asyncOp *op = 0;

  if (localBase->lastReturned) {
    cqueue_ptr_push(&localBase->B.asyncOps, localBase->lastReturned);
    localBase->lastReturned = 0;
  }

  while (1) {
    BOOL status = GetQueuedCompletionStatus(localBase->completionPort,
                                            &bytesNum,
                                            &key,
                                            (OVERLAPPED**)&overlapped,
                                            INFINITE);

    if (key) {
      op = (asyncOp*)key;
      if (op->info.object->type != ioObjectUserEvent &&
          op->info.object->type != ioObjectSocketSyn) {       
        op->info.status = aosTimeout;
        op->overlapped->op = 0;
        op->overlapped = 0;
      }
    } else if (overlapped) {
      op = overlapped->op;
      if (!op) {
        free(overlapped);
        continue;
      }

      if (status == FALSE) {
        if (op->info.object->type == ioObjectSocket) {
          int error = WSAGetLastError();
          if (error == WSAEMSGSIZE) {
            if (op->info.currentAction == ioReadMsg) {
              u_long remaining = 0;
              ioctlsocket(op->info.object->hSocket, FIONREAD, &remaining);
              startOrContinueAction(op, remaining);
            } else {
            }
          } else if (error == ERROR_NETNAME_DELETED) {
            op->info.status = aosDisconnected;
    
          } else {
            op->info.status = aosUnknownError;
          }
        }
      } else {
        void *opBuffer = ((uint8_t*)op->overlapped) + sizeof(overlappedExtra); 
        op->info.status = aosSuccess;
        if (op->info.currentAction == ioAccept) {
          struct sockaddr_in *localAddr = 0;
          struct sockaddr_in *remoteAddr = 0;
          INT localAddrLength;
          INT remoteAddrLength;
          GetAcceptExSockaddrs(opBuffer,
                               bytesNum,
                               sizeof(struct sockaddr_in)+16,
                               sizeof(struct sockaddr_in)+16,
                               (struct sockaddr**)&localAddr, &localAddrLength,
                               (struct sockaddr**)&remoteAddr, &remoteAddrLength);
          if (localAddr && remoteAddr) {
            op->info.host.family = remoteAddr->sin_family;
            op->info.host.ipv4 = remoteAddr->sin_addr.s_addr;
            op->info.host.port = remoteAddr->sin_port;       
          } else {
            op->info.status = aosUnknownError;
          }
        } else if (op->info.currentAction == ioRead) {
          void *ptr = (uint8_t*)op->info.buffer + op->info.bytesTransferred;
          memcpy(ptr, opBuffer, bytesNum);
          op->info.bytesTransferred += bytesNum;
          if ((op->info.flags & afWaitAll) &&
              op->info.bytesTransferred < op->info.transactionSize) {
            startOrContinueAction(op, op->info.transactionSize - op->info.bytesTransferred);
            continue;
          }
        } else if (op->info.currentAction == ioReadMsg) {
          void *ptr = dynamicBufferAlloc(op->info.dynamicArray, bytesNum);
          memcpy(ptr, opBuffer, bytesNum);
          op->info.bytesTransferred += bytesNum;
        }
      }
    } else {
      return; 
    }

    dispatchOp(op);
  }
}


aioObject *iocpNewAioObject(asyncBase *base, IoObjectTy type, void *data)
{
  iocpBase *localBase = (iocpBase*)base;
  aioObject *object = malloc(sizeof(aioObject));
  object->base = base;
  object->type = type;
  switch (object->type) {
    case ioObjectDevice :
      object->hDevice = *(iodevTy*)data;
      CreateIoCompletionPort(object->hDevice, localBase->completionPort, 0, 1);
      break;
    case ioObjectSocket :
      object->hSocket = *(socketTy*)data;
      CreateIoCompletionPort((HANDLE)object->hSocket, localBase->completionPort, 0, 1);
      break;
    case ioObjectSocketSyn : 
      object->hSocket = *(socketTy*)data;
      break;

  }

  return object;
}


asyncOp *iocpNewAsyncOp(asyncBase *base)
{
  asyncOp *op;
  iocpBase *localBase = (iocpBase*)base;
  op = malloc(sizeof(asyncOp));
  if (op) {
    memset(op, 0, sizeof(asyncOp));
    op->hTimer = CreateWaitableTimer(NULL, FALSE, NULL);
  }

  return (asyncOp*)op;
}


void iocpDeleteOp(asyncOp *op)
{
  deleteAsyncOperation((asyncOp*)op);
}


void iocpStartTimer(asyncOp *op, uint64_t usTimeout, int count)
{
  iocpBase *localBase = (iocpBase*)op->info.object->base;
  op->signalTime.QuadPart = -(int64_t)(usTimeout*10);
  op->counter = count;
  if (count != 0)
    QueueUserAPC(timerStartProc, localBase->timerThread, (ULONG_PTR)op);  
}


void iocpStopTimer(asyncOp *op)
{
  iocpBase *localBase = (iocpBase*)op->info.object->base;
  QueueUserAPC(timerCancelProc, localBase->timerThread, (ULONG_PTR)op);
}


void iocpActivate(asyncOp *op)
{
  iocpBase *localBase = (iocpBase*)op->info.object->base;
  PostQueuedCompletionStatus(localBase->completionPort, 0, (ULONG_PTR)op, 0);
}


void iocpAsyncConnect(asyncOp *op, const HostAddress *address, uint64_t usTimeout)
{
  struct sockaddr_in localAddress;
  localAddress.sin_family = address->family;
  localAddress.sin_addr.s_addr = address->ipv4;
  localAddress.sin_port = address->port;
  newAction((asyncOp*)op,
            ioConnect,
            &localAddress,
            sizeof(localAddress),
            usTimeout);
}


void iocpAsyncAccept(asyncOp *op, uint64_t usTimeout)
{
  newAction(op, ioAccept, 0, 0, usTimeout);
}


void iocpAsyncRead(asyncOp *op, uint64_t usTimeout)
{
  newAction(op, ioRead, op->info.buffer, op->info.transactionSize, usTimeout);
}


void iocpAsyncWrite(asyncOp *op, uint64_t usTimeout)
{
  newAction(op, ioWrite, op->info.buffer, op->info.transactionSize, usTimeout);  
}


void iocpAsyncReadMsg(asyncOp *op, uint64_t usTimeout)
{
  newAction(op, ioReadMsg, 0, minimalBufferSize, usTimeout);
}


void iocpAsyncWriteMsg(asyncOp *op, const HostAddress *address, uint64_t usTimeout)
{
  op->info.host = *address;
  newAction(op, ioWriteMsg, op->info.buffer, op->info.transactionSize, usTimeout);
}

void iocpMonitor(asyncOp *op)
{
  newAction((asyncOp*)op, ioMonitor, 0, minimalBufferSize, 0);
}

void iocpMonitorStop(asyncOp *op)
{
  newAction((asyncOp*)op, ioMonitorStop, 0, minimalBufferSize, 0);
}