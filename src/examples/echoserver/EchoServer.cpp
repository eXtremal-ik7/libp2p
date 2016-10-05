#include "asyncio/asyncio.h"
#include <stdio.h>
#include <stdlib.h>


const size_t echoBufferSize = 1024;


void readCb(AsyncOpStatus status, aioObject *socket, void *buffer, size_t size, size_t transferred, void *arg)
{
  uint8_t *echoBuffer = (uint8_t*)buffer;
  if (status == aosSuccess) {
    aioWrite(socket, echoBuffer, transferred, afNone, 1000000, 0, 0);
    aioRead(socket, echoBuffer, echoBufferSize, afNone, 0, readCb, echoBuffer);
  } else if (status == aosDisconnected) {
    delete[] echoBuffer;
    fprintf(stderr, " * connection lost\n");
  } else {
    fprintf(stderr, " * receive error\n");
    aioRead(socket, echoBuffer, echoBufferSize, afNone, 0, readCb, echoBuffer);    
  }
}


void acceptCb(AsyncOpStatus status, aioObject *listener, socketTy acceptSocket, void *arg)
{
  asyncBase *base = (asyncBase*)arg;
  if (status == aosSuccess) {
    fprintf(stderr, " * new client\n");
    uint8_t *echoBuffer = new uint8_t[echoBufferSize];
    aioObject *newSocketOp = newSocketIo(base, acceptSocket);
    aioRead(newSocketOp, echoBuffer, echoBufferSize, afNone, 0, readCb, echoBuffer);
  }
  aioAccept(listener, 0, acceptCb, base);
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

  aioAccept(socketOp, 0, acceptCb, base);
  asyncLoop(base);
  return 0;
}
