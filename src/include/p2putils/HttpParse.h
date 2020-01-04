#ifdef __cplusplus
extern "C" {
#endif 

#ifndef __LIBP2P_HTTPPARSE_H_
#define __LIBP2P_HTTPPARSE_H_

#include <stddef.h>
#include <stdint.h>

typedef enum StateTy {
  stFinish = 0,
  stMemcmp,
  stError,
  stSwitchTable
} StateTy;

typedef struct StateElement {
  StateTy state;
  int token;
  union {
    struct StateElement *element;
    const char *terminal;
  };
  int intValue;
} StateElement;

enum {
  hhContentLength = 1,
  hhContentType,
  hhConnection,
  hhDate,
  hhServer,
  hhTransferEncoding
};

typedef enum HttpParserStateTy {
  httpStStartLine = 0,
  httpStHeader,  
  httpStBody,
  httpStLast
} HttpParserStateTy;

typedef enum HttpParserResultTy {
  httpResultOk = 0,
  httpResultNeedMoreData,
  httpResultError
} HttpParserResultTy;

typedef enum HttpParserDataTy {
  httpDtInitialize = 0,
  httpDtStartLine,
  httpDtHeaderEntry,
  httpDtData,
  httpDtDataFragment,
  httpDtFinalize
} HttpParserDataTy;

typedef struct HttpParserState {
  HttpParserStateTy state;
  const char *buffer;
  const char *ptr;
  const char *end;
  int chunked;
  int firstFragment;
  size_t dataRemaining;
} HttpParserState;

typedef struct Raw {
  const char *data;
  size_t size;
} Raw;

typedef struct HttpComponent {
  int type;
  union {
    // Start line
    struct {
      unsigned majorVersion;
      unsigned minorVersion;
      unsigned code;
      Raw description;
    } startLine;
    
    // Header
    struct {
      int entryType;
      Raw entryName;
      union {
        Raw stringValue;
        size_t sizeValue;
      };
    } header;
    
    Raw data;
  };
} HttpComponent;

typedef void httpParseCb(HttpComponent *component, void *arg);

void httpInit(HttpParserState *state);
void httpSetBuffer(HttpParserState *state, const void *buffer, size_t size);
HttpParserResultTy httpParse(HttpParserState *state, httpParseCb callback, void *arg);

const void *httpDataPtr(HttpParserState *state);
size_t httpDataRemaining(HttpParserState *state);

#endif //__LIBP2P_HTTPPARSE_H_

#ifdef __cplusplus
}
#endif
