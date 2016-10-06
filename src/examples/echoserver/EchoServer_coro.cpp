#include "asyncio/asyncio.h"
#include "asyncio/coroutine.h"
#include <stdio.h>
#include <stdlib.h>

struct listenerContext {
  asyncBase *base;
  aioObject *socket;
};

struct readerContext {
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

void *readerProc(void *arg)
{
  readerContext *reader = (readerContext*)arg;
  uint8_t echoBuffer[1024];
  while (true) {
    ssize_t bytesRead = ioRead(reader->base, reader->socket, echoBuffer, sizeof(echoBuffer), afNone, 0);
    if (bytesRead != -1) {
      printhex(echoBuffer, bytesRead);
      ioWrite(reader->base, reader->socket, echoBuffer, bytesRead, afNone, 0);
    } else {
      fprintf(stderr, " * asyncRead error, exiting..\n");
      break;
    }
  }
  
  return 0;
}

void *listenerProc(void *arg)
{
  listenerContext *ctx = (listenerContext*)arg;
  while (true) {
    socketTy acceptSocket = ioAccept(ctx->base, ctx->socket, 0);
    if (acceptSocket != INVALID_SOCKET) {
      fprintf(stderr, "new connection accepted\n");
      readerContext *reader = new readerContext;
      reader->base = ctx->base;
      reader->socket = newSocketIo(ctx->base, acceptSocket);      
      coroutineTy *echoProc = coroutineNew(readerProc, reader, 0x10000);
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
  address.port = htons(9999);
  
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
  
  listenerContext ctx;
  ctx.base = base;
  ctx.socket = socketOp;

  coroutineCall(coroutineNew(listenerProc, &ctx, 0x10000));
  asyncLoop(base);
  return 0;
}
