#include "asyncio/api.h"
#include "asyncio/asyncio.h"
#include "p2pformat.h"

class xmstream;
class p2pNode;


struct p2pConnection;
struct p2pInfo;

enum p2pStatusTy {
  // p2p status
  p2pStFormatError = aosLast,
  p2pStAuthFailed,
  p2pStAppNotFound
};

static inline AsyncOpStatus p2pMakeStatus(p2pStatusTy status) {
  return static_cast<AsyncOpStatus>(status);
}

typedef p2pErrorTy p2pAcceptCb(AsyncOpStatus, p2pConnection*, p2pConnectData*, void*);
typedef void p2pConnectCb(AsyncOpStatus, p2pConnection*, void*);
typedef void p2preadCb(AsyncOpStatus, p2pConnection*, p2pHeader, void*, void*);
typedef void p2preadStreamCb(AsyncOpStatus, p2pConnection*, p2pHeader, p2pStream*, void*);
typedef void p2pwriteCb(AsyncOpStatus, p2pConnection*, p2pHeader, void*);

struct p2pOp {
  asyncOpRoot root;
  AsyncOpStatus lastError;
  int state;
  int rwState;
  union {
    void *buffer;
    p2pStream *stream;
  };
  size_t bufferSize;

  HostAddress address;
  p2pHeader header;
  p2pConnectData connectMsg;
};

struct p2pConnection {
  aioObjectRoot root;
  aioObject *socket;
  p2pStream stream;
};

p2pConnection *p2pConnectionNew(aioObject *socket);
void p2pConnectionDelete(p2pConnection *connection);

void aiop2pAccept(p2pConnection *connection,
                  uint64_t timeout,
                  p2pAcceptCb *callback,
                  void *arg);

void aiop2pConnect(p2pConnection *connection,
                   const HostAddress *address,
                   p2pConnectData *data,
                   uint64_t timeout,
                   p2pConnectCb *callback,
                   void *arg);
  
void aiop2pRecvStream(p2pConnection *connection,
                      p2pStream &stream,
                      size_t maxMsgSize,
                      AsyncFlags flags,
                      uint64_t timeout,
                      p2preadStreamCb *callback,
                      void *arg);

void aiop2pRecv(p2pConnection *connection,
                void *buffer,
                uint32_t bufferSize,
                AsyncFlags flags,
                uint64_t timeout,
                p2preadCb *callback,
                void *arg);

void aiop2pSend(p2pConnection *connection,
                const void *data,
                uint32_t id,
                uint32_t type,
                uint32_t size,
                AsyncFlags flags,
                uint64_t timeout,
                p2pwriteCb *callback,
                void *arg);

int iop2pAccept(p2pConnection *connection, uint64_t timeout, p2pAcceptCb *callback, void *arg);
int iop2pConnect(p2pConnection *connection, const HostAddress *address, uint64_t timeout, p2pConnectData *data);
ssize_t iop2pSend(p2pConnection *connection, const void *data, uint32_t id, uint32_t type, uint32_t size, AsyncFlags flags, uint64_t timeout);
ssize_t iop2pRecvStream(p2pConnection *connection, p2pStream &stream, uint32_t maxMsgSize, AsyncFlags flags, uint64_t timeout, p2pHeader *header);
ssize_t iop2pRecv(p2pConnection *connection, void *buffer, uint32_t bufferSize, AsyncFlags flags, uint64_t timeout, p2pHeader *header);

