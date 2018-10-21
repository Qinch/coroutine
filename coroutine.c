#include "coroutine.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>

#if __APPLE__ && __MACH__
	#include <sys/ucontext.h>
#else 
	#include <ucontext.h>
#endif 

#define STACK_SIZE (1024*1024)
#define DEFAULT_COROUTINE 16

struct coroutine;

struct schedule {
	char stack[STACK_SIZE]; //用于runing co的stack
	ucontext_t main;
	int nco; //n个co(不一定连续分布在co中)
	int cap; //max cap个co
	int running; //当前执行的co
	struct coroutine **co; //all co 
};

/*
  typedef struct coroutine cot;

  cot** co->|--------------|
	    |  cot* co[0]  |
	    |--------------|
	    |  cot* co[1]  |
	    |--------------|
	    |    .....     |
	    |--------------|
	    | cot* co[n-1] |
	    |--------------|
*/

struct coroutine {
	coroutine_func func;
	void *ud; //func的输入参数
	ucontext_t ctx;
	struct schedule * sch; //S
	ptrdiff_t cap;  //max stack 
	ptrdiff_t size; //stack curr size
	int status; //run status
	char *stack; //用于保存co的stack
};

struct coroutine *_co_new(struct schedule *S , coroutine_func func, void *ud) 
{
	struct coroutine * co = malloc(sizeof(*co));
	co->func = func; //func ptr
	co->ud = ud; //input parameter
	co->sch = S; //schedule
	co->cap = 0;  //stack max size
	co->size = 0; //stack curr size
	co->status = COROUTINE_READY; //wait for schedule
	co->stack = NULL;

	return co;
}


//free co's stack
void _co_delete(struct coroutine *co) 
{
	free(co->stack);
	free(co);
}

//create schedule
struct schedule *coroutine_open(void) 
{
	struct schedule *S = malloc(sizeof(*S));
	S->nco = 0; //co的数量
	S->cap = DEFAULT_COROUTINE; //支持co的max num,  ie capability
	S->running = -1;
	S->co = malloc(sizeof(struct coroutine *) * S->cap);
	memset(S->co, 0, sizeof(struct coroutine *) * S->cap);

	return S;
}


//close schedule S
void coroutine_close(struct schedule *S) 
{
	int i;
	for (i=0;i<S->cap;i++) {
		struct coroutine * co = S->co[i];
		if (co) {
			_co_delete(co);
		}
	}
	free(S->co);
	S->co = NULL;
	free(S);
}

//Returns:为co的id
int coroutine_new(struct schedule *S, coroutine_func func, void *ud) 
{
	struct coroutine *co = _co_new(S, func , ud);
	//已经达到最大的cap，cap=cap*2
	if (S->nco >= S->cap) {
		int id = S->cap;
		S->co = realloc(S->co, S->cap * 2 * sizeof(struct coroutine *));
		memset(S->co + S->cap , 0 , sizeof(struct coroutine *) * S->cap);
		S->co[S->cap] = co;
		S->cap *= 2;
		++S->nco;
		return id;
	} else {
		int i;
		//找到第一个空闲的element
		for (i=0;i<S->cap;i++) {
			int id = (i+S->nco) % S->cap;
			if (S->co[id] == NULL) {
				S->co[id] = co;
				++S->nco;
				return id;
			}
		}
	}
	assert(0);
	return -1;
}

static void
mainfunc(uint32_t low32, uint32_t hi32) {
	uintptr_t ptr = (uintptr_t)low32 | ((uintptr_t)hi32 << 32);
	struct schedule *S = (struct schedule *)ptr;
	int id = S->running;
	struct coroutine *C = S->co[id];
	C->func(S,C->ud);
	_co_delete(C);
	S->co[id] = NULL;
	--S->nco;
	S->running = -1;
}

//resume
void coroutine_resume(struct schedule * S, int id) 
{
	assert(S->running == -1);
	assert(id >=0 && id < S->cap);
	struct coroutine *C = S->co[id];
	if (C == NULL)
		return;
	int status = C->status;
	switch(status) {
	//第一次执行
	case COROUTINE_READY:
		getcontext(&C->ctx);
		C->ctx.uc_stack.ss_sp = S->stack;
		C->ctx.uc_stack.ss_size = STACK_SIZE;
		//ctx执行完后，返回的context
		C->ctx.uc_link = &S->main;
		S->running = id;
		C->status = COROUTINE_RUNNING;
		uintptr_t ptr = (uintptr_t)S;
		makecontext(&C->ctx, (void (*)(void)) mainfunc, 2, (uint32_t)ptr, (uint32_t)(ptr>>32));
		//切换到C->ctx
		//set curr context to main, then switch to ctx
		swapcontext(&S->main, &C->ctx);
		break;
	case COROUTINE_SUSPEND:
		//[S->stack+STACK_SIZE, S->stak +STACK_SIZE -C->size] = [ebp, esp]
		memcpy(S->stack + STACK_SIZE - C->size, C->stack, C->size);
		S->running = id;
		C->status = COROUTINE_RUNNING;
		swapcontext(&S->main, &C->ctx);
		break;
	default:
		assert(0);
	}
}

//
static void
_save_stack(struct coroutine *C, char *top) {
	char dummy = 0;
	assert(top - &dummy <= STACK_SIZE);
	//C->stak空间不足
	if (C->cap < top - &dummy) {
		free(C->stack);
		C->cap = top-&dummy;
		C->stack = malloc(C->cap);
	}
	//size为call stack的大小
	C->size = top - &dummy;
	//将[ebp, esp]保存起来
	memcpy(C->stack, &dummy, C->size);
}


//yield
void
coroutine_yield(struct schedule * S) {
	//yield的co的id
	int id = S->running;
	assert(id >= 0);
	struct coroutine * C = S->co[id];
	assert((char *)&C > S->stack);
	//保存当前的调用栈
	_save_stack(C,S->stack + STACK_SIZE);
	C->status = COROUTINE_SUSPEND;
	S->running = -1;
	//切换到schedule co
	swapcontext(&C->ctx , &S->main);
}


//返回co id的status
int 
coroutine_status(struct schedule * S, int id) {
	assert(id>=0 && id < S->cap);
	if (S->co[id] == NULL) {
		return COROUTINE_DEAD;
	}
	return S->co[id]->status;
}

//当前运行的co id
int 
coroutine_running(struct schedule * S) {
	return S->running;
}

