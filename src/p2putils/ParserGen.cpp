#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "p2putils/CommonParse.h"

const int tableSize = 128;
struct State;

struct StateLink {
  State *table;
  int linkIndex;
};

struct State {
  StateTy state;
  const char *terminalName;
  union {
    struct {
      const char *data;
      size_t size;
    } memcmpData;
    StateLink link;
  };
};

struct Terminal {
  const char *value;
  const char *constant;
};

struct FSMNode {
  std::vector<Terminal> elements;
};

void buildSimpleTableParser(Terminal *terminals,
                            size_t count,
                            const std::string &parserName,
                            const std::string &headerName,
                            std::string &headerOut,
                            std::string &sourceOut);

State *buildTable(Terminal *terminals, size_t count, int *nameCounter)
{
  FSMNode current[tableSize];

  for (int i = 0; i < tableSize; i++)
    current[i].elements.clear();
  
  for (size_t i = 0; i < count; i++)
    current[(int)terminals[i].value[0]].elements.push_back(terminals[i]);

  State *table = new State[tableSize];
  for (int i = 0; i < tableSize; i++) {
    if (current[i].elements.size() == 0) {
      table[i].state = stError;
    } else if (current[i].elements.size() == 1) {
      Terminal singleTerminal = current[i].elements[0];
      if (strlen(singleTerminal.value) == 1) {
        table[i].state = stFinish;
        table[i].terminalName = singleTerminal.constant;
      } else {
        table[i].state = stMemcmp;
        table[i].memcmpData.data = singleTerminal.value+1;
        table[i].memcmpData.size = strlen(singleTerminal.value)-1;
        table[i].terminalName = singleTerminal.constant;
      }
    } else {
      // build subtable
      std::vector<Terminal> &subTerminals = current[i].elements;
      for (size_t tIdx = 0; tIdx < current[i].elements.size(); tIdx++) {
        subTerminals[tIdx].value = subTerminals[tIdx].value + 1;
      }
      
      State *newTable = buildTable(&subTerminals[0], current[i].elements.size(), nameCounter);
      table[i].state = stSwitchTable;
      table[i].link.table = newTable;
      table[i].link.linkIndex = (*nameCounter)++;
    }
    
  }
  
  return table;
}


void dumpTable(StateLink link, std::string &sourceOut)
{
  char buffer[256];
  for (int i = 0; i < tableSize; i++) {
    if (link.table[i].state == stSwitchTable)
      dumpTable(link.table[i].link, sourceOut);
  }
  
  if (link.linkIndex != -1)
    sourceOut += "static ";
  sourceOut += "StateElement ";
  if (link.linkIndex == -1) {
    sourceOut += "httpHeaderFSM";
  } else {
    sprintf(buffer, "table%i", link.linkIndex);
    sourceOut += buffer;
  }
  
  {
    sprintf(buffer, "[%i] = {\n", tableSize);
    sourceOut += buffer;
  }
  
  for (int i = 0; i < tableSize; i++) {
    State &T = link.table[i];
    switch (T.state) {
      case stFinish : {
        sprintf(buffer, "  { stFinish, %s, 0, 0}",  T.terminalName);
        sourceOut += buffer;
        break;
      }
      
      case stMemcmp : {
        sprintf(buffer, "  { stMemcmp, %s, \"%s\", %u}", T.terminalName, T.memcmpData.data, (unsigned)T.memcmpData.size);
        sourceOut += buffer;
        break;
      }
      
      case stError : {
        sourceOut += "  { stError, 0, 0, 0 }";
        break;
      }
      
      case stSwitchTable : {
        sprintf(buffer, "  { stSwitchTable, 0, table%i, 0}", T.link.linkIndex);
        sourceOut += buffer;
        break;
      }
    }
    
    if (i != tableSize-1)
      sourceOut += ",\n";
    else
      sourceOut += "\n";
  }
  
  sourceOut += "};\n\n";
}


void buildSimpleTableParser(Terminal *terminals,
                            size_t count,
                            const std::string &parserName,
                            const std::string &headerName,
                            std::string &headerOut,
                            std::string &sourceOut)
{
  int nameCounter = 0;
  State *table = buildTable(terminals, count, &nameCounter);
  
  // Dump enum
  headerOut += "enum {\n";
  for (size_t i = 0; i < count; i++) {
    if (i == 0) {
      headerOut += "  ";
      headerOut += terminals[i].constant;
      headerOut += " = 1,";
    } else if (i != count-1) {
      headerOut += "  ";
      headerOut += terminals[i].constant;
      headerOut += ",";
    } else {
      headerOut += "  ";
      headerOut += terminals[i].constant;
    }
    headerOut += '\n';
  }
  headerOut += "};\n\n";
  
  sourceOut += "#include \"p2putils/CommonParse.h\"\n";
  sourceOut += "#include \"";
  sourceOut += headerName;
  sourceOut += "\"\n\n";
  
  // Dump tables;
  StateLink link;
  link.linkIndex = -1;
  link.table = table;
  dumpTable(link, sourceOut);
}

enum {
  stWaitNewTerminal = 0,
  stWaitNewTerminalContinue,
  stWaitTerminalValue,
  stWaitTerminalValueContinue
};

static inline int isWhiteSpace(char c)
{
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}




int main(int argc, char **argv)
{
  if (argc != 4) {
    fprintf(stderr, "Usage: %s <in file> <source file> <header file>\n", argv[0]);
    exit(1);
  }
  
  // Read in file
  FILE *inFile = fopen(argv[1], "r");
  if (!inFile) {
    fprintf(stderr, "Error: can't open %s\n", argv[1]);
    exit(1);
  }
  
  fseek(inFile, 0, SEEK_END);
  long inFileSize = ftell(inFile);
  fseek(inFile, 0, SEEK_SET);
  char *inFileBuffer = new char[inFileSize];
  fread(inFileBuffer, inFileSize, 1, inFile);
  fclose(inFile);
  
  // Extract terminals
  const char *p = inFileBuffer;
  const char *end = inFileBuffer+inFileSize;
  int state = stWaitNewTerminal;
  std::string terminalName;
  std::string terminalValue;
  std::vector<Terminal> terminals;
  while (p < end) {
    switch (state) {
      case stWaitNewTerminal : {
        if (isWhiteSpace(*p)) {
          break;
        } else {
          terminalName = *p;
          state = stWaitNewTerminalContinue;
        }
        break;
      }
      
      case stWaitNewTerminalContinue : {
        if (*p == ':') {
          state = stWaitTerminalValue;
        } else {
          terminalName.push_back(*p);
        }
        
        break;
      }
      
      case stWaitTerminalValue : {
        if (isWhiteSpace(*p)) {
          break;
        } else if (*p == '\"') {
          state = stWaitTerminalValueContinue;
          terminalValue.clear();
        } else {
          fprintf(stderr, "Error: can't read terminal %s\n", terminalName.c_str());
          exit(1);
        }
        break;
      }
      
      case stWaitTerminalValueContinue : {
        if (*p == '\"') {
          Terminal t;
          t.constant = strdup(terminalName.c_str());
          t.value = strdup(terminalValue.c_str());
          terminals.push_back(t);
          state = stWaitNewTerminal;
        } else {
          terminalValue.push_back(*p);
        }

        break;
      }
      
      default:
        break;
    }
    
    p++;
  }
  
  std::string sourceOut;
  std::string headerOut;
  buildSimpleTableParser(&terminals[0], terminals.size(), "httpHeader", argv[3], headerOut, sourceOut);
  
  FILE *hSource = fopen(argv[2], "w+");
  FILE *hHeader = fopen(argv[3], "w+");
  
  fwrite(sourceOut.c_str(), sourceOut.size(), 1, hSource);
  fwrite(headerOut.c_str(), headerOut.size(), 1, hHeader);  
  
  fclose(hSource);
  fclose(hHeader);
  
  return 0;
}
