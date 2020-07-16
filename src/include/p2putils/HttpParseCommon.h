#ifndef __LIBP2P_HTTPPARSECOMMON_H_
#define __LIBP2P_HTTPPARSECOMMON_H_

#ifdef __cplusplus
extern "C" {
#endif

enum {
  hhAccept = 1,
  hhConnection,
  hhContentLength,
  hhContentType,
  hhDate,
  hhHost,
  hhServer,
  hhTransferEncoding,
  hhUserAgent
};

enum {
  hmUnknown = 0,
  hmGet,
  hmHead,
  hmPost,
  hmPut,
  hmDelete,
  hmConnect,
  hmOptions,
  hmTrace,
  hmPatch
};

#ifdef __cplusplus
}
#endif

#endif //__LIBP2P_HTTPPARSECOMMON_H_
