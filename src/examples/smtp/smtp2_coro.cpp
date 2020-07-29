#include "asyncio/asyncio.h"
#include "asyncio/socket.h"
#include "asyncio/smtp.h"

#include "p2putils/uriParse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(OS_WINDOWS)
#include <netdb.h>
#endif

struct Context {
  asyncBase *Base;
  SMTPClient *Client;
  HostAddress SmtpServerAddress;
  const char *server;
  const char *type;
  const char *clientHost;
  const char *login;
  const char *password;
  const char *from;
  const char *to;
  const char *subject;
  const char *text;
  bool startTls;
};

void sendMailCoro(void *arg)
{
  Context *context = static_cast<Context*>(arg);
  int result = ioSmtpSendMail(context->Client,
                 context->SmtpServerAddress,
                 context->startTls,
                 context->clientHost,
                 context->login,
                 context->password,
                 context->from,
                 context->to,
                 context->subject,
                 context->text,
                 afNone,
                 5000000);

  int code = smtpClientGetResultCode(context->Client);
  const char *response = smtpClientGetResponse(context->Client);
  if (result != 0) {
    int status = -result;
    if (status == smtpInvalidFormat)
      fprintf(stderr, "SMTP Protocol mismatch\n");
    else if (status == smtpError)
      fprintf(stderr, "SMTP Error code: %u; text: %s\n", code, response ? response : "?");
    else
      fprintf(stderr, "Error %i\n", status);
  } else if (response) {
    fprintf(stdout, "--> %s\n", response);
    fflush(stdout);
  }

  postQuitOperation(context->Base);
}

int main(int argc, char **argv)
{
  if (argc != 10) {
    fprintf(stderr, "usage: %s <server:port> <type> <client host> <login> <password> <from> <to> <subject> <text>\n", argv[0]);
    return 1;
  }

  Context context;
  context.server = argv[1];
  context.type = argv[2];
  context.clientHost = argv[3];
  context.login = argv[4];
  context.password = argv[5];
  context.from = argv[6];
  context.to = argv[7];
  context.subject = argv[8];
  context.text = argv[9];
  SmtpServerType serverType = smtpServerPlain;

  // Build HostAddress for server
  {
    char *colonPos = (char*)strchr(context.server, ':');
    if (colonPos == nullptr) {
      fprintf(stderr, "Invalid server %s\nIt must have address:port format\n", context.server);
      return 1;
    }

    *colonPos = 0;
    hostent *host = gethostbyname(context.server);
    if (!host) {
      fprintf(stderr, " * cannot retrieve address of %s (gethostbyname failed)\n", context.server);
    }

    u_long addr = host->h_addr ? *reinterpret_cast<u_long*>(host->h_addr) : 0;
    if (!addr) {
      fprintf(stderr, " * cannot retrieve address of %s (gethostbyname returns 0)\n", context.server);
      return 1;
    }

    context.SmtpServerAddress.family = AF_INET;
    context.SmtpServerAddress.ipv4 = static_cast<uint32_t>(addr);
    context.SmtpServerAddress.port = htons(atoi(colonPos + 1));
  }

  // Analyze type
  context.startTls = false;
  if (strcmp(context.type, "plain") == 0) {
    serverType = smtpServerPlain;
  } else if (strcmp(context.type, "smtps") == 0) {
    serverType = smtpServerSmtps;
  } else if (strcmp(context.type, "starttls") == 0) {
    serverType = smtpServerPlain;
    context.startTls = true;
  } else {
    fprintf(stderr, "Invalid server type\nAvailable types: plain, smtps, starttls\n");
    return 1;
  }


  initializeSocketSubsystem();
  asyncBase *base = createAsyncBase(amOSDefault);

  HostAddress localHost;
  localHost.ipv4 = INADDR_ANY;
  localHost.family = AF_INET;
  localHost.port = 0;
  context.Client = smtpClientNew(base, localHost, serverType);

  context.Base = base;
  coroutineTy *coro = coroutineNew(sendMailCoro, &context, 0x10000);
  coroutineCall(coro);
  asyncLoop(base);
  return 0;
}
