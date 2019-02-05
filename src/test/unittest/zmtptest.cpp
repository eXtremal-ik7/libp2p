#include "unittest.h"
#include <asyncio/coroutine.h>
#include <asyncioextras/zmtp.h>
#include <zmq.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

__NO_PADDING_BEGIN
struct zmtpContext {
  asyncBase *base;
  std::atomic<bool> serverRunning;
  std::atomic<bool> clientRunning;
  int clientState;
  int serverState;
  aioObject *listener;
  zmtpSocket *clientSocket;
  zmtpSocket *serverSocket;
  zmtpStream stream;
  zmtpContext(asyncBase *baseArg) : base(baseArg), serverRunning(false), clientRunning(false), clientState(0), serverState(0) {}
};
__NO_PADDING_END

static bool waitClient(zmtpContext *ctx)
{
  for (unsigned i = 0; i < 5000; i++) {
    if (ctx->clientRunning == false)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  return ctx->clientRunning == false;
}

static bool waitServer(zmtpContext *ctx, bool event=false)
{
  for (unsigned i = 0; i < 5000; i++) {
    if (ctx->serverRunning == event)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  return ctx->serverRunning == event;
}

static void zmq_server_pull(zmtpContext *ctx, uint16_t port)
{
  auto zmqCtx = zmq_ctx_new();
  auto socket = zmq_socket(zmqCtx, ZMQ_PULL);

  int timeout = 3000;
  zmq_setsockopt (socket, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));

  char address[128];
  snprintf(address, sizeof(address), "tcp://127.0.0.1:%u", static_cast<unsigned>(port));
  int bindResult;
  if ( (bindResult = zmq_bind(socket, address)) != 0) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    bindResult = zmq_bind(socket, address);
  }

  EXPECT_EQ(bindResult, 0);
  if (bindResult == 0) {
    int recvResult;
    ctx->serverRunning = true;
    ctx->serverState = 1;

    {
      reqStruct msg = {0, 0};
      recvResult = zmq_recv(socket, &msg, sizeof(msg), 0);
      EXPECT_EQ(recvResult, static_cast<ssize_t>(sizeof(msg)));
      EXPECT_EQ(msg.a, 11u);
      EXPECT_EQ(msg.b, 77u);
      if (recvResult == sizeof(msg) && msg.a == 11 && msg.b == 77)
        ctx->serverState++;
    }

    {
      reqStruct longMsg[1024];
      recvResult = zmq_recv(socket, longMsg, sizeof(longMsg), 0);
      EXPECT_EQ(recvResult, static_cast<ssize_t>(sizeof(longMsg)));
      bool valid = true;
      for (unsigned i = 0; i < 1024; i++) {
        EXPECT_EQ(longMsg[i].a, i);
        EXPECT_EQ(longMsg[i].b, i);
        if (longMsg[i].a != i || longMsg[i].b != i) {
          valid = false;
          break;
        }
      }

      if (valid)
        ctx->serverState++;
    }
  }

  ctx->serverRunning = false;
  EXPECT_EQ(waitClient(ctx), true);
  zmq_close(socket);
  zmq_term(zmqCtx);
}

static void zmq_server_rep(zmtpContext *ctx, uint16_t port)
{
  auto zmqCtx = zmq_ctx_new();
  auto socket = zmq_socket(zmqCtx, ZMQ_REP);

  int linger = 0;
  int timeout = 3000;
  zmq_setsockopt(socket, ZMQ_RCVTIMEO, &timeout, sizeof(timeout));
  zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(int));

  char address[128];
  snprintf(address, sizeof(address), "tcp://127.0.0.1:%u", static_cast<unsigned>(port));
  int bindResult;
  if ( (bindResult = zmq_bind(socket, address)) != 0) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    bindResult = zmq_bind(socket, address);
  }

  EXPECT_EQ(bindResult, 0);
  if (bindResult == 0) {
    int recvResult;
    ctx->serverRunning = true;
    ctx->serverState = 1;

    {
      reqStruct msg = {0, 0};
      recvResult = zmq_recv(socket, &msg, sizeof(msg), 0);
      EXPECT_EQ(recvResult, static_cast<ssize_t>(sizeof(msg)));
      EXPECT_EQ(msg.a, 11u);
      EXPECT_EQ(msg.b, 77u);
      if (recvResult == sizeof(msg) && msg.a == 11 && msg.b == 77) {
        repStruct rep;
        rep.c = msg.a + msg.b;
        if (zmq_send(socket, &rep, sizeof(rep), 0) == sizeof(rep))
          ctx->serverState++;
      }
    }

    {
      reqStruct longReq[1024];
      repStruct longRep[1024];
      recvResult = zmq_recv(socket, longReq, sizeof(longReq), 0);
      EXPECT_EQ(recvResult, static_cast<ssize_t>(sizeof(longReq)));
      bool valid = true;
      for (unsigned i = 0; i < 1024; i++) {
        longRep[i].c = longReq[i].a + longReq[i].b;
        EXPECT_EQ(longReq[i].a, i);
        EXPECT_EQ(longReq[i].b, i);
        if (longReq[i].a != i || longReq[i].b != i) {
          valid = false;
          break;
        }
      }

      if (valid) {
        if (zmq_send(socket, longRep, sizeof(longRep), 0) == sizeof(longRep))
          ctx->serverState++;
      }
    }

    {
      reqStruct msg;
      recvResult = zmq_recv(socket, &msg, sizeof(msg), 0);
      EXPECT_EQ(recvResult, static_cast<ssize_t>(sizeof(msg)));
      if (recvResult == sizeof(msg)) {
        repStruct rep;
        rep.c = msg.a + msg.b;
        if (zmq_send(socket, &rep, sizeof(rep), 0) == sizeof(rep))
          ctx->serverState++;
      }
    }
  }

  ctx->serverRunning = false;
  EXPECT_EQ(waitClient(ctx), true);
  zmq_close(socket);
  zmq_term(zmqCtx);
}

static void zmq_server_push(zmtpContext *ctx, uint16_t port)
{
  auto zmqCtx = zmq_ctx_new();
  auto socket = zmq_socket(zmqCtx, ZMQ_PUSH);

  int linger = 0;
  int timeout = 3000;
  zmq_setsockopt (socket, ZMQ_SNDTIMEO, &timeout, sizeof(timeout));
  zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(int));

  char address[128];
  snprintf(address, sizeof(address), "tcp://127.0.0.1:%u", static_cast<unsigned>(port));
  int connectResult = zmq_connect(socket, address);
  EXPECT_EQ(connectResult, 0);
  if (connectResult == 0) {
    ctx->clientState = 1;
    // Short message
    {
      reqStruct req;
      req.a = 11;
      req.b = 77;
      if (zmq_send(socket, &req, sizeof(req), 0) == sizeof(req))
        ctx->clientState++;
    }

    // Long Message
    {
      auto data = std::unique_ptr<reqStruct[]>(new reqStruct[1024]);
      for (unsigned i = 0; i < 1024; i++) {
        data[i].a = i;
        data[i].b = i;
      }
      if (zmq_send(socket, data.get(), 1024*sizeof(reqStruct), 0) == 1024*sizeof(reqStruct))
        ctx->clientState++;
    }

    // Limit check
    {
      reqStruct req;
      req.a = 99;
      req.b = 99;
      if (zmq_send(socket, &req, sizeof(req), 0) == sizeof(req))
        ctx->clientState++;
    }
  }

  EXPECT_EQ(waitServer(ctx), true);
  zmq_close(socket);
  zmq_term(zmqCtx);
  ctx->clientRunning = false;
}

static void zmq_server_req(zmtpContext *ctx, uint16_t port)
{
  auto zmqCtx = zmq_ctx_new();
  auto socket = zmq_socket(zmqCtx, ZMQ_REQ);

  int linger = 0;
  int timeout = 3000;
  zmq_setsockopt (socket, ZMQ_SNDTIMEO, &timeout, sizeof(timeout));
  zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(int));

  char address[128];
  snprintf(address, sizeof(address), "tcp://127.0.0.1:%u", static_cast<unsigned>(port));
  int connectResult = zmq_connect(socket, address);
  EXPECT_EQ(connectResult, 0);
  if (connectResult == 0) {
    int recvResult;
    ctx->clientState = 1;
    // Short message
    {
      reqStruct req;
      repStruct rep = {0};
      req.a = 11;
      req.b = 77;
      zmq_send(socket, &req, sizeof(req), 0);
      recvResult = zmq_recv(socket, &rep, sizeof(rep), 0);
      EXPECT_EQ(recvResult, static_cast<ssize_t>(sizeof(rep)));
      EXPECT_EQ(rep.c, req.a+req.b);
      if (recvResult == sizeof(rep) && rep.c == req.a+req.b)
        ctx->clientState++;
    }

    // Long Message
    {
      bool valid = true;
      auto data = std::unique_ptr<reqStruct[]>(new reqStruct[1024]);
      auto rep = std::unique_ptr<repStruct[]>(new repStruct[1024]);
      for (unsigned i = 0; i < 1024; i++) {
        data[i].a = i;
        data[i].b = i;
      }
      zmq_send(socket, data.get(), 1024*sizeof(reqStruct), 0);
      recvResult = zmq_recv(socket, rep.get(), 1024*sizeof(repStruct), 0);
      EXPECT_EQ(recvResult, static_cast<ssize_t>(1024*sizeof(repStruct)));
      if (recvResult == 1024*sizeof(repStruct)) {
        for (unsigned i = 0; i < 1024; i++) {
          EXPECT_EQ(rep[i].c, data[i].a + data[i].b);
          if (rep[i].c != data[i].a + data[i].b) {
            valid = false;
            break;
          }
        }
      } else {
        valid = false;
      }

      if (valid) {
        ctx->clientState++;
      }
    }

    // Limit check
    {
      reqStruct req;
      req.a = 99;
      req.b = 99;
      zmq_send(socket, &req, sizeof(req), 0);
      ctx->clientState++;
    }
  }

  EXPECT_EQ(waitServer(ctx), true);
  zmq_close(socket);
  zmq_term(zmqCtx);
  ctx->clientRunning = false;
}

static void aio_push_writecb(AsyncOpStatus status, zmtpSocket*, void *arg)
{
  EXPECT_EQ(status, aosSuccess);
  auto ctx = static_cast<zmtpContext*>(arg);
  if (status == aosSuccess)
    ctx->clientState++;
}

static void aio_push_connectcb(AsyncOpStatus status, zmtpSocket *client, void *arg)
{
  EXPECT_EQ(status, aosSuccess);
  auto ctx = static_cast<zmtpContext*>(arg);
  if (status == aosSuccess) {
    reqStruct req;
    req.a = 11;
    req.b = 77;
    ctx->clientState = 1;
    // short message
    aioZmtpSend(ctx->clientSocket, &req, sizeof(req), zmtpMessage, afNone, 1000000, aio_push_writecb, ctx);

    // long message
    {
      auto data = std::unique_ptr<reqStruct[]>(new reqStruct[1024]);
      for (unsigned i = 0; i < 1024; i++) {
        data[i].a = i;
        data[i].b = i;
      }
      aioZmtpSend(ctx->clientSocket, data.get(), 1024*sizeof(reqStruct), zmtpMessage, afNone, 1000000, aio_push_writecb, ctx);
    }
  }

  EXPECT_EQ(waitServer(ctx), true);
  zmtpSocketDelete(client);
  postQuitOperation(ctx->base);
  ctx->clientRunning = false;
}


TEST(zmtp, aio_push)
{
  zmtpContext context(gBase);
  std::thread serverThread(zmq_server_pull, &context, gPort);

  // Wait server
  EXPECT_EQ(waitServer(&context, true), true);
  context.clientSocket = zmtpSocketNew(gBase, initializeTCPClient(gBase, nullptr, nullptr, 0), zmtpSocketPUSH);
  context.clientRunning = true;

  HostAddress address;
  address.ipv4 = inet_addr("127.0.0.1");
  address.family = AF_INET;
  address.port = htons(gPort);
  aioZmtpConnect(context.clientSocket, &address, afNone, 1000000, aio_push_connectcb, &context);
  asyncLoop(gBase);
  serverThread.join();
  ASSERT_EQ(context.clientState, 3);
  ASSERT_EQ(context.serverState, 3);
}

void aio_push_client_coro(void *arg)
{
  auto ctx = static_cast<zmtpContext*>(arg);

  HostAddress address;
  address.ipv4 = inet_addr("127.0.0.1");
  address.family = AF_INET;
  address.port = htons(gPort);
  int connectResult = ioZmtpConnect(ctx->clientSocket, &address, afNone, 1000000);
  EXPECT_EQ(connectResult, 0);
  if (connectResult == 0) {
    ctx->clientState = 1;
    ssize_t sendResult;
    {
      // short message
      reqStruct req;
      req.a = 11;
      req.b = 77;
      sendResult = ioZmtpSend(ctx->clientSocket, &req, sizeof(req), zmtpMessage, afNone, 1000000);
      EXPECT_EQ(sendResult, static_cast<ssize_t>(sizeof(req)));
      if (sendResult == sizeof(req))
        ctx->clientState++;
    }

    {
      // long message
      auto data = std::unique_ptr<reqStruct[]>(new reqStruct[1024]);
      for (unsigned i = 0; i < 1024; i++) {
        data[i].a = i;
        data[i].b = i;
      }
      sendResult = ioZmtpSend(ctx->clientSocket, data.get(), 1024*sizeof(reqStruct), zmtpMessage, afNone, 1000000);
      EXPECT_EQ(sendResult, static_cast<ssize_t>(1024*sizeof(reqStruct)));
      if (sendResult == 1024*sizeof(reqStruct))
        ctx->clientState++;
    }
  }

  EXPECT_EQ(waitServer(ctx), true);
  zmtpSocketDelete(ctx->clientSocket);
  postQuitOperation(ctx->base);
  ctx->clientRunning = false;
}

TEST(zmtp, aio_push_coro)
{
  zmtpContext context(gBase);
  std::thread serverThread(zmq_server_pull, &context, gPort);

  // Wait server
  EXPECT_EQ(waitServer(&context, true), true);
  context.clientSocket = zmtpSocketNew(gBase, initializeTCPClient(gBase, nullptr, nullptr, 0), zmtpSocketPUSH);
  context.clientRunning = true;

  coroutineCall(coroutineNew(aio_push_client_coro, &context, 0x10000));
  asyncLoop(gBase);
  serverThread.join();
  ASSERT_EQ(context.clientState, 3);
  ASSERT_EQ(context.serverState, 3);
}

static void aio_pull_readcb(AsyncOpStatus status, zmtpSocket *socket, zmtpUserMsgTy type, zmtpStream *stream, void *arg)
{
  bool end = true;
  auto ctx = static_cast<zmtpContext*>(arg);
  if (ctx->serverState == 1) {
    // Check short message, read long message
    reqStruct *req = stream->data<reqStruct>();
    EXPECT_EQ(status, aosSuccess);
    EXPECT_EQ(type, zmtpMessage);
    EXPECT_EQ(stream->sizeOf(), sizeof(reqStruct));
    EXPECT_EQ(req->a, 11u);
    EXPECT_EQ(req->b, 77u);
    if (status == aosSuccess &&
        stream->sizeOf() == sizeof(reqStruct) &&
        req->a == 11 &&
        req->b == 77) {
      ctx->serverState = 2;
      aioZmtpRecv(socket, ctx->stream, 65536, afNone, 1000000, aio_pull_readcb, ctx);
      end = false;
    }
  } else if (ctx->serverState == 2) {
    // Check long message, read limited
    bool valid = true;
    reqStruct *req = stream->data<reqStruct>();
    EXPECT_EQ(status, aosSuccess);
    EXPECT_EQ(type, zmtpMessage);
    EXPECT_EQ(stream->sizeOf(), 1024*sizeof(reqStruct));
    for (unsigned i = 0; i < 1024; i++) {
      EXPECT_EQ(req[i].a, i);
      EXPECT_EQ(req[i].b, i);
      if (req[i].a != i || req[i].b != i) {
        valid = false;
        break;
      }
    }

    if (status == aosSuccess && stream->sizeOf() == 1024*sizeof(reqStruct) && valid) {
      ctx->serverState = 3;
      aioZmtpRecv(socket, ctx->stream, 8, afNone, 1000000, aio_pull_readcb, ctx);
      end = false;
    }
  } else if (ctx->serverState == 3) {
    // Check limit
    EXPECT_EQ(status, aosBufferTooSmall);
    if (status == aosBufferTooSmall)
      ctx->serverState = 4;
  }

  if (end) {
    ctx->serverRunning = false;
    EXPECT_EQ(waitClient(ctx), true);
    zmtpSocketDelete(ctx->serverSocket);
    postQuitOperation(ctx->base);
  }
}

static void aio_pull_zmtpacceptcb(AsyncOpStatus status, zmtpSocket *socket, void *arg)
{
  EXPECT_EQ(status, aosSuccess);
  auto ctx = static_cast<zmtpContext*>(arg);
  if (status == aosSuccess) {
    ctx->serverState = 1;
    aioZmtpRecv(socket, ctx->stream, 1024, afNone, 1000000, aio_pull_readcb, ctx);
  } else {
    ctx->serverRunning = false;
    EXPECT_EQ(waitClient(ctx), true);
    zmtpSocketDelete(ctx->serverSocket);
    postQuitOperation(ctx->base);
  }
}

static void aio_pull_acceptcb(AsyncOpStatus status, aioObject*, HostAddress, socketTy acceptSocket, void *arg)
{
  EXPECT_EQ(status, aosSuccess);
  auto ctx = static_cast<zmtpContext*>(arg);
  if (status == aosSuccess) {
    ctx->serverSocket = zmtpSocketNew(ctx->base, newSocketIo(ctx->base, acceptSocket), zmtpSocketPULL);
    aioZmtpAccept(ctx->serverSocket, afNone, 1000000, aio_pull_zmtpacceptcb, ctx);
  } else {
    ctx->serverRunning = false;
    postQuitOperation(ctx->base);
  }
}

TEST(zmtp, aio_pull)
{
  zmtpContext context(gBase);
  context.listener = startTCPServer(gBase, aio_pull_acceptcb, &context, gPort);
  context.clientRunning = true;
  context.serverRunning = true;
  ASSERT_NE(context.listener, nullptr);
  std::thread clientThread(zmq_server_push, &context, gPort);
  asyncLoop(gBase);
  clientThread.join();
  deleteAioObject(context.listener);
  ASSERT_EQ(context.clientState, 4);
  ASSERT_EQ(context.serverState, 4);
}

void aio_pull_accept_coro(void *arg)
{
  zmtpSocket *socket = nullptr;
  auto ctx = static_cast<zmtpContext*>(arg);
  socketTy fd = ioAccept(ctx->listener, 1000000);
  EXPECT_GT(fd, 0);
  if (fd > 0) {
    socket = zmtpSocketNew(ctx->base, newSocketIo(ctx->base, fd), zmtpSocketPULL);
    int acceptResult = ioZmtpAccept(socket, afNone, 3000000);
    EXPECT_EQ(acceptResult, 0);
    if (acceptResult == 0) {
      ssize_t recvResult;
      zmtpUserMsgTy msgType;
      ctx->serverState = 1;

      {
        // Short message
        recvResult = ioZmtpRecv(socket, ctx->stream, 1024, afNone, 1000000, &msgType);
        reqStruct *req = ctx->stream.data<reqStruct>();
        EXPECT_EQ(msgType, zmtpMessage);
        EXPECT_EQ(recvResult, static_cast<ssize_t>(sizeof(reqStruct)));
        EXPECT_EQ(ctx->stream.sizeOf(), sizeof(reqStruct));
        EXPECT_EQ(req->a, 11u);
        EXPECT_EQ(req->b, 77u);
        if (msgType == zmtpMessage &&
            recvResult == sizeof(reqStruct) &&
            ctx->stream.sizeOf() == sizeof(reqStruct) &&
            req->a == 11 &&
            req->b == 77) {
          ctx->serverState++;
        }
      }

      {
        // Long message
        bool valid = true;
        recvResult = ioZmtpRecv(socket, ctx->stream, 65536, afNone, 1000000, &msgType);
        reqStruct *req = ctx->stream.data<reqStruct>();
        EXPECT_EQ(msgType, zmtpMessage);
        EXPECT_EQ(recvResult, static_cast<ssize_t>(1024*sizeof(reqStruct)));
        EXPECT_EQ(ctx->stream.sizeOf(), 1024*sizeof(reqStruct));
        for (unsigned i = 0; i < 1024; i++) {
          EXPECT_EQ(req[i].a, i);
          EXPECT_EQ(req[i].b, i);
          if (req[i].a != i || req[i].b != i) {
            valid = false;
            break;
          }
        }

        if (msgType == zmtpMessage &&
            recvResult == 1024*sizeof(reqStruct) &&
            ctx->stream.sizeOf() == 1024*sizeof(reqStruct) &&
            valid) {
          ctx->serverState++;
        }
      }

      {
        // Small limit
        recvResult = ioZmtpRecv(socket, ctx->stream, 8, afNone, 1000000, &msgType);
        EXPECT_EQ(recvResult, -aosBufferTooSmall);
        if (recvResult == -aosBufferTooSmall)
          ctx->serverState++;
      }
    }
  }

  ctx->serverRunning = false;
  EXPECT_EQ(waitClient(ctx), true);
  if (socket)
    zmtpSocketDelete(socket);
  postQuitOperation(ctx->base);
}

TEST(zmtp, aio_pull_coro)
{
  zmtpContext context(gBase);
  context.listener = startTCPServer(gBase, nullptr, nullptr, gPort);
  context.clientRunning = true;
  context.serverRunning = true;
  ASSERT_NE(context.listener, nullptr);
  std::thread clientThread(zmq_server_push, &context, gPort);
  coroutineCall(coroutineNew(aio_pull_accept_coro, &context, 0x10000));
  asyncLoop(gBase);
  clientThread.join();
  deleteAioObject(context.listener);
  ASSERT_EQ(context.clientState, 4);
  ASSERT_EQ(context.serverState, 4);
}

static void aio_req_writecb(AsyncOpStatus status, zmtpSocket *socket, void *arg)
{
  EXPECT_EQ(status, aosSuccess);
  auto ctx = static_cast<zmtpContext*>(arg);
  if (status != aosSuccess) {
    EXPECT_EQ(waitServer(ctx), true);
    zmtpSocketDelete(socket);
    postQuitOperation(ctx->base);
    ctx->clientRunning = false;
  }
}

static void aio_req_readcb(AsyncOpStatus status, zmtpSocket *socket, zmtpUserMsgTy type, zmtpStream *stream, void *arg)
{
  bool end = true;
  auto ctx = static_cast<zmtpContext*>(arg);
  if (ctx->clientState == 1) {
    EXPECT_EQ(status, aosSuccess);
    EXPECT_EQ(type, zmtpMessage);
    EXPECT_EQ(stream->sizeOf(), sizeof(repStruct));
    EXPECT_EQ(stream->data<repStruct>()->c, 88u);
    if (status == aosSuccess &&
        type == zmtpMessage &&
        stream->sizeOf() == sizeof(repStruct) &&
        stream->data<repStruct>()->c == 88) {
      ctx->clientState = 2;
      end = false;

      // send long message
      {
        auto data = std::unique_ptr<reqStruct[]>(new reqStruct[1024]);
        for (unsigned i = 0; i < 1024; i++) {
          data[i].a = i;
          data[i].b = i;
        }
        aioZmtpSend(socket, data.get(), 1024*sizeof(reqStruct), zmtpMessage, afNone, 1000000, aio_req_writecb, ctx);
        aioZmtpRecv(socket, ctx->stream, 65536, afNone, 1000000, aio_req_readcb, ctx);
      }
    }
  } else if (ctx->clientState == 2) {
    // check long response
    bool valid = true;
    repStruct *rep = ctx->stream.data<repStruct>();
    EXPECT_EQ(status, aosSuccess);
    EXPECT_EQ(type, zmtpMessage);
    EXPECT_EQ(stream->sizeOf(), 1024*sizeof(repStruct));
    if (stream->sizeOf() == 1024*sizeof(repStruct)) {
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
      aioZmtpSend(socket, &req, sizeof(req), zmtpMessage, afNone, 1000000, aio_req_writecb, ctx);
      aioZmtpRecv(socket, ctx->stream, 4, afNone, 1000000, aio_req_readcb, ctx);
      ctx->clientState = 3;
      end = false;
    }
  } else if (ctx->clientState == 3) {
    EXPECT_EQ(status, aosBufferTooSmall);
    if (status == aosBufferTooSmall)
      ctx->clientState = 4;
  }

  if (end) {
    EXPECT_EQ(waitServer(ctx), true);
    zmtpSocketDelete(ctx->clientSocket);
    postQuitOperation(ctx->base);
    ctx->clientRunning = false;
  }
}

static void aio_req_connectcb(AsyncOpStatus status, zmtpSocket *client, void *arg)
{
  EXPECT_EQ(status, aosSuccess);
  auto ctx = static_cast<zmtpContext*>(arg);
  if (status == aosSuccess) {
    reqStruct req;
    req.a = 11;
    req.b = 77;
    ctx->clientState = 1;
    // short message
    aioZmtpSend(ctx->clientSocket, &req, sizeof(req), zmtpMessage, afNone, 1000000, aio_req_writecb, ctx);
    aioZmtpRecv(client, ctx->stream, 1024, afNone, 1000000, aio_req_readcb, ctx);
  } else {
    EXPECT_EQ(waitServer(ctx), true);
    zmtpSocketDelete(client);
    postQuitOperation(ctx->base);
    ctx->clientRunning = false;
  }
}

TEST(zmtp, aio_req)
{
  zmtpContext context(gBase);
  std::thread serverThread(zmq_server_rep, &context, gPort);

  // Wait server
  EXPECT_EQ(waitServer(&context, true), true);
  context.clientSocket = zmtpSocketNew(gBase, initializeTCPClient(gBase, nullptr, nullptr, 0), zmtpSocketREQ);
  context.clientRunning = true;

  HostAddress address;
  address.ipv4 = inet_addr("127.0.0.1");
  address.family = AF_INET;
  address.port = htons(gPort);
  aioZmtpConnect(context.clientSocket, &address, afNone, 1000000, aio_req_connectcb, &context);
  asyncLoop(gBase);
  serverThread.join();
  ASSERT_EQ(context.clientState, 4);
  ASSERT_EQ(context.serverState, 4);
}

void aio_req_client_coro(void *arg)
{
  auto ctx = static_cast<zmtpContext*>(arg);

  HostAddress address;
  address.ipv4 = inet_addr("127.0.0.1");
  address.family = AF_INET;
  address.port = htons(gPort);
  int connectResult = ioZmtpConnect(ctx->clientSocket, &address, afNone, 1000000);
  EXPECT_EQ(connectResult, 0);
  if (connectResult == 0) {
    ctx->clientState = 1;
    zmtpUserMsgTy msgType;
    ssize_t recvResult;
    ssize_t sendResult;
    {
      // short message
      reqStruct req;
      req.a = 11;
      req.b = 77;
      sendResult = ioZmtpSend(ctx->clientSocket, &req, sizeof(req), zmtpMessage, afNone, 1000000);
      recvResult = ioZmtpRecv(ctx->clientSocket, ctx->stream, 1024, afNone, 1000000, &msgType);
      repStruct *rep = ctx->stream.data<repStruct>();
      EXPECT_EQ(sendResult, static_cast<ssize_t>(sizeof(req)));
      EXPECT_EQ(recvResult, static_cast<ssize_t>(sizeof(repStruct)));
      EXPECT_EQ(msgType, zmtpMessage);
      EXPECT_EQ(ctx->stream.sizeOf(), sizeof(repStruct));
      EXPECT_EQ(rep->c, req.a + req.b);
      if (msgType == zmtpMessage &&
          recvResult == sizeof(repStruct) &&
          ctx->stream.sizeOf() == sizeof(repStruct) &&
          rep->c == req.a + req.b) {
        ctx->clientState++;
      }
    }

    {
      // long message
      bool valid = true;
      auto data = std::unique_ptr<reqStruct[]>(new reqStruct[1024]);
      for (unsigned i = 0; i < 1024; i++) {
        data[i].a = i;
        data[i].b = i;
      }
      sendResult = ioZmtpSend(ctx->clientSocket, data.get(), 1024*sizeof(reqStruct), zmtpMessage, afNone, 1000000);
      recvResult = ioZmtpRecv(ctx->clientSocket, ctx->stream, 65536, afNone, 1000000, &msgType);
      repStruct *rep = ctx->stream.data<repStruct>();
      EXPECT_EQ(sendResult, static_cast<ssize_t>(1024*sizeof(reqStruct)));
      EXPECT_EQ(recvResult, static_cast<ssize_t>(1024*sizeof(repStruct)));
      EXPECT_EQ(ctx->stream.sizeOf(), 1024*sizeof(repStruct));
      if (ctx->stream.sizeOf() == 1024*sizeof(repStruct)) {
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
      ioZmtpSend(ctx->clientSocket, &req, sizeof(req), zmtpMessage, afNone, 1000000);
      recvResult = ioZmtpRecv(ctx->clientSocket, ctx->stream, 4, afNone, 1000000, &msgType);
      EXPECT_EQ(recvResult, -aosBufferTooSmall);
      if (recvResult == -aosBufferTooSmall)
        ctx->clientState++;
    }
  }

  EXPECT_EQ(waitServer(ctx), true);
  zmtpSocketDelete(ctx->clientSocket);
  postQuitOperation(ctx->base);
  ctx->clientRunning = false;
}

TEST(zmtp, aio_req_coro)
{
  zmtpContext context(gBase);
  std::thread serverThread(zmq_server_rep, &context, gPort);

  // Wait server
  EXPECT_EQ(waitServer(&context, true), true);
  context.clientSocket = zmtpSocketNew(gBase, initializeTCPClient(gBase, nullptr, nullptr, 0), zmtpSocketREQ);
  context.clientRunning = true;

  coroutineCall(coroutineNew(aio_req_client_coro, &context, 0x10000));
  asyncLoop(gBase);
  serverThread.join();
  ASSERT_EQ(context.clientState, 4);
  ASSERT_EQ(context.serverState, 4);
}

static void aio_rep_writecb(AsyncOpStatus status, zmtpSocket *socket, void *arg)
{
  EXPECT_EQ(status, aosSuccess);
  auto ctx = static_cast<zmtpContext*>(arg);
  if (status != aosSuccess) {
    ctx->serverRunning = false;
    EXPECT_EQ(waitClient(ctx), true);
    zmtpSocketDelete(socket);
    postQuitOperation(ctx->base);
  }
}

static void aio_rep_readcb(AsyncOpStatus status, zmtpSocket *socket, zmtpUserMsgTy type, zmtpStream *stream, void *arg)
{
  bool end = true;
  auto ctx = static_cast<zmtpContext*>(arg);
  if (ctx->serverState == 1) {
    // Check short message, read long message
    reqStruct *req = stream->data<reqStruct>();
    EXPECT_EQ(status, aosSuccess);
    EXPECT_EQ(type, zmtpMessage);
    EXPECT_EQ(stream->sizeOf(), sizeof(reqStruct));
    EXPECT_EQ(req->a, 11u);
    EXPECT_EQ(req->b, 77u);
    if (status == aosSuccess &&
        stream->sizeOf() == sizeof(reqStruct) &&
        req->a == 11 &&
        req->b == 77) {
      ctx->serverState = 2;
      repStruct rep;
      rep.c = req->a + req->b;
      aioZmtpSend(socket, &rep, sizeof(rep), zmtpMessage, afNone, 1000000, aio_rep_writecb, ctx);
      aioZmtpRecv(socket, ctx->stream, 65536, afNone, 1000000, aio_rep_readcb, ctx);
      end = false;
    }
  } else if (ctx->serverState == 2) {
    // Check long message, read limited
    bool valid = true;
    repStruct rep[1024];
    reqStruct *req = stream->data<reqStruct>();
    EXPECT_EQ(status, aosSuccess);
    EXPECT_EQ(type, zmtpMessage);
    EXPECT_EQ(stream->sizeOf(), 1024*sizeof(reqStruct));
    for (unsigned i = 0; i < 1024; i++) {
      rep[i].c = req[i].a + req[i].b;
      EXPECT_EQ(req[i].a, i);
      EXPECT_EQ(req[i].b, i);
      if (req[i].a != i || req[i].b != i) {
        valid = false;
        break;
      }
    }

    if (status == aosSuccess && stream->sizeOf() == 1024*sizeof(reqStruct) && valid) {
      ctx->serverState = 3;
      aioZmtpSend(socket, rep, sizeof(rep), zmtpMessage, afNone, 1000000, aio_rep_writecb, ctx);
      aioZmtpRecv(socket, ctx->stream, 8, afNone, 1000000, aio_rep_readcb, ctx);
      end = false;
    }
  } else if (ctx->serverState == 3) {
    // Check limit
    EXPECT_EQ(status, aosBufferTooSmall);
    if (status == aosBufferTooSmall)
      ctx->serverState = 4;
  }

  if (end) {
    ctx->serverRunning = false;
    EXPECT_EQ(waitClient(ctx), true);
    zmtpSocketDelete(ctx->serverSocket);
    postQuitOperation(ctx->base);
  }
}

static void aio_rep_zmtpacceptcb(AsyncOpStatus status, zmtpSocket *socket, void *arg)
{
  EXPECT_EQ(status, aosSuccess);
  auto ctx = static_cast<zmtpContext*>(arg);
  if (status == aosSuccess) {
    ctx->serverState = 1;
    aioZmtpRecv(socket, ctx->stream, 1024, afNone, 1000000, aio_rep_readcb, ctx);
  } else {
    ctx->serverRunning = false;
    EXPECT_EQ(waitClient(ctx), true);
    zmtpSocketDelete(ctx->serverSocket);
    postQuitOperation(ctx->base);
  }
}


static void aio_rep_acceptcb(AsyncOpStatus status, aioObject*, HostAddress, socketTy acceptSocket, void *arg)
{
  EXPECT_EQ(status, aosSuccess);
  auto ctx = static_cast<zmtpContext*>(arg);
  if (status == aosSuccess) {
    ctx->serverSocket = zmtpSocketNew(ctx->base, newSocketIo(ctx->base, acceptSocket), zmtpSocketREP);
    aioZmtpAccept(ctx->serverSocket, afNone, 1000000, aio_rep_zmtpacceptcb, ctx);
  } else {
    ctx->serverRunning = false;
    postQuitOperation(ctx->base);
  }
}

TEST(zmtp, aio_rep)
{
  zmtpContext context(gBase);
  context.listener = startTCPServer(gBase, aio_rep_acceptcb, &context, gPort);
  context.clientRunning = true;
  context.serverRunning = true;
  ASSERT_NE(context.listener, nullptr);
  std::thread clientThread(zmq_server_req, &context, gPort);
  asyncLoop(gBase);
  clientThread.join();
  deleteAioObject(context.listener);
  ASSERT_EQ(context.clientState, 4);
  ASSERT_EQ(context.serverState, 4);
}

void aio_rep_accept_coro(void *arg)
{
  auto ctx = static_cast<zmtpContext*>(arg);
  zmtpSocket *socket = nullptr;
  socketTy fd = ioAccept(ctx->listener, 1000000);
  EXPECT_GT(fd, 0);
  if (fd > 0) {
    socket = zmtpSocketNew(ctx->base, newSocketIo(ctx->base, fd), zmtpSocketREP);
    int acceptResult = ioZmtpAccept(socket, afNone, 3000000);
    EXPECT_EQ(acceptResult, 0);
    if (acceptResult == 0) {
      ssize_t recvResult;
      ssize_t sendResult;
      zmtpUserMsgTy msgType;
      ctx->serverState = 1;

      {
        // Short message
        recvResult = ioZmtpRecv(socket, ctx->stream, 1024, afNone, 1000000, &msgType);
        reqStruct *req = ctx->stream.data<reqStruct>();
        repStruct rep;
        EXPECT_EQ(msgType, zmtpMessage);
        EXPECT_EQ(recvResult, static_cast<ssize_t>(sizeof(reqStruct)));
        EXPECT_EQ(ctx->stream.sizeOf(), sizeof(reqStruct));
        EXPECT_EQ(req->a, 11u);
        EXPECT_EQ(req->b, 77u);
        if (msgType == zmtpMessage &&
            recvResult == sizeof(reqStruct) &&
            ctx->stream.sizeOf() == sizeof(reqStruct) &&
            req->a == 11 &&
            req->b == 77) {
          rep.c = req->a + req->b;
          sendResult = ioZmtpSend(socket, &rep, sizeof(rep), zmtpMessage, afNone, 1000000);
          EXPECT_EQ(sendResult, static_cast<ssize_t>(sizeof(rep)));
          if (sendResult == sizeof(rep))
            ctx->serverState++;
        }
      }

      {
        // Long message
        bool valid = true;
        recvResult = ioZmtpRecv(socket, ctx->stream, 65536, afNone, 1000000, &msgType);
        reqStruct *req = ctx->stream.data<reqStruct>();
        repStruct rep[1024];
        EXPECT_EQ(msgType, zmtpMessage);
        EXPECT_EQ(recvResult, static_cast<ssize_t>(1024*sizeof(reqStruct)));
        EXPECT_EQ(ctx->stream.sizeOf(), 1024*sizeof(reqStruct));
        for (unsigned i = 0; i < 1024; i++) {
          rep[i].c = req[i].a + req[i].b;
          EXPECT_EQ(req[i].a, i);
          EXPECT_EQ(req[i].b, i);
          if (req[i].a != i || req[i].b != i) {
            valid = false;
            break;
          }
        }

        if (msgType == zmtpMessage &&
            recvResult == 1024*sizeof(reqStruct) &&
            ctx->stream.sizeOf() == 1024*sizeof(reqStruct) &&
            valid) {
          sendResult = ioZmtpSend(socket, rep, sizeof(rep), zmtpMessage, afNone, 1000000);
          EXPECT_EQ(sendResult, static_cast<ssize_t>(sizeof(rep)));
          if (sendResult == sizeof(rep))
            ctx->serverState++;
        }
      }

      {
        // Small limit
        recvResult = ioZmtpRecv(socket, ctx->stream, 8, afNone, 1000000, &msgType);
        EXPECT_EQ(recvResult, -aosBufferTooSmall);
        if (recvResult == -aosBufferTooSmall)
          ctx->serverState++;
      }
    }
  }

  ctx->serverRunning = false;
  EXPECT_EQ(waitClient(ctx), true);
  if (socket)
    zmtpSocketDelete(socket);
  postQuitOperation(ctx->base);
}

TEST(zmtp, aio_rep_coro)
{
  zmtpContext context(gBase);
  context.listener = startTCPServer(gBase, nullptr, nullptr, gPort);
  context.clientRunning = true;
  context.serverRunning = true;
  ASSERT_NE(context.listener, nullptr);
  std::thread clientThread(zmq_server_req, &context, gPort);
  coroutineCall(coroutineNew(aio_rep_accept_coro, &context, 0x10000));
  asyncLoop(gBase);
  clientThread.join();
  deleteAioObject(context.listener);
  ASSERT_EQ(context.clientState, 4);
  ASSERT_EQ(context.serverState, 4);
}
