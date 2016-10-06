#include "asyncio/asyncio.h"
#include <stdio.h>
#include <stdlib.h>


const size_t echoBufferSize = 1024;


void readCb(AsyncOpStatus status, asyncBase *base, aioObject *socket, size_t transferred, void *arg)
{
  uint8_t *echoBuffer = (uint8_t*)arg;
  if (status == aosSuccess) {
    aioWrite(base, socket, echoBuffer, transferred, afNone, 1000000, 0, 0);
    aioRead(base, socket, echoBuffer, echoBufferSize, afNone, 0, readCb, echoBuffer);
  } else if (status == aosDisconnected) {
    delete[] echoBuffer;
    fprintf(stderr, " * connection lost\n");
  } else {
    fprintf(stderr, " * receive error\n");
    aioRead(base, socket, echoBuffer, echoBufferSize, afNone, 0, readCb, echoBuffer);    
  }
}


void acceptCb(AsyncOpStatus status, asyncBase *base, aioObject *listener, HostAddress client, socketTy acceptSocket, void *arg)
{
  if (status == aosSuccess) {
    fprintf(stderr, " * new client\n");
    uint8_t *echoBuffer = new uint8_t[echoBufferSize];
    aioObject *newSocketOp = newSocketIo(base, acceptSocket);
    aioRead(base, newSocketOp, echoBuffer, echoBufferSize, afNone, 0, readCb, echoBuffer);
  }
  aioAccept(base, listener, 0, acceptCb, 0);
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

  aioAccept(base, socketOp, 0, acceptCb, 0);
  asyncLoop(base);
  return 0;
}
