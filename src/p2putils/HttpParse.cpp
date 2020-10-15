#include "macro.h"
#include "p2putils/HttpParse.h"
#include "LiteFlatHashTable.h"
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <vector>

static LiteFlatHashTable *headerNames = nullptr;

static Terminal httpHeaders[] = {
  {"Content-Length", hhContentLength},
  {"Content-Type", hhContentType},
  {"Connection", hhConnection},
  {"Date", hhDate},
  {"Server", hhServer},
  {"Transfer-Encoding", hhTransferEncoding}
};

static int isDigit(char s)
{
  return (s >= '0' && s <= '9');
}

static inline int canRead(const char *ptr, const char *end, size_t size)
{
  return size <= static_cast<size_t>(end-ptr);
}

static ParserResultTy compareUnchecked(const char **ptr, const char *end, const char *substr, size_t size)
{
  __UNUSED(end);
  if (memcmp(*ptr, substr, size) == 0) {
    (*ptr) += size;
    return ParserResultOk;
  } else {
    return ParserResultError;
  }
}

static ParserResultTy skipSPCharacters(const char **ptr, const char *end)
{
  if (*ptr < end) {
    if (**ptr != ' ')
      return ParserResultError;
    while (*ptr < end && **ptr == ' ')
      (*ptr)++;
    return ParserResultOk;
  } else {
   return ParserResultNeedMoreData;
  }
}

static ParserResultTy readUntilCRLF(const char **ptr, const char *end)
{
  if (*ptr >= end-2)
    return ParserResultNeedMoreData;
  
  while (*ptr < end-1) {
    if (**ptr == 0x0D && *(*ptr + 1) == 0x0A)
      break;
    (*ptr)++;
  }
  
  (*ptr) += 2;
  return ParserResultOk;
}

static inline ParserResultTy readDec(const char **ptr, const char *end, size_t *size)
{
  *size = 0;
  const char *p = *ptr;
  while (p < end-2) {
    if (*p == 0x0D && *(p+1) == 0x0A) {
      *ptr = p+2;
      return ParserResultOk;
    } else if (*p >= '0' && *p <= '9') {
      *size *= 10;
      *size += static_cast<unsigned char>(*p-'0');
    } else {
      return ParserResultError;
    }
    
    p++;
  }
  
  return ParserResultNeedMoreData;
}

static inline ParserResultTy readHex(const char **ptr, const char *end, size_t *size)
{
  *size = 0;
  const char *p = *ptr;
  while (p < end-2) {
    if (*p == 0x0D && *(p+1) == 0x0A) {
      *ptr = p+2;
      return ParserResultOk;
    } else if (*p >= '0' && *p <= '9') {
      *size <<= 4; *size += static_cast<unsigned char>(*p-'0');
    } else if (*p >= 'A' && *p <= 'F') {
      *size <<= 4; *size += static_cast<unsigned char>(*p-'A'+10);
    } else if (*p >= 'a' && *p <= 'f') {
      *size <<= 4; *size += static_cast<unsigned char>(*p-'a'+10);
    } else {
      return ParserResultError;
    }
    
    p++;
  }
  
  return ParserResultNeedMoreData;
}

static ParserResultTy httpParseStartLine(HttpParserState *state, httpParseCb callback, void *arg)
{
  const char startLine[] = "HTTP/";
  
  ParserResultTy result;
  HttpComponent component;
  const char *ptr = state->ptr;
  if (!canRead(ptr, state->end, sizeof(startLine)-1 + 3))
    return ParserResultNeedMoreData;
  
  // HTTP Protocol version
  if ( (result = compareUnchecked(&ptr, state->end, startLine, sizeof(startLine)-1)) != ParserResultOk )
    return result;
  if ( !(isDigit(ptr[0]) && ptr[1] == '.' && isDigit(ptr[2])) )
    return ParserResultError;
  component.startLine.majorVersion = static_cast<unsigned char>(ptr[0]-'0');
  component.startLine.minorVersion = static_cast<unsigned char>(ptr[2]-'0');
  ptr += 3;
  
  // HTTP response code
  skipSPCharacters(&ptr, state->end);
  if (!canRead(ptr, state->end, 3))
    return ParserResultNeedMoreData;
  component.startLine.code =
      100*static_cast<unsigned char>(ptr[0]-'0') +
      10*static_cast<unsigned char>(ptr[1]-'0') +
      static_cast<unsigned char>(ptr[2]-'0');
  ptr += 3;
  
  // associated textual phrase
  skipSPCharacters(&ptr, state->end);
  component.startLine.description.data = ptr;
  if ( ( result = readUntilCRLF(&ptr, state->end)) != ParserResultOk )
    return result;
  component.startLine.description.size = static_cast<size_t>(ptr-component.startLine.description.data-2);
  component.type = httpDtStartLine;
  callback(&component, arg);
  
  state->ptr = ptr;
  return ParserResultOk;
}

void httpInit(HttpParserState *state)
{
  state->state = httpStStartLine;
  state->chunked = false;
  state->dataRemaining = 0;
  state->firstFragment = true;
  if (!headerNames)
    headerNames = buildLiteFlatHashTable(httpHeaders, sizeof(httpHeaders)/sizeof(Terminal));
}

void httpSetBuffer(HttpParserState *state, const void *buffer, size_t size)
{
  state->ptr = state->buffer = static_cast<const char*>(buffer);
  state->end = state->buffer + size;
}

ParserResultTy httpParse(HttpParserState *state, httpParseCb callback, void *arg)
{
  ParserResultTy result;
  HttpComponent component;

  if (state->state == httpStStartLine) {
    if ( (result = httpParseStartLine(state, callback, arg)) != ParserResultOk)
      return result;
    state->state = httpStHeader;
  }

  if (state->state == httpStHeader) {
    if (canRead(state->ptr, state->end, 2)) {
      component.type = httpDtHeaderEntry;
      for (;;) {
        if (state->ptr[0] == '\r' && state->ptr[1] == '\n') {
          state->ptr += 2;
          state->state = httpStBody;
          break;
        } else {
          int token;
          component.header.entryName.data = state->ptr;
          const char *p = state->ptr;
          char colon = ':';
          ParserResultTy result = searchLiteFlatHashTable(&p, state->end, &colon, 1, headerNames, &token);
          if (result == ParserResultOk) {
            component.header.entryName.size = p - component.header.entryName.data;
            component.header.entryType = token;
            ++p;
            skipSPCharacters(&p, state->end);

            switch (token) {
              case hhContentLength : {
                size_t contentSize;
                if ( ( result = readDec(&p, state->end, &contentSize)) != ParserResultOk )
                  return result;
                component.header.sizeValue = contentSize;
                state->dataRemaining = contentSize;
                callback(&component, arg);
                break;
              }

              case hhTransferEncoding : {
                component.header.stringValue.data = p;
                if ( ( result = readUntilCRLF(&p, state->end)) != ParserResultOk )
                  return result;
                component.header.stringValue.size = static_cast<size_t>(p-component.header.stringValue.data-2);
                callback(&component, arg);

                state->chunked = (component.header.stringValue.size == 7) &&
                                 (memcmp(component.header.stringValue.data, "chunked", 7) == 0);
                break;
              }

              case hhContentType :
              case hhConnection :
              case hhDate :
              case hhServer : {
                component.header.stringValue.data = p;
                if ( ( result = readUntilCRLF(&p, state->end)) != ParserResultOk )
                  return result;
                component.header.stringValue.size = static_cast<size_t>(p-component.header.stringValue.data-2);
                callback(&component, arg);
                break;
              }

              default : {
                component.header.stringValue.data = p;
                if ( ( result = readUntilCRLF(&p, state->end)) != ParserResultOk )
                  return result;
                component.header.stringValue.size = static_cast<size_t>(p-component.header.stringValue.data-2);
                callback(&component, arg);
                break;
              }
            }

            state->ptr = p;
          } else if (result == ParserResultNeedMoreData) {
            return result;
          } else if (result == ParserResultError) {
            component.header.entryType = 0;
            component.header.entryName.data = p;
            const char *entryNameEnd = nullptr;
            while (p < state->end) {
              if (*p == ':') {
                entryNameEnd = p;
                p++;
                break;
              }
              p++;
            }

            component.header.entryName.size = static_cast<size_t>(entryNameEnd-component.header.entryName.data);
            skipSPCharacters(&p, state->end);
            component.header.stringValue.data = p;
            if ( ( result = readUntilCRLF(&p, state->end)) != ParserResultOk )
              return result;
            component.header.stringValue.size = static_cast<size_t>(p-component.header.stringValue.data-2);
            callback(&component, arg);

            state->ptr = p;
          }
        }
      }
    } else {
      return ParserResultNeedMoreData;
    }
  }

  if (state->state == httpStBody) {
    if (state->chunked) {
      const char *readyChunk = nullptr;
      size_t readyChunkSize = 0;
      const char *p = state->ptr;

      for (;;) {
        if (state->dataRemaining) {
          bool needMoreData = false;
          // need read /httpDataRemaining/ bytes
          if (canRead(p, state->end, state->dataRemaining)) {
            readyChunk = p;
            readyChunkSize = state->dataRemaining;
            p += state->dataRemaining;
            state->dataRemaining = 0;
            state->firstFragment = false;
          } else {
            readyChunk = p;
            readyChunkSize = std::min(state->dataRemaining, static_cast<size_t>(state->end - p));
            p += readyChunkSize;
            state->dataRemaining -= readyChunkSize;
            needMoreData = true;
          }

          if (readyChunkSize) {
            component.type = httpDtDataFragment;
            component.data.data = readyChunk;
            component.data.size = readyChunkSize;
            callback(&component, arg);
            state->ptr = p;
          }

          if (needMoreData)
            return ParserResultNeedMoreData;
        } else {
          // we at begin of next chunk
          if (!state->firstFragment) {
            // skip CRLF for non-first chunk
            if (!canRead(p, state->end, 2))
              return ParserResultNeedMoreData;
            p += 2;
          }

          size_t chunkSize;
          if ( ( result = readHex(&p, state->end, &chunkSize)) != ParserResultOk )
            return result;

          if (chunkSize == 0) {
            component.type = httpDtData;
            component.data.data = p;
            component.data.size = 0;
            callback(&component, arg);
            state->state = httpStLast;
            state->ptr = p+2;
            break;
          } else {
            state->dataRemaining = chunkSize;
          }
        }
      }
    } else {
      const char *p = state->ptr;
      if (canRead(p, state->end, state->dataRemaining)) {
        component.type = state->firstFragment ? httpDtData : httpDtDataFragment;
        component.data.data = p;
        component.data.size = state->dataRemaining;
        callback(&component, arg);
        state->ptr = p + state->dataRemaining;
        state->state = httpStLast;
      } else {
        if (p != state->end) {
          size_t size = std::min(state->dataRemaining, static_cast<size_t>(state->end - p));
          component.type = httpDtDataFragment;
          component.data.data = p;
          component.data.size = size;
          callback(&component, arg);
          state->ptr = p + size;
          state->firstFragment = false;
          state->dataRemaining -= size;
        }
        return ParserResultNeedMoreData;
      }
    }
  }

  return ParserResultOk;
}

const void *httpDataPtr(HttpParserState *state)
{
  return state->ptr;
}

size_t httpDataRemaining(HttpParserState *state)
{
  return static_cast<size_t>(state->end - state->ptr);
}
