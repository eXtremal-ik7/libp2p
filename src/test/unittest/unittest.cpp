#include "unittest.h"
#include "asyncio/coroutine.h"
#include "asyncio/device.h"
#include "asyncio/socket.h"
#include "atomic.h"
#include <chrono>
#include <thread>

asyncBase *gBase = nullptr;

aioObject *startTCPServer(asyncBase *base, aioAcceptCb callback, void *arg, uint16_t port)
{
  HostAddress address;

  address.family = AF_INET;
  address.ipv4 = INADDR_ANY;
  address.port = htons(port);
  socketTy acceptSocket = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
  socketReuseAddr(acceptSocket);
  if (socketBind(acceptSocket, &address) != 0) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (socketBind(acceptSocket, &address) != 0)
      return nullptr;
  }

  if (socketListen(acceptSocket) != 0)
    return nullptr;

  aioObject *object = newSocketIo(base, acceptSocket);
  if (callback)
    aioAccept(object, 333000, callback, arg);
  return object;
}

aioObject *startUDPServer(asyncBase *base, aioReadMsgCb callback, void *arg, void *buffer, size_t size, uint16_t port)
{
  HostAddress address;

  address.family = AF_INET;
  address.ipv4 = INADDR_ANY;
  address.port = htons(port);
  socketTy acceptSocket = socketCreate(AF_INET, SOCK_DGRAM, IPPROTO_UDP, 1);
  if (socketBind(acceptSocket, &address) != 0) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (socketBind(acceptSocket, &address) != 0)
      return nullptr;
  }

  aioObject *object = newSocketIo(base, acceptSocket);
  if (callback)
    aioReadMsg(object, buffer, size, afNone, 1000000, callback, arg);
  return object;
}

aioObject *initializeTCPClient(asyncBase *base, aioConnectCb callback, void *arg, uint16_t port)
{
  HostAddress address;
  address.family = AF_INET;
  address.ipv4 = INADDR_ANY;
  address.port = 0;
  socketTy connectSocket = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
  int bindResult = socketBind(connectSocket, &address);
  EXPECT_EQ(bindResult, 0);
  if (bindResult != 0)
    return nullptr;

  aioObject *object = newSocketIo(base, connectSocket);
  if (callback) {
    address.family = AF_INET;
    address.ipv4 = inet_addr("127.0.0.1");
    address.port = htons(port);
    aioConnect(object, &address, 333000, callback, arg);
  }

  return object;
}

aioObject *initializeUDPClient(asyncBase *base)
{
  HostAddress address;
  address.family = AF_INET;
  address.ipv4 = INADDR_ANY;
  address.port = 0;
  socketTy clientSocket = socketCreate(AF_INET, SOCK_DGRAM, IPPROTO_UDP, 1);
  if (socketBind(clientSocket, &address) != 0)
    return nullptr;

  aioObject *object = newSocketIo(base, clientSocket);
  return object;
}

void test_pipe_writecb(AsyncOpStatus status, aioObject*, size_t, void *arg)
{
  TestContext *ctx = static_cast<TestContext*>(arg);
  EXPECT_EQ(status, aosSuccess);
  if (status != aosSuccess)
    postQuitOperation(ctx->base);
}

void test_pipe_readcb(AsyncOpStatus status, aioObject*, size_t transferred, void *arg)
{
  TestContext *ctx = static_cast<TestContext*>(arg);
  reqStruct *req = reinterpret_cast<reqStruct*>(ctx->serverBuffer);
  EXPECT_EQ(status, aosSuccess);
  EXPECT_EQ(transferred, sizeof(reqStruct));
  if (status == aosSuccess && transferred == sizeof(reqStruct)) {
    EXPECT_EQ(req->a, 11);
    EXPECT_EQ(req->b, 77);
  }

  postQuitOperation(ctx->base);
}

TEST(basic, test_pipe)
{
  TestContext context(gBase);
  pipeTy unnamedPipe;
  int result = pipeCreate(&unnamedPipe, 1);
  EXPECT_EQ(result, 0);
  if (result == 0) {
    reqStruct req;
    context.pipeRead = newDeviceIo(gBase, unnamedPipe.read);
    context.pipeWrite = newDeviceIo(gBase, unnamedPipe.write);
    req.a = 11;
    req.b = 77;
    aioRead(context.pipeRead, context.serverBuffer, sizeof(req), afWaitAll, 1000000, test_pipe_readcb, &context);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    aioWrite(context.pipeWrite, &req, sizeof(req), afWaitAll, 0, test_pipe_writecb, &context);
    asyncLoop(gBase);
    deleteAioObject(context.pipeRead);
    deleteAioObject(context.pipeWrite);
  }
}

void test_connect_accept_readcb(AsyncOpStatus status, aioObject *socket, size_t transferred, void *arg)
{
  __UNUSED(transferred);
  TestContext *ctx = static_cast<TestContext*>(arg);
  EXPECT_EQ(status, aosDisconnected);
  if (status == aosDisconnected) {
    if (ctx->serverState == 1)
      ctx->success = true;
  }

  deleteAioObject(socket);
  postQuitOperation(ctx->base);
}

void test_connect_accept_acceptcb(AsyncOpStatus status, aioObject *listener, HostAddress client, socketTy acceptSocket, void *arg)
{
  __UNUSED(listener);
  __UNUSED(client);
  TestContext *ctx = static_cast<TestContext*>(arg);
  if (status == aosSuccess) {
    aioObject *newSocketOp = newSocketIo(ctx->base, acceptSocket);
    aioRead(newSocketOp, ctx->serverBuffer, sizeof(ctx->serverBuffer), afNone, 1000000, test_connect_accept_readcb, ctx);
  } else {
    postQuitOperation(ctx->base);
  }
}

void test_connect_accept_connectcb(AsyncOpStatus status, aioObject *object, void *arg)
{
  EXPECT_EQ(status, aosSuccess);
  TestContext *ctx = static_cast<TestContext*>(arg);
  if (status == aosSuccess)
    ctx->serverState = 1;
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
  TestContext *ctx = static_cast<TestContext*>(arg);
  EXPECT_EQ(status, aosSuccess);
  EXPECT_EQ(transferred, 7u);
  EXPECT_EQ(ctx->serverState, 1);
  if (status == aosSuccess && transferred == 7 && ctx->serverState == 1) {
    EXPECT_STREQ(reinterpret_cast<const char*>(ctx->clientBuffer), "234567");
    ctx->success = strcmp(reinterpret_cast<const char*>(ctx->clientBuffer), "234567") == 0;
  }

  deleteAioObject(socket);
}

void test_tcp_rw_connectcb(AsyncOpStatus status, aioObject *object, void *arg)
{
  TestContext *ctx = static_cast<TestContext*>(arg);
  EXPECT_EQ(status, aosSuccess);
  if (status == aosSuccess) {
    ctx->serverState = 1;
    aioWrite(object, "123456", 7, afWaitAll, 0, nullptr, nullptr);
    aioRead(object, ctx->clientBuffer, 7, afWaitAll, 333000, test_tcp_rw_client_read, ctx);
  } else {
    deleteAioObject(object);
    postQuitOperation(ctx->base);
  }
}

void test_tcp_rw_server_readcb(AsyncOpStatus status, aioObject *socket, size_t transferred, void *arg)
{
  TestContext *ctx = static_cast<TestContext*>(arg);
  if (status == aosSuccess) {
    for (size_t i = 0; i < transferred-1; i++)
      ctx->serverBuffer[i]++;
    aioWrite(socket, ctx->serverBuffer, transferred, afNone, 0, nullptr, nullptr);
    aioRead(socket, ctx->serverBuffer, sizeof(ctx->serverBuffer), afNone, 0, test_tcp_rw_server_readcb, ctx);
  } else {
    deleteAioObject(socket);
    postQuitOperation(ctx->base);
  }
}

void test_tcp_rw_acceptcb(AsyncOpStatus status, aioObject *listener, HostAddress client, socketTy acceptSocket, void *arg)
{
  __UNUSED(listener);
  __UNUSED(client);
  TestContext *ctx = static_cast<TestContext*>(arg);
  EXPECT_EQ(status, aosSuccess);
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
  __UNUSED(address);
  TestContext *ctx = static_cast<TestContext*>(arg);
  EXPECT_EQ(status, aosSuccess);
  EXPECT_EQ(transferred, 7u);
  EXPECT_EQ(ctx->serverState, 1);
  if (status == aosSuccess &&
      transferred == 7 &&
      ctx->serverState == 1) {
    EXPECT_STREQ(reinterpret_cast<const char*>(ctx->clientBuffer), "234567");
    if (strcmp(reinterpret_cast<const char*>(ctx->clientBuffer), "234567") == 0)
      ctx->serverState++;
  }

  deleteAioObject(socket);
  deleteAioObject(ctx->serverSocket);
}

void test_udp_rw_server_readcb(AsyncOpStatus status, aioObject *socket, HostAddress address, size_t transferred, void *arg)
{
  TestContext *ctx = static_cast<TestContext*>(arg);
  if (status == aosSuccess) {
    ctx->serverState++;
    for (size_t i = 0; i < transferred-1; i++)
      ctx->serverBuffer[i]++;
    aioWriteMsg(socket, &address, ctx->serverBuffer, transferred, afNone, 0, nullptr, nullptr);
    aioReadMsg(socket, ctx->serverBuffer, sizeof(ctx->serverBuffer), afNone, 1000000, test_udp_rw_server_readcb, ctx);
  } else {
    if (status == aosCanceled)
      ctx->success = (ctx->serverState == 2);
    postQuitOperation(ctx->base);
  }
}

TEST(basic, test_udp_rw)
{
  TestContext context(gBase);
  context.serverSocket = startUDPServer(gBase, test_udp_rw_server_readcb, &context, context.serverBuffer, sizeof(context.serverBuffer), gPort);
  context.clientSocket = initializeUDPClient(gBase);
  ASSERT_NE(context.serverSocket, nullptr);
  ASSERT_NE(context.clientSocket, nullptr);

  HostAddress address;
  address.family = AF_INET;
  address.ipv4 = inet_addr("127.0.0.1");
  address.port = htons(gPort);
  aioWriteMsg(context.clientSocket, &address, "123456", 7, afNone, 0, nullptr, nullptr);
  aioReadMsg(context.clientSocket, context.clientBuffer, sizeof(context.clientBuffer), afNone, 1000000, test_udp_rw_client_readcb, &context);
  asyncLoop(gBase);
  ASSERT_TRUE(context.success);
}

void test_timeout_readcb(AsyncOpStatus status, aioObject *socket, HostAddress address, size_t transferred, void *arg)
{
  __UNUSED(socket);
  __UNUSED(address);
  __UNUSED(transferred);
  TestContext *ctx = static_cast<TestContext*>(arg);
  EXPECT_EQ(status, aosTimeout);
  if (status == aosTimeout) {
    ctx->serverState++;
    if (ctx->serverState == 1000+1+1+1+1000) {
      ctx->success = true;
      postQuitOperation(ctx->base);
    }
  }
}

TEST(basic, test_timeout)
{
  TestContext context(gBase);
  context.serverSocket = startUDPServer(gBase, nullptr, &context, context.serverBuffer, sizeof(context.serverBuffer), gPort);
  ASSERT_NE(context.serverSocket, nullptr);
  for (unsigned i = 0; i < 1000; i++)
    aioReadMsg(context.serverSocket, context.serverBuffer, sizeof(context.serverBuffer), afRealtime, 77000, test_timeout_readcb, &context);
  aioReadMsg(context.serverSocket, context.serverBuffer, sizeof(context.serverBuffer), afRealtime, 1000, test_timeout_readcb, &context);
  aioReadMsg(context.serverSocket, context.serverBuffer, sizeof(context.serverBuffer), afRealtime, 5000, test_timeout_readcb, &context);
  aioReadMsg(context.serverSocket, context.serverBuffer, sizeof(context.serverBuffer), afRealtime, 10000, test_timeout_readcb, &context);
  for (unsigned i = 0; i < 1000; i++)
    aioReadMsg(context.serverSocket, context.serverBuffer, sizeof(context.serverBuffer), afNone, 100000, test_timeout_readcb, &context);
  asyncLoop(gBase);
  deleteAioObject(context.serverSocket);
  ASSERT_TRUE(context.success);
}

void test_delete_object_eventcb(aioUserEvent *event, void *arg)
{
  __UNUSED(event);
  TestContext *ctx = static_cast<TestContext*>(arg);
  deleteAioObject(ctx->serverSocket);
}

void test_delete_object_readcb(AsyncOpStatus status, aioObject *socket, HostAddress address, size_t transferred, void *arg)
{
  __UNUSED(socket);
  __UNUSED(address);
  __UNUSED(transferred);
  TestContext *ctx = static_cast<TestContext*>(arg);
  EXPECT_EQ(status, aosCanceled);
  if (status == aosCanceled) {
    ctx->serverState++;
    if (ctx->serverState == 1000) {
      ctx->success = true;
      postQuitOperation(ctx->base);
    }
  } else {
    postQuitOperation(ctx->base);
  }
}

TEST(basic, test_delete_object)
{
  TestContext context(gBase);
  context.serverSocket = startUDPServer(gBase, nullptr, &context, context.serverBuffer, sizeof(context.serverBuffer), gPort);
  ASSERT_NE(context.serverSocket, nullptr);

  for (int i = 0; i < 1000; i++)
    aioReadMsg(context.serverSocket, context.serverBuffer, sizeof(context.serverBuffer), afNone, 3*1000000, test_delete_object_readcb, &context);

  aioUserEvent *event = newUserEvent(gBase, test_delete_object_eventcb, &context);
  userEventStartTimer(event, 5000, 1);
  asyncLoop(gBase);
  deleteUserEvent(event);
  ASSERT_TRUE(context.success);
}

void test_userevent_cb(aioUserEvent *event, void *arg)
{
  TestContext *ctx = static_cast<TestContext*>(arg);
  unsigned value = __uint_atomic_fetch_and_add(reinterpret_cast<unsigned*>(&ctx->serverState), 1);
  if (value == 256) {
    userEventActivate(event);
  } else if (value == 257) {
    ctx->success = true;
    postQuitOperation(ctx->base);
  }
}

TEST(basic, test_userevent)
{
  TestContext context(gBase);
  aioUserEvent *event = newUserEvent(gBase, test_userevent_cb, &context);
  userEventStartTimer(event, 400, 256);
  userEventActivate(event);
  std::thread threads[4];
  for (unsigned i = 0; i < 4; i++) {
    threads[i] = std::thread([](){
      asyncLoop(gBase);
    });
  }
  std::for_each(threads, threads+4, [](std::thread &thread) {
    thread.join();
  });
  deleteUserEvent(event);
  ASSERT_TRUE(context.success);
}

void coroutine_create_proc(void *arg)
{
  int *x = static_cast<int*>(arg);
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
  int *x = static_cast<int*>(arg);
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
  int *x = static_cast<int*>(arg);
  (*x)++;
}

void coroutine_nested_proc1(void *arg)
{
  int *x = static_cast<int*>(arg);
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

void p2pproto_ca_read(AsyncOpStatus status, p2pConnection *connection, p2pHeader header, void *data, void *arg)
{
  __UNUSED(header);
  __UNUSED(data);
  TestContext *ctx = static_cast<TestContext*>(arg);
  if (ctx->clientState == 2) {
    EXPECT_EQ(status, aosDisconnected);
    if (status == aosDisconnected)
      ctx->serverState = 3;
  }
  p2pConnectionDelete(connection);
  postQuitOperation(ctx->base);
}

p2pErrorTy p2pproto_ca_check(AsyncOpStatus status, p2pConnection *connection, p2pConnectData *data, void *arg)
{
  TestContext *ctx = static_cast<TestContext*>(arg);
  if (data) {
    EXPECT_EQ(ctx->serverState, 0);
    EXPECT_STREQ(data->login, "p2pproto_ca_login");
    EXPECT_STREQ(data->password, "p2pproto_ca_password");
    EXPECT_STREQ(data->application, "p2pproto_ca_application");
    if (ctx->serverState == 0 &&
        strcmp(data->login, "p2pproto_ca_login") == 0 &&
        strcmp(data->password, "p2pproto_ca_password") == 0 &&
        strcmp(data->application, "p2pproto_ca_application") == 0)
      ctx->serverState = 1;
  } else {
    EXPECT_EQ(status, aosSuccess);
    EXPECT_EQ(ctx->serverState, 1);
    if (status == aosSuccess &&
        ctx->serverState == 1)
      ctx->serverState = 2;
    aiop2pRecv(connection, ctx->serverBuffer, 128, afNone, 1000000, p2pproto_ca_read, ctx);
  }

  return p2pOk;
}

void p2pproto_ca_accept(AsyncOpStatus status, aioObject *listener, HostAddress client, socketTy acceptSocket, void *arg)
{
  __UNUSED(listener);
  __UNUSED(client);
  TestContext *ctx = static_cast<TestContext*>(arg);
  EXPECT_EQ(status, aosSuccess);
  if (status == aosSuccess) {
    p2pConnection *connection = p2pConnectionNew(newSocketIo(ctx->base, acceptSocket));
    aiop2pAccept(connection, 1000000, p2pproto_ca_check, ctx);
  }
}

void p2pproto_ca_connect(AsyncOpStatus status, p2pConnection *connection, void *arg)
{
  TestContext *ctx = static_cast<TestContext*>(arg);
  EXPECT_EQ(status, aosSuccess);
  if (status == aosSuccess)
    ctx->clientState = 1;
  p2pConnectionDelete(connection);
}

TEST(p2pproto, connect_accept)
{
  TestContext context(gBase);
  context.serverSocket = startTCPServer(gBase, p2pproto_ca_accept, &context, gPort);
  context.clientSocket = initializeTCPClient(gBase, nullptr, &context, gPort);
  ASSERT_NE(context.serverSocket, nullptr);
  ASSERT_NE(context.clientSocket, nullptr);

  HostAddress address;
  p2pConnectData data;
  address.family = AF_INET;
  address.ipv4 = inet_addr("127.0.0.1");
  address.port = htons(gPort);
  data.login = "p2pproto_ca_login";
  data.password = "p2pproto_ca_password";
  data.application = "p2pproto_ca_application";
  p2pConnection *connection = p2pConnectionNew(context.clientSocket);
  aiop2pConnect(connection, &address, &data, 1000000, p2pproto_ca_connect, &context);
  asyncLoop(gBase);
  deleteAioObject(context.serverSocket);
  ASSERT_EQ(context.serverState, 2);
  ASSERT_EQ(context.clientState, 1);
}

void p2pproto_rw_serverread(AsyncOpStatus status, p2pConnection *connection, p2pHeader header, p2pStream *stream, void *arg)
{
  bool end = true;
  TestContext *ctx = static_cast<TestContext*>(arg);
  if (ctx->serverState == 2 ) {
    EXPECT_EQ(status, aosSuccess);
    EXPECT_EQ(header.id, 222u);
    EXPECT_EQ(header.type, p2pMsgRequest);
    EXPECT_EQ(header.size, 5u);
    EXPECT_STREQ(static_cast<const char*>(stream->data()), "msg1");
    if (status == aosSuccess &&
        header.id == 222u &&
        header.type == p2pMsgRequest &&
        strcmp(static_cast<const char*>(stream->data()), "msg1") == 0) {
      aiop2pSend(connection, "msg2", 222, p2pMsgResponse, 5, afNone, 1000000, nullptr, nullptr);
      aiop2pRecvStream(connection, ctx->serverStream, 65536, afNone, 1000000, p2pproto_rw_serverread, ctx);
      end = false;
      ctx->serverState = 3;
    }
  } else if (ctx->serverState == 3) {
    bool valid = true;
    repStruct rep[1024];
    reqStruct *req = stream->data<reqStruct>();
    EXPECT_EQ(status, aosSuccess);
    EXPECT_EQ(header.id, 223u);
    EXPECT_EQ(header.type, p2pMsgRequest);
    EXPECT_EQ(header.size, 1024*sizeof(reqStruct));
    if (stream->sizeOf() == 1024*sizeof(reqStruct)) {
      for (unsigned i = 0; i < 1024; i++) {
        rep[i].c = req[i].a + req[i].b;
        EXPECT_EQ(req[i].a, i);
        EXPECT_EQ(req[i].b, i);
        if (req[i].a != i || req[i].b != i) {
          valid = false;
          break;
        }
      }
    } else {
      valid = false;
    }

    if (status == aosSuccess && valid) {
      ctx->serverState = 4;
      aiop2pSend(connection, rep, header.id, p2pMsgResponse, 1024*sizeof(repStruct), afNone, 1000000, nullptr, ctx);
      aiop2pRecvStream(connection, ctx->serverStream, 4096, afNone, 1000000, p2pproto_rw_serverread, ctx);
      end = false;
    }
  } else if (ctx->serverState == 4) {
    EXPECT_EQ(status, aosSuccess);
    EXPECT_EQ(header.id, 224u);
    EXPECT_EQ(header.type, p2pMsgRequest);
    EXPECT_EQ(header.size, sizeof(reqStruct));
    EXPECT_EQ(ctx->serverStream.sizeOf(), sizeof(reqStruct));
    if (status == aosSuccess &&
        header.id == 224u &&
        header.type == p2pMsgRequest &&
        ctx->serverStream.sizeOf() == sizeof(reqStruct)) {
      ctx->serverState = 5;
      repStruct rep;
      reqStruct *req = stream->data<reqStruct>();
      rep.c = req->a + req->b;
      aiop2pSend(connection, &rep, header.id, p2pMsgResponse, sizeof(repStruct), afNone, 1000000, nullptr, ctx);
      aiop2pRecvStream(connection, ctx->serverStream, 4096, afNone, 1000000, p2pproto_rw_serverread, ctx);
      end = false;
    }
  } else if (ctx->serverState == 5) {
    EXPECT_EQ(status, aosDisconnected);
    if (status == aosDisconnected)
      ctx->serverState = 6;
  }

  if (end) {
    p2pConnectionDelete(connection);
    postQuitOperation(ctx->base);
  }
}

void p2pproto_rw_clientread(AsyncOpStatus status, p2pConnection *connection, p2pHeader header, void *data, void *arg)
{
  bool end = true;
  TestContext *ctx = static_cast<TestContext*>(arg);
  if (ctx->clientState == 1) {
    EXPECT_EQ(status, aosSuccess);
    EXPECT_EQ(header.id, 222u);
    EXPECT_EQ(header.type, p2pMsgResponse);
    EXPECT_EQ(header.size, 5u);
    EXPECT_STREQ(static_cast<const char*>(data), "msg2");
    if (status == aosSuccess &&
        header.id == 222u &&
        header.type == p2pMsgResponse &&
        header.size == 5u &&
        strcmp(static_cast<const char*>(data), "msg2") == 0) {
      ctx->clientState = 2;

      // Send long message
      auto data = std::unique_ptr<reqStruct[]>(new reqStruct[1024]);
      for (unsigned i = 0; i < 1024; i++) {
        data[i].a = i;
        data[i].b = i;
      }

      aiop2pSend(connection, data.get(), 223, p2pMsgRequest, 1024*sizeof(reqStruct), afNone, 1000000, nullptr, nullptr);
      aiop2pRecv(connection, ctx->clientBuffer, 65536, afNone, 1000000, p2pproto_rw_clientread, ctx);
      end = false;
    }
  } else if (ctx->clientState == 2) {
    bool valid = true;
    EXPECT_EQ(status, aosSuccess);
    EXPECT_EQ(header.id, 223u);
    EXPECT_EQ(header.type, p2pMsgResponse);
    EXPECT_EQ(header.size, 1024*sizeof(repStruct));
    if (header.id == 223u &&
        header.type == p2pMsgResponse &&
        header.size == 1024*sizeof(repStruct)) {
      repStruct *rep = static_cast<repStruct*>(data);
      for (unsigned i = 0; i < 1024; i++) {
        EXPECT_EQ(rep[i].c, i+i);
        if (rep[i].c != i+i) {
          valid = false;
          break;
        }
      }
    } else {
      valid = false;
    }

    if (status == aosSuccess && valid) {
      // send message and receive to buffer with small limit
      reqStruct req;
      req.a = 11;
      req.b = 77;
      aiop2pSend(connection, &req, 224, p2pMsgRequest, sizeof(reqStruct), afNone, 1000000, nullptr, nullptr);
      aiop2pRecv(connection, ctx->clientBuffer, 4, afNone, 1000000, p2pproto_rw_clientread, ctx);
      ctx->clientState = 3;
      end = false;
    }
  } else if (ctx->clientState == 3) {
    EXPECT_EQ(status, aosBufferTooSmall);
    if (status == aosBufferTooSmall)
      ctx->clientState = 4;
  }

  if (end) {
    p2pConnectionDelete(connection);
  }
}

p2pErrorTy p2pproto_rw_check(AsyncOpStatus status, p2pConnection *connection, p2pConnectData *data, void *arg)
{
  TestContext *ctx = static_cast<TestContext*>(arg);
  if (data) {
    EXPECT_EQ(ctx->serverState, 0);
    EXPECT_STREQ(data->login, "p2pproto_rw_login");
    EXPECT_STREQ(data->password, "p2pproto_rw_password");
    EXPECT_STREQ(data->application, "p2pproto_rw_application");
    if (ctx->serverState == 0 &&
        strcmp(data->login, "p2pproto_rw_login") == 0 &&
        strcmp(data->password, "p2pproto_rw_password") == 0 &&
        strcmp(data->application, "p2pproto_rw_application") == 0)
      ctx->serverState = 1;
  } else {
    EXPECT_EQ(status, aosSuccess);
    EXPECT_EQ(ctx->serverState, 1);
    if (status == aosSuccess &&
        ctx->serverState == 1)
      ctx->serverState = 2;
    aiop2pRecvStream(connection, ctx->serverStream, 128, afNone, 1000000, p2pproto_rw_serverread, ctx);
  }

  return p2pOk;
}

void p2pproto_rw_accept(AsyncOpStatus status, aioObject *listener, HostAddress client, socketTy acceptSocket, void *arg)
{
  __UNUSED(listener);
  __UNUSED(client);
  TestContext *ctx = static_cast<TestContext*>(arg);
  EXPECT_EQ(status, aosSuccess);
  if (status == aosSuccess) {
    p2pConnection *connection = p2pConnectionNew(newSocketIo(ctx->base, acceptSocket));
    aiop2pAccept(connection, 1000000, p2pproto_rw_check, ctx);
  }
}

void p2pproto_rw_connect(AsyncOpStatus status, p2pConnection *connection, void *arg)
{
  TestContext *ctx = static_cast<TestContext*>(arg);
  EXPECT_EQ(status, aosSuccess);
  if (status == aosSuccess) {
    ctx->clientState = 1;
    aiop2pSend(connection, "msg1", 222, p2pMsgRequest, 5, afNone, 1000000, nullptr, nullptr);
    aiop2pRecv(connection, ctx->clientBuffer, 128, afNone, 1000000, p2pproto_rw_clientread, ctx);
  } else {
    p2pConnectionDelete(connection);
    postQuitOperation(ctx->base);
  }
}

TEST(p2pproto, read_write)
{
  TestContext context(gBase);
  context.serverSocket = startTCPServer(gBase, p2pproto_rw_accept, &context, gPort);
  context.clientSocket = initializeTCPClient(gBase, nullptr, &context, gPort);
  ASSERT_NE(context.serverSocket, nullptr);
  ASSERT_NE(context.clientSocket, nullptr);

  HostAddress address;
  p2pConnectData data;
  address.family = AF_INET;
  address.ipv4 = inet_addr("127.0.0.1");
  address.port = htons(gPort);
  data.login = "p2pproto_rw_login";
  data.password = "p2pproto_rw_password";
  data.application = "p2pproto_rw_application";
  p2pConnection *connection = p2pConnectionNew(context.clientSocket);
  aiop2pConnect(connection, &address, &data, 1000000, p2pproto_rw_connect, &context);
  asyncLoop(gBase);
  deleteAioObject(context.serverSocket);
  ASSERT_EQ(context.serverState, 6);
  ASSERT_EQ(context.clientState, 4);
}

p2pErrorTy p2pproto_coro_ca_check(AsyncOpStatus status, p2pConnection *connection, p2pConnectData *data, void *arg)
{
  __UNUSED(connection);
  TestContext *ctx = static_cast<TestContext*>(arg);
  if (status == aosPending && data) {
    EXPECT_EQ(ctx->serverState, 0);
    EXPECT_STREQ(data->login, "p2pproto_coro_ca_login");
    EXPECT_STREQ(data->password, "p2pproto_coro_ca_password");
    EXPECT_STREQ(data->application, "p2pproto_coro_ca_application");
    if (ctx->serverState == 0 &&
        strcmp(data->login, "p2pproto_coro_ca_login") == 0 &&
        strcmp(data->password, "p2pproto_coro_ca_password") == 0 &&
        strcmp(data->application, "p2pproto_coro_ca_application") == 0)
      ctx->serverState = 1;
  }

  return p2pOk;
}

void p2pproto_coro_ca_listener(void *arg)
{
  TestContext *ctx = static_cast<TestContext*>(arg);

  socketTy socket = ioAccept(ctx->serverSocket, 1000000);
  EXPECT_GT(socket, 0);
  if (socket >= 0) {
    p2pConnection *connection = p2pConnectionNew(newSocketIo(ctx->base, socket));
    int acceptResult = iop2pAccept(connection, 1000000, p2pproto_coro_ca_check, ctx);
    EXPECT_EQ(acceptResult, 0);
    if (acceptResult == 0) {
      ssize_t recvResult;
      p2pHeader header;
      EXPECT_EQ(ctx->serverState, 1);
      recvResult = iop2pRecv(connection, ctx->serverBuffer, 128, afNone, 3000000, &header);
      EXPECT_EQ(recvResult, -aosDisconnected);
      if (recvResult == -aosDisconnected)
        ctx->serverState = 2;
      p2pConnectionDelete(connection);
      postQuitOperation(ctx->base);
    }
  }
}

void p2pproto_coro_ca_client(void *arg)
{
  TestContext *ctx = static_cast<TestContext*>(arg);
  HostAddress address;
  p2pConnectData data;
  address.family = AF_INET;
  address.ipv4 = inet_addr("127.0.0.1");
  address.port = htons(gPort);
  data.login = "p2pproto_coro_ca_login";
  data.password = "p2pproto_coro_ca_password";
  data.application = "p2pproto_coro_ca_application";
  p2pConnection *connection = p2pConnectionNew(ctx->clientSocket);
  int connectResult = iop2pConnect(connection, &address, 1000000, &data);
  EXPECT_EQ(connectResult, 0);
  if (connectResult == 0)
    ctx->clientState = 1;
  p2pConnectionDelete(connection);
}

TEST(p2pproto, coro_connect_accept)
{
  TestContext context(gBase);
  context.serverSocket = startTCPServer(gBase, nullptr, &context, gPort);
  context.clientSocket = initializeTCPClient(gBase, nullptr, &context, gPort);
  ASSERT_NE(context.serverSocket, nullptr);
  ASSERT_NE(context.clientSocket, nullptr);
  coroutineTy *listenerCoro = coroutineNew(p2pproto_coro_ca_listener, &context, 0x10000);
  coroutineTy *clientCoro = coroutineNew(p2pproto_coro_ca_client, &context, 0x10000);
  coroutineCall(listenerCoro);
  coroutineCall(clientCoro);
  asyncLoop(gBase);
  deleteAioObject(context.serverSocket);
  ASSERT_EQ(context.serverState, 2);
  ASSERT_EQ(context.clientState, 1);
}

p2pErrorTy p2pproto_coro_rw_check(AsyncOpStatus status, p2pConnection *connection, p2pConnectData *data, void *arg)
{
  __UNUSED(connection);
  TestContext *ctx = static_cast<TestContext*>(arg);
  if (status == aosPending && data) {
    EXPECT_EQ(ctx->serverState, 0);
    EXPECT_STREQ(data->login, "p2pproto_coro_rw_login");
    EXPECT_STREQ(data->password, "p2pproto_coro_rw_password");
    EXPECT_STREQ(data->application, "p2pproto_coro_rw_application");
    if (ctx->serverState == 0 &&
        strcmp(data->login, "p2pproto_coro_rw_login") == 0 &&
        strcmp(data->password, "p2pproto_coro_rw_password") == 0 &&
        strcmp(data->application, "p2pproto_coro_rw_application") == 0)
      ctx->serverState = 1;
  }

  return p2pOk;
}

void p2pproto_coro_rw_listener(void *arg)
{
  TestContext *ctx = static_cast<TestContext*>(arg);

  socketTy socket = ioAccept(ctx->serverSocket, 1000000);
  EXPECT_GT(socket, 0);
  if (socket > 0) {
    int acceptResult;
    p2pConnection *connection = p2pConnectionNew(newSocketIo(ctx->base, socket));
    acceptResult = iop2pAccept(connection, 1000000, p2pproto_coro_rw_check, ctx);
    EXPECT_EQ(acceptResult, 0);
    if (acceptResult == 0) {
      ssize_t recvResult;
      ssize_t sendResult;
      ctx->serverState = 1;
      p2pStream stream;
      p2pHeader header;

      {
        // Short message
        recvResult = iop2pRecvStream(connection, stream, 4096, afNone, 1000000, &header);
        sendResult = iop2pSend(connection, "msg2", header.id, p2pMsgResponse, 5, afNone, 1000000);
        EXPECT_EQ(recvResult, 5);
        EXPECT_EQ(header.id, 333u);
        EXPECT_EQ(header.type, p2pMsgRequest);
        EXPECT_EQ(header.size, 5u);
        EXPECT_STREQ(static_cast<char*>(stream.data()), "msg1");
        EXPECT_EQ(sendResult, 5);
        if (recvResult == 5 &&
            header.id == 333u &&
            header.type == p2pMsgRequest &&
            header.size == 5u &&
            strcmp(static_cast<char*>(stream.data()), "msg1") == 0 &&
            sendResult == 5) {
          ctx->serverState++;
        }
      }

      {
        // Long message
        bool valid = true;
        recvResult = iop2pRecvStream(connection, stream, 65536, afNone, 1000000, &header);
        reqStruct *req = stream.data<reqStruct>();
        repStruct rep[1024];
        EXPECT_EQ(header.id, 334u);
        EXPECT_EQ(header.type, p2pMsgRequest);
        EXPECT_EQ(recvResult, static_cast<ssize_t>(1024*sizeof(reqStruct)));
        EXPECT_EQ(stream.sizeOf(), 1024*sizeof(reqStruct));
        if (header.id == 334u &&
            header.type == p2pMsgRequest &&
            stream.sizeOf() == 1024*sizeof(reqStruct)) {
          for (unsigned i = 0; i < 1024; i++) {
            rep[i].c = req[i].a + req[i].b;
            EXPECT_EQ(req[i].a, i);
            EXPECT_EQ(req[i].b, i);
            if (req[i].a != i || req[i].b != i) {
              valid = false;
              break;
            }
          }
        } else {
          valid = false;
        }

        if (valid) {
          sendResult = iop2pSend(connection, rep, header.id, p2pMsgResponse, sizeof(rep), afNone, 1000000);
          EXPECT_EQ(sendResult, static_cast<ssize_t>(sizeof(rep)));
          if (sendResult == sizeof(rep))
            ctx->serverState++;
        }
      }

      {
        // Small limit
        recvResult = iop2pRecvStream(connection, stream, 4096, afNone, 1000000, &header);
        reqStruct *req = stream.data<reqStruct>();
        repStruct rep;
        EXPECT_EQ(header.id, 335u);
        EXPECT_EQ(header.type, p2pMsgRequest);
        EXPECT_EQ(recvResult, static_cast<ssize_t>(sizeof(reqStruct)));
        EXPECT_EQ(header.size, sizeof(reqStruct));
        if (header.id == 335u &&
            header.type == p2pMsgRequest &&
            recvResult == sizeof(reqStruct)) {
          rep.c = req->a + req->b;
          sendResult = iop2pSend(connection, &rep, header.id, p2pMsgResponse, sizeof(rep), afNone, 1000000);
          EXPECT_EQ(sendResult, static_cast<ssize_t>(sizeof(rep)));
          if (sendResult == sizeof(rep))
            ctx->serverState++;
        }
      }

      {
        recvResult = iop2pRecvStream(connection, stream, 4096, afNone, 1000000, &header);
        EXPECT_EQ(recvResult, -aosDisconnected);
        if (recvResult == -aosDisconnected)
          ctx->serverState++;
      }
    }

    p2pConnectionDelete(connection);
  }

  postQuitOperation(ctx->base);
}

void p2pproto_coro_rw_client(void *arg)
{
  TestContext *ctx = static_cast<TestContext*>(arg);
  HostAddress address;
  p2pConnectData data;
  address.family = AF_INET;
  address.ipv4 = inet_addr("127.0.0.1");
  address.port = htons(gPort);
  data.login = "p2pproto_coro_rw_login";
  data.password = "p2pproto_coro_rw_password";
  data.application = "p2pproto_coro_rw_application";
  p2pConnection *connection = p2pConnectionNew(ctx->clientSocket);
  int connectResult = iop2pConnect(connection, &address, 1000000, &data);
  EXPECT_EQ(connectResult, 0);
  if (connectResult == 0) {
    p2pHeader header;
    ssize_t recvResult;
    ssize_t sendResult;
    ctx->clientState = 1;

    // short message
    {
      sendResult = iop2pSend(connection, "msg1", 333, p2pMsgRequest, 5, afNone, 1000000);
      recvResult = iop2pRecv(connection, ctx->clientBuffer, 128, afNone, 1000000, &header);
      EXPECT_EQ(sendResult, 5);
      EXPECT_EQ(recvResult, 5);
      EXPECT_EQ(header.id, 333u);
      EXPECT_EQ(header.type, p2pMsgResponse);
      EXPECT_EQ(header.size, 5u);
      EXPECT_STREQ(reinterpret_cast<const char*>(ctx->clientBuffer), "msg2");
      if (sendResult == 5 &&
          recvResult == 5 &&
          header.id == 333 &&
          header.type == p2pMsgResponse &&
          header.size == 5 &&
          strcmp(reinterpret_cast<const char*>(ctx->clientBuffer), "msg2") == 0)
        ctx->clientState++;
    }

    {
      // long message
      bool valid = true;
      auto data = std::unique_ptr<reqStruct[]>(new reqStruct[1024]);
      for (unsigned i = 0; i < 1024; i++) {
        data[i].a = i;
        data[i].b = i;
      }
      sendResult = iop2pSend(connection, data.get(), 334, p2pMsgRequest, 1024*sizeof(reqStruct), afNone, 1000000);
      recvResult = iop2pRecv(connection, ctx->clientBuffer, 65536, afNone, 1000000, &header);
      repStruct *rep = reinterpret_cast<repStruct*>(ctx->clientBuffer);
      EXPECT_EQ(header.id, 334u);
      EXPECT_EQ(header.type, p2pMsgResponse);
      EXPECT_EQ(sendResult, static_cast<ssize_t>(1024*sizeof(reqStruct)));
      EXPECT_EQ(recvResult, static_cast<ssize_t>(1024*sizeof(repStruct)));
      if (header.id == 334u &&
          header.type == p2pMsgResponse &&
          recvResult == 1024*sizeof(repStruct)) {
        for (unsigned i = 0; i < 1024; i++) {
          EXPECT_EQ(rep[i].c, i+i);
          if (rep[i].c != i+i) {
            valid = false;
            break;
          }
        }
      } else {
        valid = false;
      }

      if (valid)
        ctx->clientState++;
    }

    {
      // limit check
      reqStruct req;
      req.a = 99;
      req.b = 99;
      iop2pSend(connection, &req, 335, p2pMsgRequest, sizeof(req), afNone, 1000000);
      recvResult = iop2pRecv(connection, ctx->clientBuffer, 4, afNone, 1000000, &header);
      EXPECT_EQ(recvResult, -aosBufferTooSmall);
      if (recvResult == -aosBufferTooSmall)
        ctx->clientState++;
    }
  }

  p2pConnectionDelete(connection);
}

TEST(p2pproto, coro_read_write)
{
  TestContext context(gBase);
  context.serverSocket = startTCPServer(gBase, nullptr, &context, gPort);
  context.clientSocket = initializeTCPClient(gBase, nullptr, &context, gPort);
  ASSERT_NE(context.serverSocket, nullptr);
  ASSERT_NE(context.clientSocket, nullptr);
  coroutineTy *listenerCoro = coroutineNew(p2pproto_coro_rw_listener, &context, 0x10000);
  coroutineTy *clientCoro = coroutineNew(p2pproto_coro_rw_client, &context, 0x10000);
  coroutineCall(listenerCoro);
  coroutineCall(clientCoro);
  asyncLoop(gBase);
  deleteAioObject(context.serverSocket);
  ASSERT_EQ(context.serverState, 5);
  ASSERT_EQ(context.clientState, 4);
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
