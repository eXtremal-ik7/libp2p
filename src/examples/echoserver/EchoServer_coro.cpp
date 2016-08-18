#include "asyncio/asyncio.h"
#include "asyncio/coroutine.h"
#include <stdio.h>
#include <stdlib.h>

struct listenerCtx {
  asyncBase *base;
  aioObject *socket;
};

void printhex(uint8_t *data, size_t size)
{
  for (size_t i = 0; i < size; i++) {
    fprintf(stderr, "%02X", data[i]);
  }
  fprintf(stderr, "\n");   
}

void *reader(void *arg)
{
  aioObject *socket = (aioObject*)arg;  
  uint8_t echoBuffer[1024];
  while (true) {
    ssize_t bytesRead = ioRead(socket, echoBuffer, sizeof(echoBuffer), afNone, 0);
    if (bytesRead != -1) {
      printhex(echoBuffer, bytesRead);
      ioWrite(socket, echoBuffer, bytesRead, afNone, 0);
    } else {
      fprintf(stderr, " * asyncRead error, exiting..\n");
      break;
    }
  }
  
  return 0;
}

void *listener(void *arg)
{
  listenerCtx *ctx = (listenerCtx*)arg;
  while (true) {
    socketTy acceptSocket = ioAccept(ctx->socket, 0);
    if (acceptSocket != INVALID_SOCKET) {
      fprintf(stderr, "new connection accepted\n");
      aioObject *newSocketOp = newSocketIo(ctx->base, acceptSocket);      
      coroutineTy *echoProc = coroutineNew(reader, newSocketOp, 0x10000);
      coroutineCall(echoProc);      
    }
  }
  
  return 0;
}

int main(int argc, char **argv)
{
  HostAddress address;
  address.family = AF_INET;
  address.ipv4 = INADDR_ANY;
  address.port = htons(12200);
  
  initializeSocketSubsystem();
  socketTy hSocket = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
  socketReuseAddr(hSocket);
  if (socketBind(hSocket, &address) != 0) {
    fprintf(stderr, "cannot bind\n");
    exit(1);
  }

  if (socketListen(hSocket) != 0) {
    fprintf(stderr, "listen error\n");
    exit(1);
  }

  asyncBase *base = createAsyncBase(amOSDefault);
  aioObject *socketOp = newSocketIo(base, hSocket);
  
  listenerCtx ctx;
  ctx.base = base;
  ctx.socket = socketOp;

  coroutineTy *listenerProc = coroutineNew(listener, &ctx, 0x10000);
  coroutineCall(listenerProc);
  asyncLoop(base);
  return 0;
}
