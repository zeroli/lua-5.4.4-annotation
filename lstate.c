/*
** $Id: lstate.c $
** Global State
** See Copyright Notice in lua.h
*/

#define lstate_c
#define LUA_CORE

#include "lprefix.h"


#include <stddef.h>
#include <string.h>

#include "lua.h"

#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "llex.h"
#include "lmem.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"



/*
** thread state + extra space
*/
typedef struct LX {
  lu_byte extra_[LUA_EXTRASPACE];  // TODO：这个是干什么呢？align？？
  lua_State l;
} LX;


/*
** Main thread combines a thread state and the global state
*/
typedef struct LG {
  LX l;
  global_State g;
} LG;


// 已知一个L，便可以依据它在LX中的offset获取LX的指针
#define fromstate(L)	(cast(LX *, cast(lu_byte *, (L)) - offsetof(LX, l)))


/*
** A macro to create a "random" seed when a state is created;
** the seed is used to randomize string hashes.
*/
#if !defined(luai_makeseed)

#include <time.h>

/*
** Compute an initial seed with some level of randomness.
** Rely on Address Space Layout Randomization (if present) and
** current time.
*/
#define addbuff(b,p,e) \
  { size_t t = cast_sizet(e); \
    memcpy(b + p, &t, sizeof(t)); p += sizeof(t); }

// 这种生成seed的方法，比较特别
// local变量h仍然是依据当前时间点
// 而且代码更改，L和全局函数地址也会变
// 生成的hash作为种子也会变
static unsigned int luai_makeseed (lua_State *L) {
  char buff[3 * sizeof(size_t)];
  unsigned int h = cast_uint(time(NULL));
  int p = 0;
  addbuff(buff, p, L);  /* heap variable */
  addbuff(buff, p, &h);  /* local variable */
  addbuff(buff, p, &lua_newstate);  /* public function */
  lua_assert(p == sizeof(buff));
  return luaS_hash(buff, p, h);
}

#endif


/*
** set GCdebt to a new value keeping the value (totalbytes + GCdebt)
** invariant (and avoiding underflows in 'totalbytes')
*/
// TODO: 这个函数要做什么？
void luaE_setdebt (global_State *g, l_mem debt) {
  l_mem tb = gettotalbytes(g);
  lua_assert(tb > 0);
  if (debt < tb - MAX_LMEM)
    debt = tb - MAX_LMEM;  /* will make 'totalbytes == MAX_LMEM' */
  g->totalbytes = tb - debt;
  g->GCdebt = debt;
}


LUA_API int lua_setcstacklimit (lua_State *L, unsigned int limit) {
  UNUSED(L); UNUSED(limit);
  return LUAI_MAXCCALLS;  /* warning?? */
}

// 创建一个新的CI，连接到当前CI的后面
CallInfo *luaE_extendCI (lua_State *L) {
  CallInfo *ci;
  lua_assert(L->ci->next == NULL);
  ci = luaM_new(L, CallInfo);
  lua_assert(L->ci->next == NULL);
  L->ci->next = ci;
  ci->previous = L->ci;
  ci->next = NULL;
  ci->u.l.trap = 0;
  L->nci++;
  return ci;
}


/*
** free all CallInfo structures not in use by a thread
*/
void luaE_freeCI (lua_State *L) {
  CallInfo *ci = L->ci;
  CallInfo *next = ci->next;
  ci->next = NULL;
  // 销毁所有当前CI后面的CI结构
  while ((ci = next) != NULL) {
    next = ci->next;
    luaM_free(L, ci);
    L->nci--;
  }
}


/*
** free half of the CallInfo structures not in use by a thread,
** keeping the first one.
*/
void luaE_shrinkCI (lua_State *L) {
  CallInfo *ci = L->ci->next;  /* first free CallInfo */
  CallInfo *next;
  if (ci == NULL)
    return;  /* no extra elements */
  // 从ci的下一个节点开始，每隔一个节点，就删除一个节点，达到只删除一半节点的目的
  while ((next = ci->next) != NULL) {  /* two extra elements? */
    CallInfo *next2 = next->next;  /* next's next */
    ci->next = next2;  /* remove next from the list */
    L->nci--;
    luaM_free(L, next);  /* free next */
    if (next2 == NULL)
      break;  /* no more elements */
    else {
      next2->previous = ci;
      ci = next2;  /* continue */
    }
  }
}


/*
** Called when 'getCcalls(L)' larger or equal to LUAI_MAXCCALLS.
** If equal, raises an overflow error. If value is larger than
** LUAI_MAXCCALLS (which means it is handling an overflow) but
** not much larger, does not report an error (to allow overflow
** handling to work).
*/
void luaE_checkcstack (lua_State *L) {
  // LUAI_MAXCCALLS = 200
  if (getCcalls(L) == LUAI_MAXCCALLS)
    luaG_runerror(L, "C stack overflow");
  else if (getCcalls(L) >= (LUAI_MAXCCALLS / 10 * 11))
    luaD_throw(L, LUA_ERRERR);  /* error while handling stack error */
}


LUAI_FUNC void luaE_incCstack (lua_State *L) {
  L->nCcalls++;
  if (l_unlikely(getCcalls(L) >= LUAI_MAXCCALLS))
    luaE_checkcstack(L);
}


// 对虚拟栈进行初始化，这个函数比较重要，对理解虚拟栈的布局比较关键
static void stack_init (lua_State *L1, lua_State *L) {
  int i; CallInfo *ci;
  /* initialize stack array */
  // 总共有45个栈元素，栈元素类型为`StackValue`
  // `StkId`是`StackValue`类型指针
  L1->stack = luaM_newvector(L, BASIC_STACK_SIZE + EXTRA_STACK, StackValue);
  L1->tbclist = L1->stack;
  // 初始化stack元素的值为NIL
  for (i = 0; i < BASIC_STACK_SIZE + EXTRA_STACK; i++)
    setnilvalue(s2v(L1->stack + i));  /* erase new stack */
  // top为下一个位置，当前指向栈底，代表没有元素
  L1->top = L1->stack;
  // stack末尾，beyond the end
  L1->stack_last = L1->stack + BASIC_STACK_SIZE;
  /* initialize first ci */
  // call info是一个双向链表
  ci = &L1->base_ci;
  ci->next = ci->previous = NULL;
  ci->callstatus = CIST_C;
  ci->func = L1->top;
  ci->u.c.k = NULL;
  ci->nresults = 0;
  setnilvalue(s2v(L1->top));  /* 'function' entry for this 'ci' */
  L1->top++;  // increase top了！！！
  ci->top = L1->top + LUA_MINSTACK;  // 第一个函数调用可以用20个stack元素
  // 当前state的ci指向第一个call info
  L1->ci = ci;
}


static void freestack (lua_State *L) {
  if (L->stack == NULL)
    return;  /* stack not completely built yet */
  L->ci = &L->base_ci;  /* free the entire 'ci' list */
  luaE_freeCI(L);
  lua_assert(L->nci == 0);
  luaM_freearray(L, L->stack, stacksize(L) + EXTRA_STACK);  /* free stack */
}


/*
** Create registry table and its predefined values
*/
static void init_registry (lua_State *L, global_State *g) {
  /* create registry */
  // registry其实一个hash table
  Table *registry = luaH_new(L);
  // *** TODO：需要好好研究，当前可以理解为全局registry被设置为registry
  // 注意：g->l_registry是TValue类型，通用类型，registry是具体的Table类型
  sethvalue(L, &g->l_registry, registry);
  // 现在注册表里只有2项，一个是主线程state，一个是全局符号表
  luaH_resize(L, registry, LUA_RIDX_LAST, 0);
  // 下面，table当前一个数组来用
  /* registry[LUA_RIDX_MAINTHREAD] = L */
  // 主线程作为第1项放在注册表
  setthvalue(L, &registry->array[LUA_RIDX_MAINTHREAD - 1], L);
  /* registry[LUA_RIDX_GLOBALS] = new table (table of globals) */
  // 这里，全局符号表创建，放在了registry表里面第2项
  sethvalue(L, &registry->array[LUA_RIDX_GLOBALS - 1], luaH_new(L));
}


/*
** open parts of the state that may cause memory-allocation errors.
*/
static void f_luaopen (lua_State *L, void *ud) {
  global_State *g = G(L);  // 之前设置过L->G到全局的state
  UNUSED(ud);
  // 初始化虚拟栈
  stack_init(L, L);  /* init stack */
  // 初始化注册表
  init_registry(L, g);
  // 初始化字符串表hashtable和字符串缓存
  luaS_init(L);
  // tag method表初始化
  luaT_init(L);
  // Lexer的keyword字符串初始化
  luaX_init(L);
  // gc stop = 0
  g->gcstp = 0;  /* allow gc */
  setnilvalue(&g->nilvalue);  /* now state is complete */
  luai_userstateopen(L);
}


/*
** preinitialize a thread with consistent values without allocating
** any memory (to avoid errors)
*/
static void preinit_thread (lua_State *L, global_State *g) {
  G(L) = g;  // OK，这行将每个L关联全局的state了
  L->stack = NULL;
  L->ci = NULL;
  L->nci = 0;
  L->twups = L;  /* thread has no upvalues */
  L->nCcalls = 0;
  L->errorJmp = NULL;
  L->hook = NULL;
  L->hookmask = 0;
  L->basehookcount = 0;
  L->allowhook = 1;
  resethookcount(L);
  L->openupval = NULL;
  L->status = LUA_OK;
  L->errfunc = 0;
  L->oldpc = 0;
}


static void close_state (lua_State *L) {
  global_State *g = G(L);
  if (!completestate(g))  /* closing a partially built state? */
    luaC_freeallobjects(L);  /* just collect its objects */
  else {  /* closing a fully built state */
    L->ci = &L->base_ci;  /* unwind CallInfo list */
    luaD_closeprotected(L, 1, LUA_OK);  /* close all upvalues */
    luaC_freeallobjects(L);  /* collect all objects */
    luai_userstateclose(L);
  }
  luaM_freearray(L, G(L)->strt.hash, G(L)->strt.size);
  freestack(L);
  lua_assert(gettotalbytes(g) == sizeof(LG));
  // 释放最后的LG对象，包括当前L和全局state
  // 从L指针可以根据它在LG中的offset，求得LG指针
  (*g->frealloc)(g->ud, fromstate(L), sizeof(LG), 0);  /* free main block */
}


// 从一个线程创建另一个线程状态
LUA_API lua_State *lua_newthread (lua_State *L) {
  global_State *g;
  lua_State *L1;
  lua_lock(L);
  g = G(L);  // 进程全局state
  luaC_checkGC(L);
  /* create new thread */
  // 创建一个新线程状态，只创建LX，不需要进程状态
  L1 = &cast(LX *, luaM_newobject(L, LUA_TTHREAD, sizeof(LX)))->l;
  L1->marked = luaC_white(g);
  L1->tt = LUA_VTHREAD;  // 线程对象
  /* link it on list 'allgc' */
  L1->next = g->allgc;  // 链到所有gc对象链表头，单链表
  g->allgc = obj2gco(L1);
  /* anchor it on L stack */
  setthvalue2s(L, L->top, L1);
  api_incr_top(L);
  // 预初始化这个新的线程对象
  preinit_thread(L1, g);
  L1->hookmask = L->hookmask;
  L1->basehookcount = L->basehookcount;
  L1->hook = L->hook;
  resethookcount(L1);
  /* initialize L1 extra space */
  // TODO: 这个操作是在干嘛？？
  memcpy(lua_getextraspace(L1), lua_getextraspace(g->mainthread),
         LUA_EXTRASPACE);
  luai_userstatethread(L, L1);
  stack_init(L1, L);  /* init stack */
  lua_unlock(L);
  return L1;
}


void luaE_freethread (lua_State *L, lua_State *L1) {
  LX *l = fromstate(L1);
  luaF_closeupval(L1, L1->stack);  /* close all upvalues */
  lua_assert(L1->openupval == NULL);
  luai_userstatefree(L, L1);
  freestack(L1);
  luaM_free(L, l);
}


int luaE_resetthread (lua_State *L, int status) {
  CallInfo *ci = L->ci = &L->base_ci;  /* unwind CallInfo list */
  setnilvalue(s2v(L->stack));  /* 'function' entry for basic 'ci' */
  ci->func = L->stack;
  ci->callstatus = CIST_C;
  if (status == LUA_YIELD)
    status = LUA_OK;
  L->status = LUA_OK;  /* so it can run __close metamethods */
  status = luaD_closeprotected(L, 1, status);
  if (status != LUA_OK)  /* errors? */
    luaD_seterrorobj(L, status, L->stack + 1);
  else
    L->top = L->stack + 1;
  ci->top = L->top + LUA_MINSTACK; // 第一个函数调用可以用20个stack元素
  luaD_reallocstack(L, cast_int(ci->top - L->stack), 0);
  return status;
}


LUA_API int lua_resetthread (lua_State *L) {
  int status;
  lua_lock(L);
  status = luaE_resetthread(L, L->status);
  lua_unlock(L);
  return status;
}


// OK，我们来看下，new一个lua_State会发生什么？
LUA_API lua_State *lua_newstate (lua_Alloc f, void *ud) {
  int i;
  lua_State *L;
  global_State *g;  // OK，这就是全局的state！！！
  // 就是分配一个LG对象，LG对象简单包装全局state
  /*
    typedef struct LG {
    LX l;
    global_State g;
  } LG;
  */
  LG *l = cast(LG *, (*f)(ud, NULL, LUA_TTHREAD, sizeof(LG)));
  if (l == NULL) return NULL;
  L = &l->l.l;  // 系统第一个线程lua_State
  g = &l->g;  // OK, 这里有了全局state对象指针了
  L->tt = LUA_VTHREAD;  // 设置为线程对象
  // **** TODO： 这是在干什么，GC相关么？
  g->currentwhite = bitmask(WHITE0BIT);
  L->marked = luaC_white(g);

  preinit_thread(L, g);
  // 链接这个线程对象，可回收对象到gc链表头
  g->allgc = obj2gco(L);  /* by now, only object is the main thread */
  L->next = NULL;

  incnny(L);  /* main thread is always non yieldable */
  g->frealloc = f;  // 用户提供的alloc分配函数作为全局性的
  g->ud = ud;  // 用户数据，当前为null
  g->warnf = NULL;
  g->ud_warn = NULL;
  // 全局state有一个主线程lua_State
  g->mainthread = L;
  g->seed = luai_makeseed(L);
  g->gcstp = GCSTPGC;  /* no GC while building state */
  // 这是在创建全局inplace string表，就是一个hash表
  g->strt.size = g->strt.nuse = 0;
  g->strt.hash = NULL;

  // 全局注册表？？
  setnilvalue(&g->l_registry);
  g->panic = NULL;
  g->gcstate = GCSpause;
  g->gckind = KGC_INC;
  g->gcstopem = 0;
  g->gcemergency = 0;
  g->finobj = g->tobefnz = g->fixedgc = NULL;
  g->firstold1 = g->survival = g->old1 = g->reallyold = NULL;
  g->finobjsur = g->finobjold1 = g->finobjrold = NULL;
  g->sweepgc = NULL;
  g->gray = g->grayagain = NULL;
  g->weak = g->ephemeron = g->allweak = NULL;
  g->twups = NULL;
  g->totalbytes = sizeof(LG); // 统计内存字节数
  g->GCdebt = 0;
  g->lastatomic = 0;
  setivalue(&g->nilvalue, 0);  /* to signal that state is not yet built */
  setgcparam(g->gcpause, LUAI_GCPAUSE);
  setgcparam(g->gcstepmul, LUAI_GCMUL);
  g->gcstepsize = LUAI_GCSTEPSIZE;
  setgcparam(g->genmajormul, LUAI_GENMAJORMUL);
  g->genminormul = LUAI_GENMINORMUL;
  for (i=0; i < LUA_NUMTAGS; i++) g->mt[i] = NULL;
  // 调用`f_luaopen`？？
  if (luaD_rawrunprotected(L, f_luaopen, NULL) != LUA_OK) {
    /* memory allocation error: free partial state */
    close_state(L);
    L = NULL;
  }
  return L;
}


LUA_API void lua_close (lua_State *L) {
  lua_lock(L);
  L = G(L)->mainthread;  /* only the main thread can be closed */
  close_state(L);
}


void luaE_warning (lua_State *L, const char *msg, int tocont) {
  lua_WarnFunction wf = G(L)->warnf;
  if (wf != NULL)
    wf(G(L)->ud_warn, msg, tocont);
}


/*
** Generate a warning from an error message
*/
void luaE_warnerror (lua_State *L, const char *where) {
  TValue *errobj = s2v(L->top - 1);  /* error object */
  const char *msg = (ttisstring(errobj))
                  ? svalue(errobj)
                  : "error object is not a string";
  /* produce warning "error in %s (%s)" (where, msg) */
  luaE_warning(L, "error in ", 1);
  luaE_warning(L, where, 1);
  luaE_warning(L, " (", 1);
  luaE_warning(L, msg, 1);
  luaE_warning(L, ")", 0);
}
