#ifdef __cplusplus
extern "C" {
#endif

typedef struct coroutineTy coroutineTy; 

typedef void *pointerTy;
typedef pointerTy coroutineProcTy(pointerTy);

coroutineTy *coroutineCurrent();
int coroutineFinished(coroutineTy *coroutine);
coroutineTy *coroutineNew(coroutineProcTy entry, void *arg, unsigned stackSize);
void coroutineDelete(coroutineTy *coroutine);
void coroutineCall(coroutineTy *coroutine);
void coroutineYield();

#ifdef __cplusplus
}
#endif