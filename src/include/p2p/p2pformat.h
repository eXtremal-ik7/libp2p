#ifndef __P2PFORMAT_H_
#define __P2PFORMAT_H_

#include "p2putils/coreTypes.h"
#include "p2putils/xmstream.h"

enum p2pErrorTy {
  p2pOk = 0,
  p2pErrorAuthFailed,
  p2pErrorAppNotFound
};

enum p2pMsgTy {
  p2pMsgStatus = 0,
  p2pMsgConnect,
  p2pMsgRequest,
  p2pMsgResponse,
  p2pMsgSignal
};

#pragma pack(push, 1)
struct p2pHeader {
  uint32_t id;
  uint32_t type;
  uint64_t size;
  p2pHeader() {}
  p2pHeader(uint32_t typeArg, uint64_t sizeArg) : id(0), type(typeArg), size(sizeArg) {}
  p2pHeader(uint32_t idArg, uint32_t typeArg, uint64_t sizeArg) : id(idArg), type(typeArg), size(sizeArg) {}
};
#pragma pack(pop)

struct p2pConnectData {
  const char *login;
  const char *password;
  const char *application;
};

class p2pStream : public xmstream {
private:
  const char *jumpOverString();
  void writeString(const char *s);
  
public:
  p2pStream(void *data, size_t size) : xmstream(data, size) {}
  p2pStream(size_t size = 64) : xmstream(size) {}
  
  bool readConnectMessage(p2pConnectData *data);
  bool readStatusMessage(p2pErrorTy *error);

  void writeStatusMessage(p2pErrorTy error);  
  void writeConnectMessage(p2pConnectData data);
};

#endif //__P2PFORMAT_H_
