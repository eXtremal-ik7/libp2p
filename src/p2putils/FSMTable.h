#ifndef __FSMTABLE_H_
#define __FSMTABLE_H_
 
#include "p2putils/CommonParse.h"
#include <vector>

static constexpr unsigned TABLE_SIZE = 128;
 
struct StateElement {
  StateTy state;
  int token;
  union {
    struct StateElement *element;
    const char *terminal;
  };
  int intValue;
};

struct Terminal {
  const char *key;
  int value;
};

struct FSMNode {
  std::vector<Terminal> elements;
};

StateElement *buildTable(Terminal *terminals, size_t count);
ParserResultTy simpleTableParse(const char **ptr, const char *end, StateElement *entry, int *token);

#endif //__FSMTABLE_H_
