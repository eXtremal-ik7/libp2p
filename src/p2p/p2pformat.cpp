#include "p2p/p2pformat.h"

const char *p2pStream::jumpOverString()
{
  uint64_t length = readbe<uint64_t>();
  const char *s = seek<const char>(length);
  const char *e = seek<const char>(1);
  if (e && *e == 0)
    return s;
  return nullptr;
}

void p2pStream::writeString(const char *s)
{
  size_t length = strlen(s);
  writebe<uint64_t>(static_cast<uint64_t>(length));
  write(s, length);
  write<uint8_t>(0);
}

bool p2pStream::readStatusMessage(p2pErrorTy *error)
{
  *error = static_cast<p2pErrorTy>(read<uint8_t>());
  return !eof();
}


bool p2pStream::readConnectMessage(p2pConnectData *data)
{
  data->login = jumpOverString();
  data->password = jumpOverString();
  data->application = jumpOverString();
  return data->application != nullptr;
}

void p2pStream::writeStatusMessage(p2pErrorTy error)
{
  write<uint8_t>(error);
}


void p2pStream::writeConnectMessage(p2pConnectData data)
{
  writeString(data.login);
  writeString(data.password);
  writeString(data.application);
}
