#include "macro.h"
#include "p2putils/CommonParse.h"
#include "p2putils/HttpParse.h"
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <vector>

static StateElement *headerNames = nullptr;

struct Terminal {
  const char *key;
  int value;
};

struct FSMNode {
  std::vector<Terminal> elements;
};

static Terminal httpHeaders[] = {
  {"Content-Length:", hhContentLength},
  {"Content-Type:", hhContentType},
  {"Connection:", hhConnection},
  {"Date:", hhDate},
  {"Server:", hhServer},
  {"Transfer-Encoding:", hhTransferEncoding}
};

constexpr unsigned TABLE_SIZE = 128;

StateElement *buildTable(Terminal *terminals, size_t count)
{
  FSMNode current[TABLE_SIZE];

  for (unsigned i = 0; i < TABLE_SIZE; i++)
    current[i].elements.clear();

  for (size_t i = 0; i < count; i++)
    current[static_cast<unsigned char>(terminals[i].key[0])].elements.push_back(terminals[i]);

  StateElement *table = new StateElement[TABLE_SIZE];
  for (unsigned i = 0; i < TABLE_SIZE; i++) {
    if (current[i].elements.size() == 0) {
      table[i].state = stError;
    } else if (current[i].elements.size() == 1) {
      Terminal singleTerminal = current[i].elements[0];
      if (strlen(singleTerminal.key) == 1) {
        table[i].state = stFinish;
        table[i].token = singleTerminal.value;
      } else {
        table[i].state = stMemcmp;
        table[i].token = singleTerminal.value;
        table[i].terminal = singleTerminal.key+1;
        table[i].intValue = static_cast<int>(strlen(singleTerminal.key)-1);
      }
    } else {
      // build subtable
      std::vector<Terminal> &subTerminals = current[i].elements;
      for (size_t tIdx = 0; tIdx < current[i].elements.size(); tIdx++) {
        subTerminals[tIdx].key = subTerminals[tIdx].key + 1;
      }

      StateElement *newTable = buildTable(&subTerminals[0], current[i].elements.size());
      table[i].state = stSwitchTable;
      table[i].element = newTable;
    }

  }

  return table;
}

HttpParserResultTy simpleTableParse(const char **ptr, const char *end, StateElement *entry, int *token)
{
  const char *p = *ptr;
  StateElement *currentState = entry;
  while (p != end) {
    StateElement &current = currentState[static_cast<unsigned char>(*p)];
    switch (current.state) {
      case stFinish :
        *token = current.token;
        *ptr = p;
        return httpResultOk;
      case stError:
        return httpResultError;
      case stMemcmp:
        *token = current.token;
        if (memcmp(p+1, current.terminal, static_cast<size_t>(current.intValue)) == 0) {
          *ptr = p+1+current.intValue;
          return httpResultOk;
        } else {
          return httpResultError;
        }
      case stSwitchTable:
        currentState = current.element;
        break;
    }
      
    p++;
  }
  
  return httpResultNeedMoreData;
}

static int isDigit(char s)
{
  return (s >= '0' && s <= '9');
}

static inline int canRead(const char *ptr, const char *end, size_t size)
{
  return size <= static_cast<size_t>(end-ptr);
}

static HttpParserResultTy compareUnchecked(const char **ptr, const char *end, const char *substr, size_t size)
{
  __UNUSED(end);
  if (memcmp(*ptr, substr, size) == 0) {
    (*ptr) += size;
    return httpResultOk;
  } else {
    return httpResultError;
  }
}

static HttpParserResultTy skipSPCharacters(const char **ptr, const char *end)
{
  if (*ptr < end) {
    if (**ptr != ' ')
      return httpResultError;
    while (*ptr < end && **ptr == ' ')
      (*ptr)++;
    return httpResultOk;
  } else {
   return httpResultNeedMoreData; 
  }
}

static HttpParserResultTy readUntilCRLF(const char **ptr, const char *end)
{
  if (*ptr >= end-2)
    return httpResultNeedMoreData;
  
  while (*ptr < end-1) {
    if (**ptr == 0x0D && *(*ptr + 1) == 0x0A)
      break;
    (*ptr)++;
  }
  
  (*ptr) += 2;
  return httpResultOk;
}

static inline HttpParserResultTy readDec(const char **ptr, const char *end, size_t *size)
{
  *size = 0;
  const char *p = *ptr;
  while (p < end-2) {
    if (*p == 0x0D && *(p+1) == 0x0A) {
      *ptr = p+2;
      return httpResultOk;
    } else if (*p >= '0' && *p <= '9') {
      *size *= 10;
      *size += static_cast<unsigned char>(*p-'0');
    } else {
      return httpResultError;
    }
    
    p++;
  }
  
  return httpResultNeedMoreData;
}

static inline HttpParserResultTy readHex(const char **ptr, const char *end, size_t *size)
{
  *size = 0;
  const char *p = *ptr;
  while (p < end-2) {
    if (*p == 0x0D && *(p+1) == 0x0A) {
      *ptr = p+2;
      return httpResultOk;
    } else if (*p >= '0' && *p <= '9') {
      *size <<= 4; *size += static_cast<unsigned char>(*p-'0');
    } else if (*p >= 'A' && *p <= 'F') {
      *size <<= 4; *size += static_cast<unsigned char>(*p-'A'+10);
    } else if (*p >= 'a' && *p <= 'f') {
      *size <<= 4; *size += static_cast<unsigned char>(*p-'a'+10);
    } else {
      return httpResultError;
    }
    
    p++;
  }
  
  return httpResultNeedMoreData;
}

static HttpParserResultTy httpParseStartLine(HttpParserState *state, httpParseCb callback, void *arg)
{
  const char startLine[] = "HTTP/";
  
  HttpParserResultTy result;
  HttpComponent component;
  const char *ptr = state->ptr;
  if (!canRead(ptr, state->end, sizeof(startLine)-1 + 3))
    return httpResultNeedMoreData;
  
  // HTTP Protocol version
  if ( (result = compareUnchecked(&ptr, state->end, startLine, sizeof(startLine)-1)) != httpResultOk )
    return result;
  if ( !(isDigit(ptr[0]) && ptr[1] == '.' && isDigit(ptr[2])) )
    return httpResultError;
  component.startLine.majorVersion = static_cast<unsigned char>(ptr[0]-'0');
  component.startLine.minorVersion = static_cast<unsigned char>(ptr[2]-'0');
  ptr += 3;
  
  // HTTP response code
  skipSPCharacters(&ptr, state->end);
  if (!canRead(ptr, state->end, 3))
    return httpResultNeedMoreData;
  component.startLine.code =
      100*static_cast<unsigned char>(ptr[0]-'0') +
      10*static_cast<unsigned char>(ptr[1]-'0') +
      static_cast<unsigned char>(ptr[2]-'0');
  ptr += 3;
  
  // associated textual phrase
  skipSPCharacters(&ptr, state->end);
  component.startLine.description.data = ptr;
  if ( ( result = readUntilCRLF(&ptr, state->end)) != httpResultOk )
    return result;
  component.startLine.description.size = static_cast<size_t>(ptr-component.startLine.description.data-2);
  component.type = httpDtStartLine;
  callback(&component, arg);
  
  state->ptr = ptr;
  return httpResultOk;
}

void httpInit(HttpParserState *state)
{
  state->state = httpStStartLine;
  state->chunked = false;
  state->dataRemaining = 0;
  state->firstFragment = true;
  if (!headerNames)
    headerNames = buildTable(httpHeaders, sizeof(httpHeaders)/sizeof(Terminal));
}

void httpSetBuffer(HttpParserState *state, const void *buffer, size_t size)
{
  state->ptr = state->buffer = static_cast<const char*>(buffer);
  state->end = state->buffer + size;
}

HttpParserResultTy httpParse(HttpParserState *state, httpParseCb callback, void *arg)
{
  HttpParserResultTy result;
  HttpComponent component;

  if (state->state == httpStStartLine) {
    if ( (result = httpParseStartLine(state, callback, arg)) != httpResultOk)
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
          const char *p = state->ptr;
          HttpParserResultTy result = simpleTableParse(&p, state->end, headerNames, &token);
          if (result == httpResultOk) {
            component.header.entryType = token;
            skipSPCharacters(&p, state->end);

            switch (token) {
              case hhContentLength : {
                size_t contentSize;
                if ( ( result = readDec(&p, state->end, &contentSize)) != httpResultOk )
                  return result;
                component.header.sizeValue = contentSize;
                state->dataRemaining = contentSize;
                callback(&component, arg);
                break;
              }

              case hhTransferEncoding : {
                component.header.stringValue.data = p;
                if ( ( result = readUntilCRLF(&p, state->end)) != httpResultOk )
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
                if ( ( result = readUntilCRLF(&p, state->end)) != httpResultOk )
                  return result;
                component.header.stringValue.size = static_cast<size_t>(p-component.header.stringValue.data-2);
                callback(&component, arg);
                break;
              }
            }

            state->ptr = p;
          } else if (result == httpResultNeedMoreData) {
            return result;
          } else if (result == httpResultError) {
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
            if ( ( result = readUntilCRLF(&p, state->end)) != httpResultOk )
              return result;
            component.header.stringValue.size = static_cast<size_t>(p-component.header.stringValue.data-2);
            callback(&component, arg);

            state->ptr = p;
          }
        }
      }
    } else {
      return httpResultNeedMoreData;
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
            return httpResultNeedMoreData;
        } else {
          // we at begin of next chunk
          if (!state->firstFragment) {
            // skip CRLF for non-first chunk
            if (!canRead(p, state->end, 2))
              return httpResultNeedMoreData;
            p += 2;
          }

          size_t chunkSize;
          if ( ( result = readHex(&p, state->end, &chunkSize)) != httpResultOk )
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
      } else if (p != state->end) {
        size_t size = std::min(state->dataRemaining, static_cast<size_t>(state->end - p));
        component.type = httpDtDataFragment;
        component.data.data = p;
        component.data.size = size;
        callback(&component, arg);
        state->ptr = p + size;
        state->firstFragment = false;
        state->dataRemaining -= size;
        return httpResultNeedMoreData;
      }
    }
  }

  return httpResultOk;
}

const void *httpDataPtr(HttpParserState *state)
{
  return state->ptr;
}

size_t httpDataRemaining(HttpParserState *state)
{
  return static_cast<size_t>(state->end - state->ptr);
}
