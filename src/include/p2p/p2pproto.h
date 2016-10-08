#include "asyncio/asyncio.h"
#include "p2pformat.h"

class xmstream;
class p2pNode;


struct p2pConnection;
struct p2pInfo;

enum p2pStatusTy {
  // p2p status
  p2pStFormatError = aosLast,
  p2pStBufferTooSmall,
  p2pStAuthFailed,
  p2pStAppNotFound
};


typedef p2pErrorTy p2pAcceptCb(int, asyncBase*, p2pConnection*, p2pConnectData*, void*);
typedef void p2pConnectCb(int, asyncBase*, p2pConnection*, void*);
typedef void p2pCb(int, asyncBase*, p2pConnection*, p2pHeader, void*);

struct p2pOp {
  asyncOpRoot root;
  
  int lastError;
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

void aiop2pAccept(asyncBase *base, 
                  p2pConnection *connection,
                  uint64_t timeout,
                  p2pAcceptCb *callback,
                  void *arg);

void aiop2pConnect(asyncBase *base,
                   p2pConnection *connection,
                   uint64_t timeout,
                   p2pConnectData *data,
                   p2pConnectCb *callback,
                   void *arg);
  
void aiop2pRecv(asyncBase *base, 
                p2pConnection *connection,
                uint64_t timeout,
                p2pStream *stream,
                size_t maxMsgSize,
                p2pCb *callback,
                void *arg);

void aiop2pRecv(asyncBase *base, 
                p2pConnection *connection,
                uint64_t timeout,
                void *buffer,
                size_t bufferSize,
                p2pCb *callback,
                void *arg);

void aiop2pSend(asyncBase *base, 
                p2pConnection *connection,
                uint64_t timeout,
                void *data,
                p2pHeader header,
                p2pCb *callback,
                void *arg);

int iop2pAccept(asyncBase *base, p2pConnection *connection, uint64_t timeout, p2pAcceptCb *callback, void *arg);
int iop2pConnect(asyncBase *base, p2pConnection *connection, uint64_t timeout, p2pConnectData *data);
bool iop2pSend(asyncBase *base, p2pConnection *connection, uint64_t timeout, void *data, uint32_t id, uint32_t type, size_t size);
ssize_t iop2pRecv(asyncBase *base, p2pConnection *connection, uint64_t timeout, void *buffer, size_t bufferSize, p2pHeader *header);
ssize_t iop2pRecv(asyncBase *base, p2pConnection *connection, uint64_t timeout, p2pStream *stream, size_t maxMsgSize, p2pHeader *header);
