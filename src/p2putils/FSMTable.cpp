#include "FSMTable.h"
#include "p2putils/CommonParse.h"
#include <stddef.h>
#include <string.h>
#include <vector>

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

ParserResultTy simpleTableParse(const char **ptr, const char *end, StateElement *entry, int *token)
{
  const char *p = *ptr;
  StateElement *currentState = entry;
  while (p != end) {
    StateElement &current = currentState[static_cast<unsigned char>(*p)];
    switch (current.state) {
      case stFinish :
        *token = current.token;
        *ptr = p;
        return ParserResultOk;
      case stError:
        return ParserResultError;
      case stMemcmp:
        *token = current.token;
        if (memcmp(p+1, current.terminal, static_cast<size_t>(current.intValue)) == 0) {
          *ptr = p+1+current.intValue;
          return ParserResultOk;
        } else {
          return ParserResultError;
        }
      case stSwitchTable:
        currentState = current.element;
        break;
    }

    p++;
  }

  return ParserResultNeedMoreData;
}
