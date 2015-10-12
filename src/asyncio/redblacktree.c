#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include "redblacktree.h"

static treeNodePtr newNode();
static treeNodePtr binTreeInsert(treeNodePtr _root, descrStruct _keyValue);
static treeNodePtr getGrandpa(treeNodePtr _node);
static treeNodePtr getUncle(treeNodePtr _node);
static treeNodePtr getSibling(treeNodePtr _node);
static void freeNode_R(treeNodePtr _node);
static void balance_icase1(TreePtr _tree, treeNodePtr _node);
static void balance_icase2(TreePtr _tree, treeNodePtr _node);
static void balance_icase3(TreePtr _tree, treeNodePtr _node);
static void balance_icase4(TreePtr _tree, treeNodePtr _node);
static void balance_icase5(TreePtr _tree, treeNodePtr _node);
static void balance_dcase1(TreePtr _tree, treeNodePtr _node);
static void balance_dcase2(TreePtr _tree, treeNodePtr _node);
static void balance_dcase3(TreePtr _tree, treeNodePtr _node);
static void balance_dcase4(TreePtr _tree, treeNodePtr _node);
static void balance_dcase5(TreePtr _tree, treeNodePtr _node);
static void balance_dcase6(TreePtr _tree, treeNodePtr _node);
static void rotateLeft(TreePtr _tree, treeNodePtr _node);
static void rotateRight(TreePtr _tree, treeNodePtr _node);
static treeNodePtr findNode_R(treeNodePtr _node, descrStruct _keyvalue);
static treeNodePtr findMin_R(treeNodePtr _node);
static treeNodePtr findMax_R(treeNodePtr _node);
static void swapNodes(treeNodePtr _node1, treeNodePtr _node2);
static void deleteWithOneChild(TreePtr _tree, treeNodePtr _node);
static void replaceParentChild(TreePtr _tree, treeNodePtr _node1, treeNodePtr _node2);
static int cmpFunction(const descrStruct *arg1, const descrStruct *arg2);

static int cmpFunction(const descrStruct *arg1, const descrStruct *arg2)
{
  if (arg1->sock_ == arg2->sock_)
    return 0;
  if (arg1->sock_ > arg2->sock_)
    return 1;
  else
    return -1;
}


static treeNodePtr newNode()
{
  treeNodePtr _newNode;
  _newNode = malloc(sizeof(treeNode));
  _newNode->color = Black;
  _newNode->right = NULL;
  _newNode->left = NULL;
  _newNode->parent = NULL;
  _newNode->isNil = _true;
  return _newNode;
}


void initTree(TreePtr _tree)
{
  _tree->size = 0;
  _tree->root = newNode();
}


void freeTree(TreePtr _tree)
{
  freeNode_R(_tree->root);
}


static void freeNode_R(treeNodePtr _node)
{
  if(_node->left != NULL)
    freeNode_R(_node->left);
  if(_node->right != NULL)
    freeNode_R(_node->right);
  free(_node);
}


void insertValue(TreePtr _tree, descrStruct _keyValue)
{
  treeNodePtr _node;
  
  _node = binTreeInsert(_tree->root, _keyValue);
  balance_icase1(_tree, _node);
}


static treeNodePtr binTreeInsert(treeNodePtr _root, descrStruct _keyValue)
{  
  if(_root->isNil) {
    _root->isNil = _false;
    _root->keyValue = _keyValue;
    _root->color = Red;
    _root->left = newNode();
    _root->left->parent = _root;
    _root->right = newNode();
    _root->right->parent = _root;
  } else {
    if (cmpFunction(&_keyValue, &(_root->keyValue)) > 0) {
      _root->right->parent = _root;
      return binTreeInsert(_root->right, _keyValue);
    } else {
      _root->left->parent = _root;
      return binTreeInsert(_root->left, _keyValue);
    }
  }
  return _root;
}


static treeNodePtr getGrandpa(treeNodePtr _node)
{
  if ((_node != NULL) && (_node->parent != NULL))
    return _node->parent->parent;
  else 
    return NULL;
}


static treeNodePtr getUncle(treeNodePtr _node)
{
  treeNodePtr grandpa = getGrandpa(_node);
  if(grandpa == NULL)
    return NULL;
  if(_node->parent == grandpa->left)
    return grandpa->right;
  else
    return grandpa->left; 
}


static treeNodePtr getSibling(treeNodePtr _node)
{
  if (_node == _node->parent->left)
    return _node->parent->right;
  else 
    return _node->parent->left;
}


static void balance_icase1(TreePtr _tree, treeNodePtr _node)
{
  if (_node->parent == NULL)
    _node->color = Black;
  else
    balance_icase2(_tree, _node);
}


static void balance_icase2(TreePtr _tree, treeNodePtr _node)
{
  if (_node->parent->color == Black)
    return;
  else
    balance_icase3(_tree, _node);
}


static void balance_icase3(TreePtr _tree, treeNodePtr _node)
{
  treeNodePtr uncle = getUncle(_node);
  treeNodePtr grandpa;

  if ((uncle != NULL) && 
      (uncle->color == Red) && 
      (_node->parent->color == Red)) {
    
    _node->parent->color = Black;
    uncle->color = Black;
    grandpa = getGrandpa(_node);
    grandpa->color = Red;
    balance_icase1(_tree, grandpa);
  
  } else {
    balance_icase4(_tree, _node);
  }
}


static void balance_icase4(TreePtr _tree, treeNodePtr _node)
{
  treeNodePtr grandpa;
  grandpa = getGrandpa(_node);
  
  if ((_node == _node->parent->right) &&
      (_node->parent == grandpa->left)) {
    rotateLeft(_tree, _node->parent);
    _node = _node->left;
  } else if ((_node == _node->parent->left) && 
             (_node->parent == grandpa->right)) {
    rotateRight(_tree, _node->parent);
    _node = _node->right; 
  }
  balance_icase5(_tree, _node);
}


static void balance_icase5(TreePtr _tree, treeNodePtr _node)
{
  treeNodePtr grandpa;
  grandpa = getGrandpa(_node);

  _node->parent->color = Black;
  grandpa->color = Red;

  if ((_node == _node->parent->left) && (_node->parent == grandpa->left))
    rotateRight(_tree, grandpa);
  else
    rotateLeft(_tree, grandpa);
}


static void rotateLeft(TreePtr _tree, treeNodePtr _node)
{
  treeNodePtr temp = _node->right;
  _node->right = temp->left; 
  
  temp->left->parent = _node; 
  
  temp->parent = _node->parent;

  if (_node->parent) {
    if (_node == _node->parent->left)
      _node->parent->left = temp;
    else
      _node->parent->right = temp;
  } else {
    _tree->root = temp;
  }
  
  temp->left = _node;
    _node->parent = temp;
}


static void rotateRight(TreePtr _tree, treeNodePtr _node)
{
  treeNodePtr temp = _node->left;
  _node->left = temp->right;
  
  temp->right->parent = _node;
 
  temp->parent = _node->parent;

  if (_node->parent) {
    if (_node == _node->parent->right)
      _node->parent->right = temp;
    else
      _node->parent->left = temp;
  } else {
    _tree->root = temp;
  }

  temp->right = _node;
  _node->parent = temp;
}


deleteMsg deleteValue(TreePtr _tree, descrStruct _keyvalue)
{
  treeNodePtr nodeDeleting, minRightNode;
  
  nodeDeleting = findNode_R(_tree->root, _keyvalue);
  if (nodeDeleting == NULL)
    return NOT_FOUND;
  
  if (nodeDeleting->right->isNil || nodeDeleting->left->isNil)
    deleteWithOneChild(_tree, nodeDeleting);
  else {
    minRightNode = findMin_R(nodeDeleting->right);
    swapNodes(minRightNode, nodeDeleting);
    deleteWithOneChild(_tree, minRightNode);
  }

  return DELETING_OK;
}


treeNodePtr findNode(const TreePtr _tree, descrStruct _keyvalue)
{
  return findNode_R(_tree->root, _keyvalue);
}


static treeNodePtr findNode_R(treeNodePtr _node, descrStruct _keyvalue)
{
  assert(_node != NULL);
  
  if (_node->isNil)
    return NULL;
 
  if (cmpFunction(&_keyvalue, &(_node->keyValue)) == 0)
    return _node;

  if (cmpFunction(&_keyvalue, &(_node->keyValue)) > 0)
    return findNode_R(_node->right, _keyvalue);
  else
    return findNode_R(_node->left, _keyvalue);
}


static void swapNodes(treeNodePtr _node1, treeNodePtr _node2)
{
  treeNode temp;
  temp.keyValue = _node1->keyValue;
  _node1->keyValue = _node2->keyValue;
  _node2->keyValue = temp.keyValue;
}


static void deleteWithOneChild(TreePtr _tree, treeNodePtr _node)
{
  treeNodePtr child;
  assert(_node->right->isNil || _node->left->isNil);
  
  child = _node->right->isNil ? _node->left : _node->right;
  replaceParentChild(_tree, _node, child);

  if (_node->color == Black) {
    if (child->color == Red)
      child->color = Black;
    else
      balance_dcase1(_tree, child);
  }

  freeNode_R(_node);
}


static void balance_dcase1(TreePtr _tree, treeNodePtr _node)
{
  if (_node->parent != NULL)
    balance_dcase2(_tree, _node);
}


static void balance_dcase2(TreePtr _tree, treeNodePtr _node)
{
  treeNodePtr sibling;
  sibling = getSibling(_node);

  if (sibling->color == Red) {
    _node->parent->color = Red;
    sibling->color = Black;
    if (_node == _node->parent->right)
      rotateRight(_tree, _node->parent);
    else
      rotateLeft(_tree, _node->parent);
  }
  
  balance_dcase3(_tree, _node);
}


static void balance_dcase3(TreePtr _tree, treeNodePtr _node)
{
  treeNodePtr sibling;
  sibling = getSibling(_node);

  if ((_node->parent->color == Black) && 
      (sibling->color == Black) && 
      (sibling->left->color == Black) && 
      (sibling->right->color == Black)) {
        sibling->color = Red;
        balance_dcase1(_tree, _node->parent);
  } else 
        balance_dcase4(_tree, _node);
}


static void balance_dcase4(TreePtr _tree, treeNodePtr _node)
{
  treeNodePtr sibling = getSibling(_node);
  if ((_node->parent->color == Red) && 
      (sibling->color == Black) && 
      (sibling->left->color == Black) && 
      (sibling->right->color == Black)) {
        sibling->color = Red;
        _node->parent->color = Black;
  } else
        balance_dcase5(_tree,_node);
}


static void balance_dcase5(TreePtr _tree, treeNodePtr _node)
{
  treeNodePtr sibling = getSibling(_node);
  if (sibling->color == Black) {
    if ((_node == _node->parent->left) && 
        (sibling->right->color == Black) && 
        (sibling->left->color == Red)) {
          sibling->color = Red;
          sibling->left->color = Black;
          rotateRight(_tree, sibling);
    } else if ((_node == _node->parent->right) && 
               (sibling->right->color == Red) && 
               (sibling->left->color == Black)) {
           sibling->color = Red;
           sibling->right->color = Black;
           rotateLeft(_tree, sibling);
    }
  }
  balance_dcase6(_tree, _node);
}


static void balance_dcase6(TreePtr _tree, treeNodePtr _node)
{
  treeNodePtr sibling = getSibling(_node);
  sibling->color = _node->parent->color;
  _node->parent->color = Black;

  if (_node == _node->parent->left) {
    sibling->right->color = Black;
    rotateLeft(_tree, _node->parent);
  } else {
    sibling->left->color = Black;
    rotateRight(_tree, _node->parent);
  }
}


descrStruct findMinKeyValue(const TreePtr _tree)
{
  treeNodePtr temp;
  assert(_tree->root != NULL);
  assert(!_tree->root->isNil);

  temp = findMin_R(_tree->root);
  return temp->keyValue;
}


descrStruct findMaxKeyValue(const TreePtr _tree)
{
  treeNodePtr temp;
  assert(_tree->root != NULL);
  assert(!_tree->root->isNil);

  temp = findMax_R(_tree->root);
  return temp->keyValue;
}


static treeNodePtr findMin_R(treeNodePtr _node)
{
  assert(_node != NULL);
  
  if (_node->isNil)
    return NULL;

  if (_node->left->isNil)
    return _node;
  else
    return findMin_R(_node->left);
}


static treeNodePtr findMax_R(treeNodePtr _node)
{
  assert(_node != NULL);

  if (_node->isNil)
    return NULL;

  if (_node->right->isNil)
    return _node;
  else
    return findMax_R(_node->right);
}


static void replaceParentChild(TreePtr _tree, treeNodePtr _parent, treeNodePtr _child)
{
  treeNodePtr grandpa;

  assert(_child->parent == _parent || _child->isNil);
  grandpa = _parent->parent;
  
  if (_child == _child->parent->left)
    _parent->left = NULL;
  else
    _parent->right = NULL;
  
  if (grandpa == NULL) {
    _tree->root = _child;
    _child->parent = NULL;
  } else { 
    _child->parent = grandpa;

    if (_parent == grandpa->right)
      grandpa->right = _child;
    else
      grandpa->left = _child;
  }
}


treeNodePtr nextNode(treeNodePtr _node)
{
  assert(_node != NULL && !_node->isNil);
  
  if (!_node->right->isNil)
    _node = findMin_R(_node->right);
  else {
    while (_node->parent != NULL && _node == _node->parent->right)
      _node = _node->parent;
    _node = _node->parent;
  }
  return _node;
}


treeNodePtr getBegin(const TreePtr _tree)
{
  if(treeIsEmpty(_tree))
    return NULL;
  return findMin_R(_tree->root);
}


treeNodePtr getEnd(const TreePtr _tree)
{
  //assert(!treeIsEmpty(_tree));
  return NULL;
}


int treeIsEmpty(const TreePtr _tree)
{
  assert(_tree->root != NULL);
  return _tree->root->isNil;
}


