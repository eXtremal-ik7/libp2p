#include "asyncio/device.h"
#ifdef OS_WINDOWS
#include <Windows.h>
#endif

iodevTy serialPortOpen(const char *name)
{
  HANDLE hFile =
    CreateFile(name, GENERIC_READ|GENERIC_WRITE, 0, NULL,
               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
  if (hFile != INVALID_HANDLE_VALUE)
    return hFile;
  else
    return 0;
}

void serialPortClose(iodevTy port)
{
  CloseHandle(port);
}

int serialPortSetConfig(iodevTy port,
                        int speed,
                        int dataBits,
                        int stopBits,
                        int parity)
{
  DCB dcb;
  if (GetCommState(port, &dcb)) {
    switch (speed) {
      case 110:
       dcb.BaudRate = CBR_110;
        break;
      case 300:
        dcb.BaudRate = CBR_300;
        break;
      case 600:
        dcb.BaudRate = CBR_600;
        break;
      case 1200:
        dcb.BaudRate = CBR_1200;
        break;
      case 2400:
        dcb.BaudRate = CBR_2400;
        break;
      case 4800:
        dcb.BaudRate = CBR_4800;
        break;
      case 9600:
        dcb.BaudRate = CBR_9600;
        break;
      case 19200:
        dcb.BaudRate = CBR_19200;
        break;
      case 38400:
        dcb.BaudRate = CBR_38400;
        break;
      case 57600:
        dcb.BaudRate = CBR_57600;
        break;
      case 115200:
        dcb.BaudRate = CBR_115200;
        break;
      default:
        dcb.BaudRate = CBR_9600;
    }

    dcb.ByteSize = dataBits;
    if (stopBits == 1)
      dcb.StopBits = ONESTOPBIT;
    else
      dcb.StopBits = TWOSTOPBITS;

    if (parity == 'N') {
      dcb.Parity = NOPARITY;
      dcb.fParity = FALSE;
    } else if (parity == 'E') {
      dcb.Parity = EVENPARITY;
      dcb.fParity = TRUE;
    } else {
      dcb.Parity = ODDPARITY;
      dcb.fParity = TRUE;
    }

    dcb.fTXContinueOnXoff = TRUE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fBinary = TRUE;
    dcb.fAbortOnError = FALSE;
    serialPortFlush(port);
    if (SetCommState(port, &dcb))
      return 1;
  }

  return 0;
}

void serialPortFlush(iodevTy port)
{
  PurgeComm(port, PURGE_RXCLEAR | PURGE_TXCLEAR |
                  PURGE_RXABORT | PURGE_TXABORT);
}
