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
typedef void p2pCb(AsyncOpStatus, p2pConnection*, p2pHeader, void*);

struct p2pOp {
  asyncOpRoot root;
  AsyncOpStatus lastError;
  int state;  
  union {
    void *buffer;
    p2pStream *stream;
  };
  size_t bufferSize;

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
                   uint64_t timeout,
                   p2pConnectData *data,
                   p2pConnectCb *callback,
                   void *arg);
  
void aiop2pRecv(p2pConnection *connection,
                uint64_t timeout,
                p2pStream *stream,
                size_t maxMsgSize,
                p2pCb *callback,
                void *arg);

void aiop2pRecv(p2pConnection *connection,
                uint64_t timeout,
                void *buffer,
                size_t bufferSize,
                p2pCb *callback,
                void *arg);

void aiop2pSend(p2pConnection *connection,
                uint64_t timeout,
                void *data,
                p2pHeader header,
                p2pCb *callback,
                void *arg);

int iop2pAccept(p2pConnection *connection, uint64_t timeout, p2pAcceptCb *callback, void *arg);
int iop2pConnect(p2pConnection *connection, uint64_t timeout, p2pConnectData *data);
bool iop2pSend(p2pConnection *connection, uint64_t timeout, void *data, uint32_t id, uint32_t type, size_t size);
ssize_t iop2pRecv(p2pConnection *connection, uint64_t timeout, void *buffer, size_t bufferSize, p2pHeader *header);
ssize_t iop2pRecv(p2pConnection *connection, uint64_t timeout, p2pStream *stream, size_t maxMsgSize, p2pHeader *header);
