#include "p2pproto.h"
#include <list>
#include <map>
#include <vector>
#include <time.h>

class p2pNode;
class p2pPeer;
class xmstream;

typedef void p2pNodeCb(p2pPeer*);
typedef void p2pRequestCb(p2pPeer*, uint64_t, void*, size_t);
typedef void p2pSignalCb(p2pPeer*, void*, size_t, void*);

struct p2pEventHandler {
  void *callback;
  void *arg;
  coroutineTy *coroutine;
  time_t endPoint;
};  

struct p2pPeer {
private:
  static void clientNetworkWaitEnd(asyncBase *base, aioObject *event, void *arg);  
  static void clientNetworkConnectCb(AsyncOpStatus status, asyncBase *base, aioObject *object, void *arg);
  static void clientP2PConnectCb(p2pInfo *info);  
  static void clientReceiver(p2pInfo *info);  
  static p2pErrorTy nodeAcceptCb(p2pConnectData *data, p2pInfo *info);
  static void *nodeMsgHandlerEP(void *peer) { ((p2pPeer*)peer)->nodeMsgHandler(); }
  
  void nodeMsgHandler();
  
public:
  asyncBase *_base;
  aioObject *_event;
  p2pNode *_node;  
  HostAddress _address;
  bool _connected;

  p2pConnection *connection;
  std::map<unsigned, p2pEventHandler> handlersMap;
  
  p2pPeer(asyncBase *base, p2pNode *node, const HostAddress *address) :
    _base(base), _node(node), _address(*address), _connected(false), connection(0) {
    _event = newUserEvent(base, clientNetworkWaitEnd, this);
  }
  
  bool createConnection();
  void destroyConnection();
  void connect();
  void connectAfter(uint64_t timeout) { userEventStartTimer(_event, timeout, 1); }
  
  void accept(bool coroutineMode, p2pConnection *connectionArg);
  
  void addHandler(uint32_t id, p2pNodeCb *callback, void *arg, uint64_t timeout) {
    p2pEventHandler handler;
    handler.callback = (void*)callback;
    handler.arg = arg;
    handler.coroutine = 0;
    handler.endPoint = timeout ? time(0) + (timeout/1000000) : 0;
    handlersMap[id] = handler;
  }
  
  void addHandler(uint32_t id, coroutineTy *coroutine, uint64_t timeout) {
    p2pEventHandler handler;
    handler.callback = 0;
    handler.arg = 0;
    handler.coroutine = coroutine;
    handler.endPoint = timeout ? time(0) + (timeout/1000000) : 0;
    handlersMap[id] = handler;
  }
};

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
  p2pSignalCb *_signalHandler;  
  void *_signalHandlerArg;
  bool _coroutineMode;
  
private:  
  static void listener(AsyncOpStatus status, asyncBase *base, aioObject *listener, HostAddress client, socketTy socket, void *arg);

  p2pNode(asyncBase *base, const char *clusterName, bool coroutineMode) :
    _base(base), _clusterName(clusterName), _coroutineMode(coroutineMode),
    _listenerSocket(0), _requestHandler(0), _signalHandler(0) {}
  
  void addHandler(p2pNodeCb *callback, void *arg, uint64_t timeout) {
    p2pEventHandler handler;
    handler.callback = (void*)callback;
    handler.arg = arg;
    handler.coroutine = 0;
    handler.endPoint = timeout ? time(0) + (timeout/1000000) : 0;
    _connectionWaitHandlers.push_back(handler);
  }
  
  void addHandler(coroutineTy *coroutine, uint64_t timeout) {
    p2pEventHandler handler;
    handler.callback = 0;
    handler.arg = 0;
    handler.coroutine = coroutine;
    handler.endPoint = timeout ? time(0) + (timeout/1000000) : 0;
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
  p2pPeer *ioRequest(void *data, size_t size, uint64_t timeout);

  // node api
  p2pRequestCb *getRequestHandler() { return _requestHandler; }
  void setRequestHandler(p2pRequestCb *handler) { _requestHandler = handler; }
  void setSignalHandler(p2pSignalCb *handler, void *arg) {
    _signalHandler = handler;
    _signalHandlerArg = arg;
  }
  void sendSignal(void *data, size_t size);
};



// Low-level messaging functions

