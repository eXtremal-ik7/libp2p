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

enum SendEmailStateTy {
  stStartTls = 0,
  stEhlo2,
  stAuth,
  stFrom,
  stTo,
  stDataMessage,
  stData,
  stLast
};

struct Context {
  asyncBase *Base;
  SendEmailStateTy State;
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

void responseCb(AsyncOpStatus status, unsigned code, SMTPClient *client, void *arg)
{
  Context *context = static_cast<Context*>(arg);
  if (status != aosSuccess) {
    if (status == smtpInvalidFormat)
      fprintf(stderr, "SMTP Protocol mismatch\n");
    else if (status == smtpError)
      fprintf(stderr, "SMTP Error code: %u; text: %s\n", code, smtpClientGetResponse(client) ? smtpClientGetResponse(client) : "?");
    else
      fprintf(stderr, "Error %i\n", status);

    postQuitOperation(context->Base);
    return;
  }

  const char *response = smtpClientGetResponse(client);
  if (response) {
    fprintf(stdout, "--> %s\n", response);
    fflush(stdout);
  }

  switch (context->State) {
    case stStartTls : {
      context->State = stEhlo2;
      aioSmtpStartTls(client, afNone, 5000000, responseCb, arg);
      break;
    }

    case stEhlo2 : {
      context->State = stAuth;
      std::string ehlo = "EHLO ";
      ehlo.append(context->clientHost);
      aioSmtpCommand(client, ehlo.c_str(), afNone, 5000000, responseCb, arg);
      fprintf(stdout, "<-- %s\n", ehlo.c_str());
      break;
    }

    case stAuth : {
      context->State = stFrom;
      aioSmtpLogin(client, context->login, context->password, afNone, 5000000, responseCb, arg);
      break;
    }

    case stFrom : {
      context->State = stTo;
      std::string from = (std::string)"MAIL From: <" + context->from + ">";
      aioSmtpCommand(client, from.c_str(), afNone, 5000000, responseCb, arg);
      fprintf(stdout, "<-- %s\n", from.c_str());
      break;
    }

    case stTo : {
      context->State = stDataMessage;
      std::string to = (std::string)"RCPT To: <" + context->to + ">";
      aioSmtpCommand(client, to.c_str(), afNone, 5000000, responseCb, arg);
      fprintf(stdout, "<-- %s\n", to.c_str());
      break;
    }

    case stDataMessage : {
      context->State = stData;
      aioSmtpCommand(client, "DATA", afNone, 5000000, responseCb, arg);
      fprintf(stdout, "<-- %s\n", "DATA");
      break;
    }

    case stData : {
      context->State = stLast;
      std::string text = (std::string)
        "From: " + context->from + "\r\n" +
        "To: " + context->to + "\r\n" +
        "Subject: " + context->subject + "\r\n" +
        context->text + "\r\n.\r\n";
      aioSmtpCommand(client, text.c_str(), afNone, 5000000, responseCb, arg);
      fprintf(stdout, "<-- %s\n", text.c_str());
      break;
    }

    case stLast : {
      postQuitOperation(context->Base);
      break;
    }
  }
}

void connectCb(AsyncOpStatus status, SMTPClient *client, void *arg)
{
  Context *context = static_cast<Context*>(arg);
  if (status != aosSuccess) {
    fprintf(stderr, "Error %i\n", status);
    postQuitOperation(context->Base);
    return;
  }

  const char *response = smtpClientGetResponse(client);
  if (response) {
    fprintf(stdout, "--> %s\n", response);
    fflush(stdout);
  }

  std::string ehlo = "EHLO ";
  ehlo.append(context->clientHost);
  aioSmtpCommand(client, ehlo.c_str(), afNone, 5000000, responseCb, arg);
  fprintf(stdout, "<-- %s\n", ehlo.c_str());
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

  HostAddress smtpAddress;
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

    smtpAddress.family = AF_INET;
    smtpAddress.ipv4 = static_cast<uint32_t>(addr);
    smtpAddress.port = htons(atoi(colonPos + 1));
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
  SMTPClient *client = smtpClientNew(base, localHost, serverType);

  context.Base = base;
  context.State = context.startTls ? stStartTls : stAuth;
  aioSmtpConnect(client, smtpAddress, 5000000, connectCb, &context);

  asyncLoop(base);
  return 0;
}
