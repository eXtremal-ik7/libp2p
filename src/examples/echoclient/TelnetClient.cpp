#include "asyncio/asyncio.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>


const size_t clientBufferSize = 1024;


struct ClientData {
  asyncBase *base;
  aioObject *socket;
  bool isConnected;
  uint8_t buffer[clientBufferSize];
};


void readCb(AsyncOpStatus status, asyncBase *base, aioObject *object, size_t transferred, void *arg)
{
  ClientData *data = (ClientData*)arg;  
  if (status == aosSuccess) {
    for (uint8_t *p = data->buffer, *pe = p+transferred; p < pe; p++)
      printf("%02X", (unsigned)*p);
    printf("\n");
  } else if (status == aosDisconnected) {
    fprintf(stderr, "connection lost!\n");
    postQuitOperation(data->base);
  } else {
    fprintf(stderr, "receive error!\n");
  }
}


void pingTimerCb(asyncBase *base, aioObject *event, void *arg)
{
  ClientData *data = (ClientData*)arg;
  if (data->isConnected) {
    char symbol = 32 + rand() % 96;
    printf("%02X:", (int)symbol);
    fflush(stdout);
    aioWrite(data->socket, &symbol, 1, afNone, 1000000, 0, 0);
    aioRead(data->socket, data->buffer, clientBufferSize,
            afNone, 1000000, readCb, data);
  }
}


void connectCb(AsyncOpStatus status, asyncBase *base, aioObject *object, void *arg)
{
  ClientData *data = (ClientData*)arg;
  if (status == aosSuccess) {
    fprintf(stderr, "connected\n");
    data->isConnected = true;
  } else {
    fprintf(stderr, "connection error\n");
    exit(1);
  }
}


int main(int argc, char **argv)
{
  HostAddress address;
  initializeSocketSubsystem();
  srand((unsigned)time(NULL));

  asyncBase *base = createAsyncBase(amOSDefault);
  
  address.family = AF_INET;
  address.ipv4 = INADDR_ANY;
  address.port = 0;
  socketTy hSocket = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
  if (socketBind(hSocket, &address) != 0) {
    exit(1);
  }

  ClientData data;  
  aioObject *socketOp = newSocketIo(base, hSocket);
  aioObject *stdInputOp = newUserEvent(base, pingTimerCb, &data);

  address.family = AF_INET;
  address.ipv4 = inet_addr("127.0.0.1");
  address.port = htons(9999);
  data.base = base;
  data.socket = socketOp;
  data.isConnected = false;    
  aioConnect(socketOp, &address, 3000000, connectCb, &data);
  userEventStartTimer(stdInputOp, 1000000, -1);
  asyncLoop(base);
  return 0;
}
