#include "p2p/p2p.h"
#include "asyncio/coroutine.h"
#include "asyncio/socket.h"
#include <stdio.h>
#include <stdlib.h>

// Default p2p connection timeout = 1 second
#define P2P_CONNECT_TIMEOUT 1000000

void p2pPeer::checkTimeout()
{
  // check requests
  // TODO: use better timer grouping
  time_t currentTime = time(nullptr);
  for (auto I = handlersMap.begin(), IE = handlersMap.end(); I != IE;) {
    p2pEventHandler &handler = I->second;      
    if (handler.endPoint && currentTime >= handler.endPoint) {
      if (handler.coroutine) {
        _node->setLastActivePeer(nullptr);
        coroutineCall(handler.coroutine);
      } else if (handler.callback) {
        reinterpret_cast<p2pNodeCb*>(handler.callback)(nullptr);
      }
      
      handlersMap.erase(I++);
    } else {
      ++I;
    }
  }  
}

void p2pPeer::clientReceiver(AsyncOpStatus status, p2pConnection *connection, p2pHeader header, p2pStream *stream, void *arg)
{
  __UNUSED(connection);
  __UNUSED(stream);
  p2pPeer *peer = static_cast<p2pPeer*>(arg);
  
  if (status == aosSuccess) {
    switch (header.type) {
      case p2pMsgResponse : {
        auto It = peer->handlersMap.find(header.id);
        if (It != peer->handlersMap.end()) {
          p2pEventHandler &handler = It->second;
          if (header.size > handler.outSize)
            break;
          
          memcpy(handler.out, peer->connection->stream.data(), peer->connection->stream.sizeOf());
          if (handler.coroutine) {
            peer->_node->setLastActivePeer(peer);
            coroutineCall(handler.coroutine);
          } else if (handler.callback) {
            handler.callback(peer);
          }
          peer->handlersMap.erase(It);
        }
        break;
        
      }
      case p2pMsgSignal :
        peer->_node->signal(peer);
        break;
    }
    
    aiop2pRecvStream(peer->connection, peer->connection->stream, 65536, afNone, 0, clientReceiver, peer);
  } else {
    // try reconnect
    peer->connect();
  }
}

void p2pPeer::clientP2PConnectCb(AsyncOpStatus status, p2pConnection *connection, void *arg)
{
  __UNUSED(connection);
  p2pPeer *peer = static_cast<p2pPeer*>(arg);
  if (status == aosSuccess) {
    peer->_connected = true;
    peer->_node->connectionEstablished(peer);   
    aiop2pRecvStream(peer->connection, peer->connection->stream, 65536, afNone, 0, clientReceiver, peer);
  } else if (status == aosTimeout) {
    peer->connect();
  } else {
    peer->connectAfter(P2P_CONNECT_TIMEOUT);
  }
}

void p2pPeer::clientNetworkWaitEnd(aioUserEvent *event, void *arg)
{ 
  __UNUSED(event);
  static_cast<p2pPeer*>(arg)->connect();
}

p2pErrorTy p2pPeer::nodeAcceptCb(AsyncOpStatus status, p2pConnection *connection, p2pConnectData *data, void *arg)
{
  __UNUSED(status);
  __UNUSED(connection);
  __UNUSED(data);
  __UNUSED(arg);
  return p2pOk;
}

void p2pPeer::nodeMsgHandler()
{
  if (iop2pAccept(connection, 3000000, nodeAcceptCb, this) != aosSuccess) {
    delete this;
    return;
  }
  
  _node->addPeer(this);
  bool valid = true;
  p2pHeader header;
  ssize_t status;
  while (valid && (status = iop2pRecvStream(connection, connection->stream, 65536, afNone, 0, &header)) > 0) {
    switch (header.type) {
      case p2pMsgRequest : {
        if (p2pRequestCb *handler = _node->getRequestHandler()) {
          handler(this, header.id, connection->stream.data(), connection->stream.sizeOf(), _node->getRequestHandlerArg());
        } else {
          valid = false;
        }
        break;
      }
      default :
        valid = false;
        break;
    }
  }
  
  _node->removePeer(this);
  delete this;
}


bool p2pPeer::createConnection()
{
  HostAddress localAddress;
  localAddress.family = AF_INET;
  localAddress.ipv4 = INADDR_ANY;
  localAddress.port = 0;
  socketTy hSocket = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
  if (socketBind(hSocket, &localAddress) != 0) {
    connection = nullptr;
    return false;
  }
  
  aioObject *socketOp = newSocketIo(_base, hSocket);
  connection = p2pConnectionNew(socketOp);
  return true;
}

void p2pPeer::destroyConnection()
{
  if (connection) {
    p2pConnectionDelete(connection);
    connection = nullptr;
  }
}

void p2pPeer::connect()
{
  _connected = false;
  // TODO: don't call first time
  _node->connectionTimeout();
  destroyConnection();
  if (createConnection()) {
    p2pConnectData data;
    data.login = "pool";
    data.password = "pool";
    data.application = "pool_rpc";
    aiop2pConnect(connection, &_address, &data, P2P_CONNECT_TIMEOUT, clientP2PConnectCb, this);
  } else {
    userEventStartTimer(_event, 1000000, 1);
  }
}


void p2pPeer::accept(bool coroutineMode, p2pConnection *connectionArg)
{
  connection = connectionArg;
  if (coroutineMode) {
    coroutineTy *handlerProc = coroutineNew(nodeMsgHandlerEP, this, 0x100000);
    coroutineCall(handlerProc);
  }
}


p2pNode *p2pNode::createClient(asyncBase *base,
                               const HostAddress *addresses,
                               size_t addressesNum,
                               const char *clusterName)
{
  p2pNode *node = new p2pNode(base, clusterName, false);
  for (size_t i = 0; i < addressesNum; i++) {
    p2pPeer *peer = new p2pPeer(base, node, &addresses[i]);
    node->addPeer(peer);
    peer->connect();
  }
  
  return node;
}

void p2pNode::connectionEstablished(p2pPeer *peer)
{
  setLastActivePeer(peer);  
  for (auto I = _connectionWaitHandlers.begin(), IE = _connectionWaitHandlers.end(); I != IE; ++I) {
    p2pEventHandler &handler = *I;  
    if (handler.coroutine) {
      coroutineCall(handler.coroutine);
    } else if (handler.callback) {
      handler.callback(peer);
    }
  }
  
  _connectionWaitHandlers.clear();
}

void p2pNode::connectionTimeout()
{
  time_t currentTime = time(nullptr);
  for (auto I = _connectionWaitHandlers.begin(), IE = _connectionWaitHandlers.end(); I != IE;) {
    p2pEventHandler &handler = *I;      
    if (handler.endPoint && currentTime >= handler.endPoint) {
      if (handler.coroutine) {
        setLastActivePeer(nullptr);
        coroutineCall(handler.coroutine);
      } else if (handler.callback) {
        handler.callback(nullptr);
      }
    
      _connectionWaitHandlers.erase(I++);
    } else {
      ++I;
    }
  }
}

void p2pNode::signal(p2pPeer *peer)
{
  if (_signalHandler) {
    xmstream &stream = peer->connection->stream;
    _signalHandler(peer, stream.data(), stream.sizeOf(), _signalHandlerArg);
  }
}

bool p2pNode::connected()
{
  if (_connections.empty())
    return false;
  
  for (auto c: _connections) {
    if (c->_connected)
      return true;
  }
  
  return false;
}


bool p2pNode::ioWaitForConnection(uint64_t timeout)
{
  for (size_t i = 0; i < _connections.size(); i++) {
    if (_connections[i]->_connected)
      return true;
  }

  addHandler(coroutineCurrent(), timeout);
  coroutineYield();
  return _lastActivePeer != nullptr;
}


bool p2pNode::ioRequest(void *data, uint32_t size, uint64_t timeout, void *out, uint32_t outSize)
{
  // TODO: implement strategy for peer select
  if (_connections.empty())
    return 0;

  for (size_t i = 0; i < _connections.size(); i++) {
    p2pPeer *peer = _connections[i];
    if (!peer->_connected)
      continue;
    
    uint32_t id = _lastId++;
    aiop2pSend(peer->connection, data, id, p2pMsgRequest, size, afNone, timeout, nullptr, nullptr);
    peer->addHandler(id, coroutineCurrent(), timeout, out, outSize);
    coroutineYield();    
    if (_lastActivePeer)
      return true;
  }
  
  return false;
}



void p2pNode::listener(AsyncOpStatus status, aioObject *listenSocket, HostAddress client, socketTy clientSocket, void *arg)
{
  p2pNode *node = static_cast<p2pNode*>(arg);
  
  if (status == aosSuccess) {
    aioObject *object = newSocketIo(node->_base, clientSocket);
    p2pConnection *connection = p2pConnectionNew(object);
    p2pPeer *peer = new p2pPeer(node->_base, node, &client);
    peer->accept(node->_coroutineMode, connection);
  }
  
  aioAccept(listenSocket, 0, listener, node);
}


p2pNode* p2pNode::createNode(asyncBase *base,
                             const HostAddress *listenAddress,
                             const char *clusterName,
                             bool coroutineMode)
{
  socketTy hSocket = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
  socketReuseAddr(hSocket);
  aioObject *socketOp = newSocketIo(base, hSocket);
  if (socketBind(hSocket, listenAddress) != 0)
    return nullptr;
  if (socketListen(hSocket) != 0)
    return nullptr;

  p2pNode *node = new p2pNode(base, clusterName, coroutineMode);
  node->_listenerSocket = socketOp;
  aioAccept(socketOp, 0, listener, node);
  return node;
}

void p2pNode::sendSignal(void *data, uint32_t size)
{
  for (auto c: _connections)
    aiop2pSend(c->connection, data, 0, p2pMsgSignal, size, afNone, 3000000, nullptr, nullptr);
}
