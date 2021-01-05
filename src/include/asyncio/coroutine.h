#ifdef __cplusplus
extern "C" {
#endif

typedef struct coroutineTy coroutineTy; 

typedef void *pointerTy;
typedef void coroutineProcTy(pointerTy);
typedef void coroutineCbTy(pointerTy);

int coroutineIsMain();
coroutineTy *coroutineCurrent();
int coroutineFinished(coroutineTy *coroutine);
coroutineTy *coroutineNew(coroutineProcTy entry, void *arg, unsigned stackSize);
coroutineTy *coroutineNewWithCb(coroutineProcTy entry, void *arg, unsigned stackSize, coroutineCbTy finishCb, void *finishArg);
void coroutineDelete(coroutineTy *coroutine);
int coroutineCall(coroutineTy *coroutine);
void coroutineYield();

#ifdef __cplusplus
}
#endif
