#include <asyncio/asyncio.h>
#include <p2p/p2p.h>
#include <gtest/gtest.h>
#include "macro.h"

constexpr unsigned gPort = 65333;
extern asyncBase *gBase;

__NO_PADDING_BEGIN
struct TestContext {
  aioObject *serverSocket;
  aioObject *clientSocket;
  uint8_t clientBuffer[128];
  uint8_t serverBuffer[128];
  p2pStream serverStream;
  asyncBase *base;
  int state;
  int state2;
  bool success;
  TestContext(asyncBase *baseArg) : base(baseArg), state(0), state2(0), success(false) {}
};
__NO_PADDING_END


aioObject *startTCPServer(asyncBase *base, aioAcceptCb callback, void *arg, uint16_t port);
aioObject *startUDPServer(asyncBase *base, aioReadMsgCb callback, void *arg, void *buffer, size_t size, uint16_t port);
aioObject *initializeTCPClient(asyncBase *base, aioConnectCb callback, void *arg, uint16_t port);
aioObject *initializeUDPClient(asyncBase *base);
