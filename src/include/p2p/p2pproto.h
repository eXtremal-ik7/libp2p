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

typedef p2pErrorTy p2pAcceptCb(p2pConnectData*, p2pInfo*);
typedef void p2pCb(p2pInfo*);

struct p2pInfo {
  p2pConnection *connection;
  int status;
  void *arg;
  union {
    void *buffer;
    p2pStream *stream;
  };
  size_t bufferSize;
  p2pHeader header;
};

struct p2pOp {
  p2pInfo info;
  int type;
  void *callback;
  uint64_t usTimeout;
  int state;  

  p2pOp *next;
  
  // data fields
  p2pConnectData connectMsg;
};

struct p2pConnection {
  aioObject *socket;
  p2pStream stream;
  p2pOp *current;
};

p2pConnection *p2pConnectionNew(aioObject *socket);
void p2pConnectionDelete(p2pConnection *connection);

void aiop2pAccept(p2pConnection *connection,
                  uint64_t timeout,
                  p2pAcceptCb callback,
                  void *arg);

void aiop2pConnect(p2pConnection *connection,
                   uint64_t timeout,
                   p2pConnectData *data,
                   p2pCb callback,
                   void *arg);
  
void aiop2pRecv(p2pConnection *connection,
                uint64_t timeout,
                p2pStream *stream,
                size_t maxMsgSize,
                p2pCb callback,
                void *arg);

void aiop2pRecv(p2pConnection *connection,
                uint64_t timeout,
                void *buffer,
                size_t bufferSize,
                p2pCb callback,
                void *arg);

void aiop2pSend(p2pConnection *connection,
                uint64_t timeout,
                void *data,
                p2pHeader header,
                p2pCb callback,
                void *arg);

int iop2pAccept(p2pConnection *connection, uint64_t timeout, p2pAcceptCb callback, void *arg);
int iop2pConnect(p2pConnection *connection, uint64_t timeout, p2pConnectData *data);
bool iop2pSend(p2pConnection *connection, uint64_t timeout, void *data, uint32_t id, uint32_t type, size_t size);
ssize_t iop2pRecv(p2pConnection *connection, uint64_t timeout, void *buffer, size_t bufferSize, p2pHeader *header);
ssize_t iop2pRecv(p2pConnection *connection, uint64_t timeout, p2pStream *stream, size_t maxMsgSize, p2pHeader *header);
