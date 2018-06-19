#include "asyncio/asyncio.h"
#include "asyncio/socket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    aioWrite(base, data->socket, &symbol, 1, afNone, 1000000, 0, 0);
    aioRead(base, data->socket, data->buffer, clientBufferSize, afNone, 1000000, readCb, data);
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
  if (argc != 4) {
    fprintf(stderr, "usage: %s <method> <ip> <port>\n", argv[0]);
    return 1;
  }
  
  AsyncMethod method;
  if (strcmp(argv[1], "default") == 0) {
    method = amOSDefault;
  } else if (strcmp(argv[1], "select") == 0) {
    method = amSelect;
  } else if (strcmp(argv[1], "epoll") == 0) {
    method = amEPoll;
  } else if (strcmp(argv[1], "kqueue") == 0) {
    method = amKQueue;
  } else if (strcmp(argv[1], "iocp") == 0) {
    method = amIOCP;
  } else {
    fprintf(stderr, "ERROR: unknown method %s, default used\n", argv[1]);
    method = amOSDefault;
  }  
  
  HostAddress address;
  initializeSocketSubsystem();
  srand((unsigned)time(NULL));

  asyncBase *base = createAsyncBase(method);
  
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
  address.ipv4 = inet_addr(argv[2]);
  address.port = htons(atoi(argv[3]));
  data.base = base;
  data.socket = socketOp;
  data.isConnected = false;    
  aioConnect(base, socketOp, &address, 3000000, connectCb, &data);
  userEventStartTimer(stdInputOp, 1000000, -1);
  asyncLoop(base);
  return 0;
}
