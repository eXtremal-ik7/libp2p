#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>
#include "asyncioImpl.h"
#include "asyncio/dynamicBuffer.h"
#include <stdio.h>
#include <assert.h>


typedef struct iocpOp iocpOp;

typedef struct overlappedExtra {
  OVERLAPPED data;
  iocpOp *op;
} overlappedExtra;

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
  overlappedExtra overlapped;
} iocpOp;


void postEmptyOperation(asyncBase *base);
void iocpNextFinishedOperation(asyncBase *base);
aioObject *iocpNewAioObject(iocpBase *base, IoObjectTy type, void *data);
asyncOpRoot *iocpNewAsyncOp(asyncBase *base);
void iocpDeleteObject(aioObject *op);
void iocpFinishOp(iocpOp *op);
void iocpInitializeTimer(asyncOpRoot *op);
void iocpStartTimer(asyncOpRoot *op, uint64_t usTimeout);
void iocpStopTimer(asyncOpRoot *op);
void iocpActivate(asyncOpRoot *op);
void iocpAsyncConnect(iocpOp *op, const HostAddress *address, uint64_t usTimeout);
void iocpAsyncAccept(iocpOp *op, uint64_t usTimeout);
void iocpAsyncRead(iocpOp *op, uint64_t usTimeout);
void iocpAsyncWrite(iocpOp *op, uint64_t usTimeout);
void iocpAsyncReadMsg(iocpOp *op, uint64_t usTimeout);
void iocpAsyncWriteMsg(iocpOp *op, const HostAddress *address, uint64_t usTimeout);

static struct asyncImpl iocpImpl = {
  postEmptyOperation,
  iocpNextFinishedOperation,
  (newAioObjectTy*)iocpNewAioObject,
  iocpNewAsyncOp,
  iocpDeleteObject,
  (finishOpTy*)iocpFinishOp,
  iocpInitializeTimer,
  iocpStartTimer,
  iocpStopTimer,
  iocpActivate,
  (asyncConnectTy*)iocpAsyncConnect,
  (asyncAcceptTy*)iocpAsyncAccept,
  (asyncReadTy*)iocpAsyncRead,
  (asyncWriteTy*)iocpAsyncWrite,
  (asyncReadMsgTy*)iocpAsyncReadMsg,
  (asyncWriteMsgTy*)iocpAsyncWriteMsg
};

static aioObject *getObject(iocpOp *op)
{
  return (aioObject*)op->info.root.object;
}

static AsyncOpStatus getOperationStatus(iocpOp *op)
{
  DWORD bytesTransferred;
  DWORD flags;
  BOOL result;
  aioObject *object = getObject(op);
  if (object->root.type == ioObjectSocket) {
    result = WSAGetOverlappedResult(object->hSocket, &op->overlapped.data, &bytesTransferred, FALSE, &flags);
    if (result == TRUE) {
      // Check for disconnect
      if ((op->info.root.opCode == actRead || op->info.root.opCode == actWrite) &&
          bytesTransferred == 0 &&
          op->info.transactionSize > 0) {
        return aosDisconnected;
      }
      return aosSuccess;
    } else {
      int error = WSAGetLastError();
      if (error == WSAEMSGSIZE)
        return aosBufferTooSmall;
      else if (error == ERROR_NETNAME_DELETED)
        result = aosDisconnected;
      else
        return aosUnknownError;
    }
  } else {
    result = GetOverlappedResult(object->hDevice, &op->overlapped.data, &bytesTransferred, FALSE);
    return result == TRUE ? aosSuccess : aosUnknownError;
  }


}

static DWORD WINAPI timerThreadProc(LPVOID lpParameter)
{
  while (1)
    SleepEx(INFINITE, TRUE);
  return 0;
}

static VOID CALLBACK userEventTimerCb(LPVOID lpArgToCompletionRoutine,
  DWORD dwTimerLowValue,
  DWORD dwTimerHighValue)
{
  int needReactivate = 1;
  aioUserEvent *event = lpArgToCompletionRoutine;
  iocpBase *localBase = (iocpBase*)event->root.base;
  PostQueuedCompletionStatus(localBase->completionPort, 0, (ULONG_PTR)event, 0);

  if (event->counter > 0) {
    if (--event->counter == 0)
      needReactivate = 0;
  }

  if (needReactivate) {
    LARGE_INTEGER signalTime;
    signalTime.QuadPart = -(int64_t)(event->timeout * 10);
    SetWaitableTimer(event->root.timerId, &signalTime, 0, userEventTimerCb, event, FALSE);
  }
}


static VOID CALLBACK ioFinishedTimerCb(LPVOID lpArgToCompletionRoutine,
                                       DWORD dwTimerLowValue,
                                       DWORD dwTimerHighValue)
{
  // TODO: use CancelIoEx
  iocpOp *op = lpArgToCompletionRoutine;
  iocpBase *localBase = (iocpBase*)op->info.root.base;

  PostQueuedCompletionStatus(localBase->completionPort, 0, (ULONG_PTR)op, 0);
}

static void initializeOp(iocpOp *op)
{
  memset(&op->overlapped.data, 0, sizeof(op->overlapped.data));
  op->overlapped.op = op;
}

static void startOrContinueAction(iocpOp *op, void *arg)
{
  WSABUF wsabuf;
  iocpBase *localBase = (iocpBase*)op->info.root.base;
  aioObject *object = getObject(op);
  void *opBuffer = ((uint8_t*)op->info.buffer) + op->info.bytesTransferred;
  size_t size = op->info.transactionSize - op->info.bytesTransferred;

  switch (object->root.type) {
    case ioObjectSocket :
      switch (op->info.root.opCode) {
        case actConnect :
          if (localBase->ConnectExPtr)
            localBase->ConnectExPtr(object->hSocket,
                                    arg,
                                    sizeof(struct sockaddr_in),
                                    NULL,
                                    0,
                                    NULL,
                                    &op->overlapped.data);
          break;
        case actAccept :
          op->info.acceptSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
          AcceptEx(object->hSocket,
                   op->info.acceptSocket,
                   op->info.internalBuffer,
                   0,
                   sizeof(struct sockaddr_in)+16,
                   sizeof(struct sockaddr_in)+16,
                   NULL,
                   &op->overlapped.data);
          break;
        case actRead : {
          DWORD flags = 0;
          wsabuf.buf = opBuffer;
          wsabuf.len = size;
          WSARecv(object->hSocket, &wsabuf, 1, NULL, &flags, &op->overlapped.data, NULL);
          break;
        }
        
        case actReadMsg : {
          recvFromData *rf = op->info.internalBuffer;
          rf->size = sizeof(rf->addr);
          DWORD flags = 0;
          wsabuf.buf = opBuffer;
          wsabuf.len = size;
          WSARecvFrom(object->hSocket, &wsabuf, 1, NULL, &flags, (SOCKADDR*)&rf->addr, &rf->size, &op->overlapped.data, NULL);
          break;
        }
        
        case actWrite:
          wsabuf.buf = opBuffer;
          wsabuf.len = size;
          WSASend(object->hSocket, &wsabuf, 1, NULL, 0, &op->overlapped.data, NULL);
          break;

        case actWriteMsg : {
          struct sockaddr_in remoteAddress;
          wsabuf.buf = opBuffer;
          wsabuf.len = size;

          remoteAddress.sin_family = op->info.host.family;
          remoteAddress.sin_addr.s_addr = op->info.host.ipv4;
          remoteAddress.sin_port = op->info.host.port;

          WSASendTo(object->hSocket, &wsabuf, 1, NULL, 0, (struct sockaddr*)&remoteAddress, sizeof(remoteAddress), &op->overlapped.data, NULL);  
          break;
        }

        default :
          return;
      }
      break;

    case ioObjectDevice :
      switch (op->info.root.opCode) {
        case actRead :
        case actReadMsg : {
          ReadFile(object->hDevice, opBuffer, size, NULL, &op->overlapped.data);
          break;
       }
        case actWrite :
          WriteFile(object->hDevice, opBuffer, size, NULL, &op->overlapped.data);
          break;
        default :
          return;
      }
      break;
 
    default :
      return;

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
  const int maxEntriesNum = sizeof(entries)/sizeof(OVERLAPPED_ENTRY);
  iocpBase *localBase = (iocpBase*)base;
  
  while (1) {
    ULONG N, i;
    BOOL status = GetQueuedCompletionStatusEx(localBase->completionPort,
                                              entries,
                                              maxEntriesNum,
                                              &N,
                                              INFINITE,
                                              TRUE);
    
    // ignore false status
    if (status == FALSE)
      continue;
    
    for (i = 0; i < N; i++) {
      OVERLAPPED_ENTRY *entry = &entries[i];
      if (entry->lpCompletionKey) {
        asyncOpRoot *op = (asyncOpRoot*)entry->lpCompletionKey;
        if (op->opCode == actUserEvent)
          op->finishMethod(op, aosSuccess);
        else
          finishOperation(op, aosTimeout, 0);
      } else if (entry->lpOverlapped) {
        iocpOp *op = ((overlappedExtra*)entry->lpOverlapped)->op;
        AsyncOpStatus result = getOperationStatus(op);
        if (result == aosSuccess) {
          op->info.bytesTransferred += entry->dwNumberOfBytesTransferred;
          if (op->info.root.opCode == actAccept) {
            struct sockaddr_in *localAddr = 0;
            struct sockaddr_in *remoteAddr = 0;
            INT localAddrLength;
            INT remoteAddrLength;
            GetAcceptExSockaddrs(op->info.internalBuffer,
                                 entry->dwNumberOfBytesTransferred,
                                 sizeof(struct sockaddr_in)+16,
                                 sizeof(struct sockaddr_in)+16,
                                 (struct sockaddr**)&localAddr, &localAddrLength,
                                 (struct sockaddr**)&remoteAddr, &remoteAddrLength);
            if (localAddr && remoteAddr) {
              op->info.host.family = remoteAddr->sin_family;
              op->info.host.ipv4 = remoteAddr->sin_addr.s_addr;
              op->info.host.port = remoteAddr->sin_port;
            } else {
              result = aosUnknownError;
            }
          }
          else if (op->info.root.opCode == actRead || op->info.root.opCode == actWrite) {
            if ((op->info.root.flags & afWaitAll) &&
                 op->info.bytesTransferred < op->info.transactionSize) {
              startOrContinueAction(op, 0);
              continue;
            }
          } else if (op->info.root.opCode == actReadMsg) {
            struct recvFromData *rf = op->info.internalBuffer;
            op->info.host.family = rf->addr.sin_family;
            op->info.host.ipv4 = rf->addr.sin_addr.s_addr;
            op->info.host.port = rf->addr.sin_port;          
          }
        }
        
        finishOperation(&op->info.root, result, 1);
      } else {
        processTimeoutQueue(base);
        return;
      }
    }
    
    processTimeoutQueue(base);
  }
}


aioObject *iocpNewAioObject(iocpBase *base, IoObjectTy type, void *data)
{
  aioObject *object =
    (aioObject*)initObjectRoot(type, sizeof(aioObject), (aioObjectDestructor*)iocpDeleteObject);
  switch (type) {
    case ioObjectDevice :
      object->hDevice = *(iodevTy *)data;
      CreateIoCompletionPort(object->hDevice, base->completionPort, 0, 1);
      break;
    case ioObjectSocket :
      object->hSocket = *(socketTy *)data;
      CreateIoCompletionPort(object->hDevice, base->completionPort, 0, 1);
      break;
    default :
      break;
  }

  return object;
}


asyncOpRoot *iocpNewAsyncOp(asyncBase *base)
{
  iocpOp *op;
  op = malloc(sizeof(iocpOp));
  if (op)
    memset(op, 0, sizeof(asyncOp));

  return (asyncOpRoot*)op;
}


void iocpDeleteObject(aioObject *object)
{
  switch (object->root.type) {
    case ioObjectDevice :
    case ioObjectSocket :
    default :
      break;
  }  
  
  free(object);
}

void iocpFinishOp(iocpOp *op)
{
}

void iocpInitializeTimer(asyncOpRoot *op)
{
  op->timerId = (void*)CreateWaitableTimer(NULL, FALSE, NULL);
}

void iocpStartTimer(asyncOpRoot *op, uint64_t usTimeout)
{
  LARGE_INTEGER signalTime;
  signalTime.QuadPart = -(int64_t)(usTimeout*10);

  PTIMERAPCROUTINE timerCb;  
  switch (op->opCode) {
    case actUserEvent :
      timerCb = userEventTimerCb;
      break;
    default:
      timerCb = ioFinishedTimerCb;
      break;
  }  

  SetWaitableTimer((HANDLE)op->timerId, &signalTime, 0, timerCb, op, FALSE);  
}


void iocpStopTimer(asyncOpRoot *op)
{
  CancelWaitableTimer((HANDLE)op->timerId);  
}


void iocpActivate(asyncOpRoot *op)
{
  iocpBase *localBase = (iocpBase*)op->base;
  PostQueuedCompletionStatus(localBase->completionPort, 0, (ULONG_PTR)op, 0);
}


void iocpAsyncConnect(iocpOp *op, const HostAddress *address, uint64_t usTimeout)
{
  struct sockaddr_in localAddress;
  localAddress.sin_family = address->family;
  localAddress.sin_addr.s_addr = address->ipv4;
  localAddress.sin_port = address->port;
  initializeOp(op);    
  startOrContinueAction(op, &localAddress);
}


void iocpAsyncAccept(iocpOp *op, uint64_t usTimeout)
{  
  // Expand internal buffer to 2*(sizeof(struct sockaddr_in)+16) if need
  const size_t acceptResultSize = 2*(sizeof(struct sockaddr_in)+16);
  
  if (op->info.internalBuffer == 0) {
    op->info.internalBuffer = malloc(acceptResultSize);
    op->info.internalBufferSize = acceptResultSize;
  } else if (op->info.internalBufferSize < acceptResultSize) {
    op->info.internalBuffer = realloc(op->info.internalBuffer, acceptResultSize);
    op->info.internalBufferSize = acceptResultSize;
  }

  initializeOp(op);    
  startOrContinueAction(op, 0);
}


void iocpAsyncRead(iocpOp *op, uint64_t usTimeout)
{
  initializeOp(op);  
  startOrContinueAction(op, 0);
}


void iocpAsyncWrite(iocpOp *op, uint64_t usTimeout)
{
  initializeOp(op);  
  startOrContinueAction(op, 0);
}


void iocpAsyncReadMsg(iocpOp *op, uint64_t usTimeout)
{
  // Expand internal buffer to sizeof(struct sockaddr_in if need
  const size_t acceptResultSize = sizeof(recvFromData);
  
  if (op->info.internalBuffer == 0) {
    op->info.internalBuffer = malloc(acceptResultSize);
    op->info.internalBufferSize = acceptResultSize;
  } else if (op->info.internalBufferSize < acceptResultSize) {
    op->info.internalBuffer = realloc(op->info.internalBuffer, acceptResultSize);
    op->info.internalBufferSize = acceptResultSize;
  }  
  
  initializeOp(op);  
  startOrContinueAction(op, 0);
}


void iocpAsyncWriteMsg(iocpOp *op, const HostAddress *address, uint64_t usTimeout)
{
  initializeOp(op);
  op->info.host = *address;
  startOrContinueAction(op, 0);
}
