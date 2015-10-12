#ifdef __cplusplus
extern "C" {
#endif

  typedef int SocketID;

  typedef struct descrStruct {
    SocketID sock_;
    void *data;
  } descrStruct;

  enum bool_value {
    _false = 0,
    _true
  };

  typedef enum deleteMsg {
    NOT_FOUND = 0,
    DELETING_OK
  } deleteMsg;

  typedef enum nodeColor {
    Red = 0,
    Black
  } nodeColor;

  typedef struct treeNode treeNode;
  typedef treeNode *treeNodePtr;
  typedef struct Tree Tree;
  typedef Tree *TreePtr;
  typedef int (*cmp)(void*, void*);

  struct treeNode {
    descrStruct keyValue;
    nodeColor color;
    char isNil;
    treeNodePtr left, right, parent;
  };

  struct Tree {
    treeNodePtr root;
    unsigned size;
  };

  treeNodePtr findNode(const TreePtr _tree, descrStruct _keyvalue);
  descrStruct findMinKeyValue(const TreePtr _tree);
  descrStruct findMaxKeyValue(const TreePtr _tree);
  void insertValue(TreePtr _tree, descrStruct _keyValue);
  deleteMsg deleteValue(TreePtr _tree, descrStruct _keyvalue);
  void initTree(TreePtr _tree);
  void freeTree(TreePtr _tree);
  int treeIsEmpty(const TreePtr _tree);
  treeNodePtr nextNode(treeNodePtr);
  treeNodePtr getBegin(const TreePtr _tree);
  treeNodePtr getEnd(const TreePtr _tree);

#ifdef __cplusplus
}
#endif
