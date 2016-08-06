#include "asyncio/asyncio.h"
#include <stdio.h>
#include <stdlib.h>


const size_t echoBufferSize = 1024;


void readCb(aioInfo *info)
{
  uint8_t *echoBuffer = (uint8_t*)info->arg;
  if (info->status == aosSuccess) {
    aioWrite(info->object, echoBuffer, info->bytesTransferred,
               afNone, 1000000, 0, 0);
    aioRead(info->object, echoBuffer, echoBufferSize,
              afNone, 0, readCb, echoBuffer);
  } else if (info->status == aosDisconnected) {
    delete[] echoBuffer;
    fprintf(stderr, " * connection lost\n");
  } else {
    fprintf(stderr, " * receive error\n");
    aioRead(info->object, echoBuffer, echoBufferSize,
              afNone, 0, readCb, echoBuffer);    
  }
}


void acceptCb(aioInfo *info)
{
  asyncBase *base = (asyncBase*)info->arg;
  if (info->status == aosSuccess) {
    fprintf(stderr, " * new client\n");
    uint8_t *echoBuffer = new uint8_t[echoBufferSize];
    aioObject *newSocketOp =
      newSocketIo(base, info->acceptSocket);
    aioRead(newSocketOp, echoBuffer, echoBufferSize,
              afNone, 0, readCb, echoBuffer);
  }
  aioAccept(info->object, 0, acceptCb, base);
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
