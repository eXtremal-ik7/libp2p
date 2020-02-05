#include "p2pproto.h"
#include <list>
#include <map>
#include <vector>
#include <time.h>

class p2pNode;
class p2pPeer;
class xmstream;

typedef void p2pNodeCb(p2pPeer*);
typedef void p2pRequestCb(p2pPeer*, uint32_t, void*, size_t, void*);
typedef void p2pSignalCb(p2pPeer*, void*, size_t, void*);

struct p2pEventHandler {
  p2pNodeCb *callback;
  void *arg;
  coroutineTy *coroutine;
  time_t endPoint;
  void *out;
  size_t outSize;
};  

__NO_PADDING_BEGIN
class p2pPeer {
private:
  static void clientNetworkWaitEnd(aioUserEvent *event, void *arg);
  static void clientP2PConnectCb(AsyncOpStatus status, p2pConnection *connection, void *arg);
  static void clientReceiver(AsyncOpStatus status, p2pConnection *connection, p2pHeader header, p2pStream *stream, void *arg);
  static void checkTimeout(aioUserEvent *event, void *arg) {
    __UNUSED(event);
    static_cast<p2pPeer*>(arg)->checkTimeout();
  }
  static p2pErrorTy nodeAcceptCb(AsyncOpStatus status, p2pConnection *connection, p2pConnectData *data, void *arg);
  static void nodeMsgHandlerEP(void *peer) { static_cast<p2pPeer*>(peer)->nodeMsgHandler(); }
  
  void nodeMsgHandler();
  
public:
  asyncBase *_base;
  aioUserEvent *_event;
  aioUserEvent *_checkTimeoutEvent;
  p2pNode *_node;  
  HostAddress _address;
  bool _connected;

  p2pConnection *connection;
  std::map<unsigned, p2pEventHandler> handlersMap;
  
  p2pPeer(asyncBase *base, p2pNode *node, const HostAddress *address) :
    _base(base), _node(node), _address(*address), _connected(false), connection(nullptr) {
    _event = newUserEvent(base, 0, clientNetworkWaitEnd, this);
    _checkTimeoutEvent = newUserEvent(base, 0, checkTimeout, this);
    userEventStartTimer(_checkTimeoutEvent, 1000000, -1);
  }

  ~p2pPeer() {
    if (connection)
      p2pConnectionDelete(connection);
    deleteUserEvent(_checkTimeoutEvent);
    deleteUserEvent(_event);
  }
  
  void checkTimeout();
  bool createConnection();
  void destroyConnection();
  void connect();
  void connectAfter(uint64_t timeout) { userEventStartTimer(_event, timeout, 1); }
  
  void accept(bool coroutineMode, p2pConnection *connectionArg);
  
  void addHandler(uint32_t id, p2pNodeCb *callback, void *arg, uint64_t timeout) {
    p2pEventHandler handler;
    handler.callback = callback;
    handler.arg = arg;
    handler.coroutine = nullptr;
    handler.endPoint = timeout ? time(nullptr) + static_cast<time_t>(timeout/1000000) : static_cast<time_t>(0);
    handlersMap[id] = handler;
  }
  
  void addHandler(uint32_t id, coroutineTy *coroutine, uint64_t timeout, void *out, size_t outSize) {
    p2pEventHandler handler;
    handler.callback = nullptr;
    handler.arg = nullptr;
    handler.coroutine = coroutine;
    handler.endPoint = timeout ? time(nullptr) + static_cast<time_t>(timeout/1000000) : static_cast<time_t>(0);
    handler.out = out;
    handler.outSize = outSize;
    handlersMap[id] = handler;
  }
};
__NO_PADDING_END

class p2pNode {
private:
  asyncBase *_base;
  const char *_clusterName;
  std::vector<p2pPeer*> _connections;

  // client data
  p2pPeer *_lastActivePeer;
  volatile unsigned _lastId;
  std::list<p2pEventHandler> _connectionWaitHandlers;
  
  // node data
  aioObject *_listenerSocket;  
  p2pRequestCb *_requestHandler;
  void *_requestHandlerArg;
  p2pSignalCb *_signalHandler;  
  void *_signalHandlerArg;
  bool _coroutineMode;
  
private:  
  static void listener(AsyncOpStatus status, aioObject *listener, HostAddress client, socketTy socket, void *arg);

  p2pNode(asyncBase *base, const char *clusterName, bool coroutineMode) :
    _base(base), _clusterName(clusterName),
    _listenerSocket(nullptr), _requestHandler(nullptr), _requestHandlerArg(nullptr), _signalHandler(nullptr),
    _coroutineMode(coroutineMode){}
  
  void addHandler(p2pNodeCb *callback, void *arg, uint64_t timeout) {
    p2pEventHandler handler;
    handler.callback = callback;
    handler.arg = arg;
    handler.coroutine = nullptr;
    handler.endPoint = timeout ? time(nullptr) + static_cast<time_t>(timeout/1000000) : static_cast<time_t>(0);
    _connectionWaitHandlers.push_back(handler);
  }
  
  void addHandler(coroutineTy *coroutine, uint64_t timeout) {
    p2pEventHandler handler;
    handler.callback = nullptr;
    handler.arg = nullptr;
    handler.coroutine = coroutine;
    handler.endPoint = timeout ? time(nullptr) + static_cast<time_t>(timeout/1000000) : static_cast<time_t>(0);
    _connectionWaitHandlers.push_back(handler);
  }
  
public:
  static p2pNode *createClient(asyncBase *base,
                               const HostAddress *addresses,
                               size_t addressesNum,
                               const char *clusterName);
  
  static p2pNode *createNode(asyncBase *base,
                             const HostAddress *listenAddress,
                             const char *clusterName,
                             bool coroutineMode);
  
  void addPeer(p2pPeer *peer) { _connections.push_back(peer); }
  void removePeer(p2pPeer *peer) {
    auto It = std::find(_connections.begin(), _connections.end(), peer);
    if (It != _connections.end())
      _connections.erase(It);
  }
  
  void setLastActivePeer(p2pPeer *peer) { _lastActivePeer = peer; }
  void connectionEstablished(p2pPeer *peer);
  void connectionTimeout();
  
  void signal(p2pPeer *peer);

 
  asyncBase *base() { return _base; }
  
  // client api
  bool connected();
  bool ioWaitForConnection(uint64_t timeout);
  bool ioRequest(void *data, uint32_t size, uint64_t timeout, void *out, uint32_t outSize);

  // node api
  p2pRequestCb *getRequestHandler() { return _requestHandler; }
  void *getRequestHandlerArg() { return _requestHandlerArg; }
  
  void setRequestHandler(p2pRequestCb *handler, void *arg) {
    _requestHandler = handler;
    _requestHandlerArg = arg;
  }
  
  void setSignalHandler(p2pSignalCb *handler, void *arg) {
    _signalHandler = handler;
    _signalHandlerArg = arg;
  }
  void sendSignal(void *data, uint32_t size);
};



// Low-level messaging functions

