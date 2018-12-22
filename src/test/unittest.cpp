#include "asyncio/asyncio.h"
#include "asyncio/coroutine.h"
#include "asyncio/socket.h"
#include <gtest/gtest.h>

const unsigned gPort = 65333;

static asyncBase *gBase = 0;

struct TestContext {
  aioObject *serverSocket;
  aioObject *clientSocket;
  uint8_t clientBuffer[128];
  uint8_t serverBuffer[128];
  asyncBase *base;
  int state;
  bool success;
  TestContext(asyncBase *baseArg) : base(baseArg), state(0), success(false) {}
};

aioObject *startTCPServer(asyncBase *base, aioAcceptCb callback, void *arg, unsigned port)
{
  HostAddress address;
  
  address.family = AF_INET;
  address.ipv4 = INADDR_ANY;
  address.port = htons(port);  
  socketTy acceptSocket = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
  socketReuseAddr(acceptSocket);
  if (socketBind(acceptSocket, &address) != 0)
    return 0;
  
  if (socketListen(acceptSocket) != 0)
    return 0;
  
  aioObject *object = newSocketIo(base, acceptSocket);
  aioAccept(object, 333000, callback, arg);
  return object;
}

aioObject *startUDPServer(asyncBase *base, aioReadMsgCb callback, void *arg, void *buffer, size_t size, unsigned port)
{
  HostAddress address;
  
  address.family = AF_INET;
  address.ipv4 = INADDR_ANY;
  address.port = htons(port);  
  socketTy acceptSocket = socketCreate(AF_INET, SOCK_DGRAM, IPPROTO_UDP, 1);
  socketReuseAddr(acceptSocket);
  if (socketBind(acceptSocket, &address) != 0)
    return 0;
  
  aioObject *object = newSocketIo(base, acceptSocket);
  if (callback)
    aioReadMsg(object, buffer, size, afNone, 1000000, callback, arg);
  return object;
}

aioObject *initializeTCPClient(asyncBase *base, aioConnectCb callback, void *arg, unsigned port)
{
  HostAddress address;  
  address.family = AF_INET;
  address.ipv4 = INADDR_ANY;
  address.port = 0;
  socketTy connectSocket = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
  if (socketBind(connectSocket, &address) != 0)
    return 0;
  
  address.family = AF_INET;
  address.ipv4 = inet_addr("127.0.0.1");
  address.port = htons(port);    
  aioObject *object = newSocketIo(base, connectSocket);
  aioConnect(object, &address, 333000, callback, arg);
  return object;
}

aioObject *initializeUDPClient(asyncBase *base, unsigned port)
{
  HostAddress address;  
  address.family = AF_INET;
  address.ipv4 = INADDR_ANY;
  address.port = 0;
  socketTy clientSocket = socketCreate(AF_INET, SOCK_DGRAM, IPPROTO_UDP, 1);
  if (socketBind(clientSocket, &address) != 0)
    return 0;
  
  aioObject *object = newSocketIo(base, clientSocket);
  return object;
}

void test_connect_accept_readcb(AsyncOpStatus status, aioObject *socket, size_t transferred, void *arg)
{
  TestContext *ctx = (TestContext*)arg;
  if (status == aosDisconnected) {
    if (ctx->state == 1)
      ctx->success = true;
  }
  
  deleteAioObject(socket);
  postQuitOperation(ctx->base);
}

void test_connect_accept_acceptcb(AsyncOpStatus status, aioObject *listener, HostAddress client, socketTy acceptSocket, void *arg)
{
  TestContext *ctx = (TestContext*)arg;
  if (status == aosSuccess) {
    aioObject *newSocketOp = newSocketIo(ctx->base, acceptSocket);
    aioRead(newSocketOp, ctx->serverBuffer, sizeof(ctx->serverBuffer), afNone, 1000000, test_connect_accept_readcb, ctx);
  } else {
    postQuitOperation(ctx->base);
  }
}

void test_connect_accept_connectcb(AsyncOpStatus status, aioObject *object, void *arg)
{
  TestContext *ctx = (TestContext*)arg;
  if (status == aosSuccess)
    ctx->state = 1;
  deleteAioObject(object);
}

TEST(basic, test_connect_accept)
{
  TestContext context(gBase);
  context.serverSocket = startTCPServer(gBase, test_connect_accept_acceptcb, &context, gPort);
  context.clientSocket = initializeTCPClient(gBase, test_connect_accept_connectcb, &context, gPort);
  ASSERT_NE(context.serverSocket, nullptr);
  ASSERT_NE(context.clientSocket, nullptr);
  
  asyncLoop(gBase);
  deleteAioObject(context.serverSocket);
  ASSERT_TRUE(context.success);
}

void test_tcp_rw_client_read(AsyncOpStatus status, aioObject *socket, size_t transferred, void *arg)
{
  TestContext *ctx = (TestContext*)arg;
  if (status == aosSuccess && transferred == 6 && ctx->state == 1) {
    ctx->success = memcmp(ctx->clientBuffer, "234567", 6) == 0;
  } 
    
  deleteAioObject(socket);  
}

void test_tcp_rw_connectcb(AsyncOpStatus status, aioObject *object, void *arg)
{
  TestContext *ctx = (TestContext*)arg;
  if (status == aosSuccess) {
    ctx->state = 1;
    aioWrite(object, (void*)"123456", 6, afWaitAll, 0, 0, 0);
    aioRead(object, ctx->clientBuffer, 6, afWaitAll, 333000, test_tcp_rw_client_read, ctx);
  } else {
    deleteAioObject(object);
    postQuitOperation(ctx->base);
  }
}

void test_tcp_rw_server_readcb(AsyncOpStatus status, aioObject *socket, size_t transferred, void *arg)
{
  TestContext *ctx = (TestContext*)arg;
  if (status == aosSuccess) {
    for (size_t i = 0; i < transferred; i++)
      ctx->serverBuffer[i]++;
    aioWrite(socket, ctx->serverBuffer, transferred, afNone, 0, 0, 0);
    aioRead(socket, ctx->serverBuffer, sizeof(ctx->serverBuffer), afNone, 0, test_tcp_rw_server_readcb, ctx);
  } else {
    deleteAioObject(socket);
    postQuitOperation(ctx->base);
  }
}

void test_tcp_rw_acceptcb(AsyncOpStatus status, aioObject *listener, HostAddress client, socketTy acceptSocket, void *arg)
{
  TestContext *ctx = (TestContext*)arg;
  if (status == aosSuccess) {
    aioObject *newSocketOp = newSocketIo(ctx->base, acceptSocket);
    aioRead(newSocketOp, ctx->serverBuffer, sizeof(ctx->serverBuffer), afNone, 0, test_tcp_rw_server_readcb, ctx);
  } else {
    postQuitOperation(ctx->base);
  }
}

TEST(basic, test_tcp_rw)
{
  TestContext context(gBase);
  context.serverSocket = startTCPServer(gBase, test_tcp_rw_acceptcb, &context, gPort);
  context.clientSocket = initializeTCPClient(gBase, test_tcp_rw_connectcb, &context, gPort);
  ASSERT_NE(context.serverSocket, nullptr);
  ASSERT_NE(context.clientSocket, nullptr);
  
  asyncLoop(gBase);
  deleteAioObject(context.serverSocket);
  ASSERT_TRUE(context.success);
}

void test_udp_rw_client_readcb(AsyncOpStatus status, aioObject *socket, HostAddress address, size_t transferred, void *arg)
{
  TestContext *ctx = (TestContext*)arg;
  if (status == aosSuccess && transferred == 6 && ctx->state == 0) {
    ctx->success = memcmp(ctx->clientBuffer, "234567", 6) == 0;
  } 

  deleteAioObject(socket);    
  postQuitOperation(ctx->base);
}

void test_udp_rw_server_readcb(AsyncOpStatus status, aioObject *socket, HostAddress address, size_t transferred, void *arg)
{
  TestContext *ctx = (TestContext*)arg;
  if (status == aosSuccess) {
    for (size_t i = 0; i < transferred; i++)
      ctx->serverBuffer[i]++;
    aioWriteMsg(socket, &address, ctx->serverBuffer, transferred, afNone, 0, 0, 0);
    aioReadMsg(socket, ctx->serverBuffer, sizeof(ctx->serverBuffer), afNone, 1000000, test_udp_rw_server_readcb, ctx);
  }
}

TEST(basic, test_udp_rw)
{
  TestContext context(gBase);
  context.serverSocket = startUDPServer(gBase, test_udp_rw_server_readcb, &context, context.serverBuffer, sizeof(context.serverBuffer), gPort);
  context.clientSocket = initializeUDPClient(gBase, gPort);
  ASSERT_NE(context.serverSocket, (void*)0);
  ASSERT_NE(context.clientSocket, (void*)0);
  
  HostAddress address;
  address.family = AF_INET;
  address.ipv4 = inet_addr("127.0.0.1");
  address.port = htons(gPort);  
  aioWriteMsg(context.clientSocket, &address, (void*)"123456", 6, afNone, 0, 0, 0);
  aioReadMsg(context.clientSocket, context.clientBuffer, sizeof(context.clientBuffer), afNone, 1000000, test_udp_rw_client_readcb, &context);
  asyncLoop(gBase);
  deleteAioObject(context.serverSocket);
  ASSERT_TRUE(context.success);
}

void test_timeout_readcb(AsyncOpStatus status, aioObject *socket, HostAddress address, size_t transferred, void *arg)
{
  TestContext *ctx = (TestContext*)arg;
  if (status == aosTimeout) {
    ctx->state++;
    if (ctx->state == 4) {
      ctx->success = true;
      postQuitOperation(ctx->base);
    }
  }
}

TEST(basic, test_timeout)
{
  TestContext context(gBase);
  context.serverSocket = startUDPServer(gBase, 0, &context, context.serverBuffer, sizeof(context.serverBuffer), gPort);
  ASSERT_NE(context.serverSocket, (void*)0);
  aioReadMsg(context.serverSocket, context.serverBuffer, sizeof(context.serverBuffer), afRealtime, 1000, test_timeout_readcb, &context);
  aioReadMsg(context.serverSocket, context.serverBuffer, sizeof(context.serverBuffer), afRealtime, 5000, test_timeout_readcb, &context);
  aioReadMsg(context.serverSocket, context.serverBuffer, sizeof(context.serverBuffer), afRealtime, 10000, test_timeout_readcb, &context);
  aioReadMsg(context.serverSocket, context.serverBuffer, sizeof(context.serverBuffer), afNone, 100000, test_timeout_readcb, &context);
  asyncLoop(gBase);
  deleteAioObject(context.serverSocket);
  ASSERT_TRUE(context.success);
}

void test_delete_object_eventcb(aioUserEvent *event, void *arg)
{
  TestContext *ctx = (TestContext*)arg;
  deleteAioObject(ctx->serverSocket);
}

void test_delete_object_readcb(AsyncOpStatus status, aioObject *socket, HostAddress address, size_t transferred, void *arg)
{
  TestContext *ctx = (TestContext*)arg;
  if (status != aosSuccess) {
    ctx->state++;
    if (ctx->state == 1000) {
      ctx->success = true;
      postQuitOperation(ctx->base);
    }
  }
}

TEST(basic, test_delete_object)
{
  TestContext context(gBase);
  context.serverSocket = startUDPServer(gBase, 0, &context, context.serverBuffer, sizeof(context.serverBuffer), gPort);
  ASSERT_NE(context.serverSocket, nullptr);
  
  for (int i = 0; i < 1000; i++)
    aioReadMsg(context.serverSocket, context.serverBuffer, sizeof(context.serverBuffer), afNone, 10*1000000, test_delete_object_readcb, &context);
  
  aioUserEvent *event = newUserEvent(gBase, test_delete_object_eventcb, &context);
  userEventStartTimer(event, 5000, 1);
  asyncLoop(gBase);  
  deleteUserEvent(event);
  ASSERT_TRUE(context.success);  
}

void test_userevent_cb(aioUserEvent *event, void *arg)
{
  TestContext *ctx = (TestContext*)arg;
  ctx->state++;

  if (ctx->state == 9) {
    userEventActivate(event);
  } else if (ctx->state == 10) {
    ctx->success = true;
    postQuitOperation(ctx->base);
  }
}

TEST(basic, test_userevent)
{
  TestContext context(gBase);
  aioUserEvent *event = newUserEvent(gBase, test_userevent_cb, &context);
  userEventStartTimer(event, 5000, 8);
  userEventActivate(event);
  asyncLoop(gBase);  
  deleteUserEvent(event);
  ASSERT_TRUE(context.success);  
}

void coroutine_create_proc(void *arg)
{
  int *x = (int*)arg;
  (*x)++;
}

TEST(coroutine, create)
{
  int x = 0;
  coroutineTy *coro = coroutineNew(coroutine_create_proc, &x, 0x10000);
  while (!coroutineCall(coro))
    continue;
  ASSERT_EQ(x, 1);
}

void coroutine_yield_proc(void *arg)
{
  int *x = (int*)arg;
  (*x)++;
  coroutineYield();
  (*x)++;
}

TEST(coroutine, yield)
{
  int x = 0;
  coroutineTy *coro = coroutineNew(coroutine_yield_proc, &x, 0x10000);
  while (!coroutineCall(coro))
    continue;
  ASSERT_EQ(x, 2);
}

void coroutine_nested_proc2(void *arg)
{
  int *x = (int*)arg;
  (*x)++;
}

void coroutine_nested_proc1(void *arg)
{
  int *x = (int*)arg;
  (*x)++;
  coroutineYield();
  coroutineTy *coro = coroutineNew(coroutine_nested_proc2, x, 0x10000);
  coroutineCall(coro);
  coroutineYield();
  (*x)++;
}

TEST(coroutine, nested)
{
  int x = 0;
  coroutineTy *coro = coroutineNew(coroutine_nested_proc1, &x, 0x10000);
  while (!coroutineCall(coro))
    continue;
  ASSERT_EQ(x, 3);
}

int main(int argc, char **argv)
{
  AsyncMethod method = amOSDefault;
  if (argc >= 2) {
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
    }  
  }
  
  initializeSocketSubsystem();
  
  gBase = createAsyncBase(method);
  
  ::testing::InitGoogleTest(&argc, argv); 
  return RUN_ALL_TESTS();
}
