/* Lua System: Threading */

#ifdef _WIN32

#include <process.h>

#define THREAD_FUNC_RES		unsigned int
#define THREAD_FUNC_API		THREAD_FUNC_RES WINAPI

typedef unsigned int (WINAPI *thread_func_t) (void *);

typedef DWORD 			thread_key_t;
typedef HANDLE			thread_id_t;

#define thread_cancel_syncio(tid) \
	(pCancelSynchronousIo ? !pCancelSynchronousIo(tid) : 0)

#else

#define THREAD_FUNC_RES		void *
#define THREAD_FUNC_API		THREAD_FUNC_RES

typedef void *(*thread_func_t) (void *);

typedef pthread_key_t		thread_key_t;
typedef pthread_t		thread_id_t;

#define thread_cancel_syncio(tid) \
	pthread_kill((tid), SYS_SIGINTR)

#endif /* !WIN32 */


#if defined(__APPLE__) && defined(__MACH__)
#include "thread_affin_mach.c"
#else
#include "thread_affin.c"
#endif

#include "thread_sync.c"


#define THREAD_TYPENAME		"sys.thread"

#define THREAD_XDUP_TAG		"xdup__"

#define THREAD_STACK_SIZE	(64 * 1024)

struct sys_vmthread;

/* Thread's data */
struct sys_thread {
    thread_critsect_t *vmcsp;
    lua_State *L;

    struct sys_vmthread *vmtd;
    struct sys_thread *reftd;  /* parent or fake-child VM-thread */

#ifndef _WIN32
    thread_cond_t cond;
#endif
    thread_id_t tid;
    lua_Integer exit_status;

#define THREAD_SET_TERMINATE	1
#define THREAD_TERMINATED	2
#define THREAD_INTERRUPTED	4
    unsigned int volatile state;
};

/* Main VM-thread's data */
struct sys_vmthread {
    struct sys_thread td;

    thread_critsect_t vmcs;
#ifdef _WIN32
    thread_cond_t cond;
#endif

    unsigned int volatile nref;

    int cpu;  /* bind to processor (inherited by sub-threads) */
    size_t stack_size;  /* for new threads */
};

#define INVALID_TLS_INDEX	(thread_key_t) -1

/* Global Thread Local Storage Index */
static thread_key_t g_TLSIndex = INVALID_TLS_INDEX;

#define THREAD_KEY_ADDRESS	(&g_TLSIndex)

/* Threads table indexes */
#define THREAD_TABLE_EINTR	1  /* Thread Interrupt Error */

#define thread_getvm(td)	(&(td)->vmtd->td)
#define thread_isvm(td)		((td) == thread_getvm(td))

#define thread_unfake(td) \
	(!thread_isvm(td) && (td)->reftd != thread_getvm(td) \
	 ? (td)->reftd : (td))

static void thread_createmeta (lua_State *L);


void
sys_thread_set (struct sys_thread *td)
{
#ifndef _WIN32
    pthread_setspecific(g_TLSIndex, td);
#else
    TlsSetValue(g_TLSIndex, td);
#endif
}

struct sys_thread *
sys_thread_get (void)
{
    if (g_TLSIndex == INVALID_TLS_INDEX)
	return NULL;
#ifndef _WIN32
    return pthread_getspecific(g_TLSIndex);
#else
    return TlsGetValue(g_TLSIndex);
#endif
}

struct lua_State *
sys_thread_tolua (struct sys_thread *td)
{
    return td ? td->L : NULL;
}


static int
thread_waitvm (struct sys_vmthread *vmtd, msec_t timeout)
{
    int res = 0;

    if (vmtd->nref) {
#ifndef _WIN32
	thread_cond_t *condp = &vmtd->td.cond;
#else
	thread_cond_t *condp = &vmtd->cond;
#endif
	sys_vm2_leave(&vmtd->td);
	res = thread_cond_wait_value(condp, &vmtd->vmcs,
	 &vmtd->nref, 0, 0, timeout);
	sys_vm2_enter(&vmtd->td);
    }
    return res;
}

/*
 * Arguments: [status (number)]
 */
static THREAD_FUNC_RES
thread_exit (struct sys_thread *td)
{
    struct sys_thread *reftd = td->reftd;
    const int is_vm = thread_isvm(td);
    lua_Integer res;

    if (td->state != THREAD_TERMINATED) {
	td->state = THREAD_TERMINATED;
	td->exit_status = lua_tointeger(td->L, -1);
    }
    res = td->exit_status;

    if (is_vm) {
	thread_waitvm(td->vmtd, TIMEOUT_INFINITE);
	lua_close(td->L);
    } else {
	struct sys_thread *vmtd = thread_getvm(td);

#ifndef _WIN32
	pthread_cond_broadcast(&td->cond);
#endif
	/* stop collector to prevent self-deadlock on GC */
	lua_gc(vmtd->L, LUA_GCSTOP, 0);
	sys_thread_del(td);
	lua_gc(vmtd->L, LUA_GCRESTART, 0);
	sys_vm2_leave(vmtd);
    }

    /* decrease VM-thread's reference count */
    if (reftd) {
	struct sys_vmthread *vmref = reftd->vmtd;

	sys_vm2_enter(reftd);
	if (is_vm) {
	    reftd->state = THREAD_TERMINATED;
	    reftd->exit_status = res;
#ifndef _WIN32
	    pthread_cond_broadcast(&reftd->cond);
#endif
	    sys_thread_del(reftd);
	}
	if (!--vmref->nref) {
#ifndef _WIN32
	    (void) thread_cond_signal(&vmref->td.cond);
#else
	    (void) thread_cond_signal(&vmref->cond);
#endif
	}
	sys_vm2_leave(reftd);
    }

#ifndef _WIN32
    pthread_exit((THREAD_FUNC_RES) res);
#else
    _endthreadex((THREAD_FUNC_RES) res);
#endif
    return 0;
}

void
sys_thread_yield (const int check_thread)
{
    struct sys_thread *td = sys_thread_get();

    if (td) sys_vm2_leave(td);
#ifndef _WIN32
    sched_yield();
#else
    Sleep(0);
#endif
    if (td) {
	sys_vm2_enter(td);
	if (check_thread) sys_thread_check(td);
    }
}

void
sys_thread_check (struct sys_thread *td)
{
    lua_assert(td);

    {
	const unsigned int state = td->state;

	if (state == THREAD_SET_TERMINATE) {
	    td->state = THREAD_TERMINATED;
	    thread_exit(td);
	} else if (state == THREAD_INTERRUPTED) {
	    lua_rawgetp(td->L, LUA_REGISTRYINDEX, THREAD_KEY_ADDRESS);
	    lua_rawgeti(td->L, -1, THREAD_TABLE_EINTR);
	    lua_error(td->L);
	}
    }
}

void
sys_vm2_enter (struct sys_thread *td)
{
    lua_assert(td);

    thread_critsect_enter(td->vmcsp);
}

void
sys_vm2_leave (struct sys_thread *td)
{
    lua_assert(td);

    thread_critsect_leave(td->vmcsp);
}

void
sys_vm_enter (void)
{
    if (g_TLSIndex != INVALID_TLS_INDEX) {
	struct sys_thread *td = sys_thread_get();

	if (td) {
	    thread_critsect_enter(td->vmcsp);

	    if (td->state) sys_thread_check(td);
	}
    }
}

void
sys_vm_leave (void)
{
    if (g_TLSIndex != INVALID_TLS_INDEX) {
	struct sys_thread *td = sys_thread_get();
	if (td) thread_critsect_leave(td->vmcsp);
    }
}

int
sys_eintr (void)
{
#ifndef _WIN32
    if (SYS_ERRNO == EINTR) {
	struct sys_thread *td = sys_thread_get();
	return !(td && td->state);
    }
#endif
    return 0;
}


#if LUA_VERSION_NUM < 502
static void
thread_setfield (lua_State *L, const char *name, lua_CFunction f)
{
    lua_pushcfunction(L, f);
    lua_setfield(L, -2, name);
}

static void
thread_openlib (lua_State *L, const char *name, lua_CFunction func)
{
    lua_pushcfunction(L, func);
    lua_pushstring(L, name);
    lua_call(L, 1, 0);
}

static void
thread_openlibs (lua_State *L)
{
    thread_openlib(L, "_G", luaopen_base);
    thread_openlib(L, LUA_LOADLIBNAME, luaopen_package);

    lua_getglobal(L, "package");
    lua_getfield(L, -1, "preload");
    thread_setfield(L, LUA_TABLIBNAME, luaopen_table);
    thread_setfield(L, LUA_IOLIBNAME, luaopen_io);
    thread_setfield(L, LUA_OSLIBNAME, luaopen_os);
    thread_setfield(L, LUA_STRLIBNAME, luaopen_string);
    thread_setfield(L, LUA_MATHLIBNAME, luaopen_math);
    thread_setfield(L, LUA_DBLIBNAME, luaopen_debug);
    lua_pop(L, 2);
}
#else
static const luaL_Reg loadedlibs[] = {
    {"_G", luaopen_base},
    {LUA_LOADLIBNAME, luaopen_package},
    {NULL, NULL}
};


static const luaL_Reg preloadedlibs[] = {
    {LUA_COLIBNAME, luaopen_coroutine},
    {LUA_TABLIBNAME, luaopen_table},
    {LUA_IOLIBNAME, luaopen_io},
    {LUA_OSLIBNAME, luaopen_os},
    {LUA_STRLIBNAME, luaopen_string},
    {LUA_BITLIBNAME, luaopen_bit32},
    {LUA_MATHLIBNAME, luaopen_math},
    {LUA_DBLIBNAME, luaopen_debug},
    {NULL, NULL}
};

static void
thread_openlibs (lua_State *L)
{
    const luaL_Reg *lib;
    /* call open functions from 'loadedlibs' and set results to global table */
    for (lib = loadedlibs; lib->func; lib++) {
	luaL_requiref(L, lib->name, lib->func, 1);
	lua_pop(L, 1);  /* remove lib */
    }
    /* add open functions from 'preloadedlibs' into 'package.preload' table */
    luaL_getsubtable(L, LUA_REGISTRYINDEX, "_PRELOAD");
    for (lib = preloadedlibs; lib->func; lib++) {
	lua_pushcfunction(L, lib->func);
	lua_setfield(L, -2, lib->name);
    }
    lua_pop(L, 1);  /* remove _PRELOAD table */
}
#endif

/*
 * Arguments: ..., coroutine, thread_udata
 */
static void
thread_settable (lua_State *L, struct sys_thread *td)
{
    lua_rawgetp(L, LUA_REGISTRYINDEX, THREAD_KEY_ADDRESS);
    lua_insert(L, -3);

    lua_rawsetp(L, -3, td); /* thread_udata */
    lua_rawsetp(L, -2, td->L);  /* coroutine */

    lua_pop(L, 1);
}

static struct sys_thread *
thread_newvm (lua_State *L, struct sys_thread *reftd)
{
    struct sys_vmthread *vmtd;
    lua_State *NL;

    if (L) {
	NL = lua_newthread(L);
	if (!NL) return NULL;
    } else {
	NL = luaL_newstate();
	if (!NL) return NULL;

	L = NL;
	thread_openlibs(L);
	thread_createmeta(L);

	lua_pushthread(L);
    }

    vmtd = lua_newuserdata(L, sizeof(struct sys_vmthread));
    memset(vmtd, 0, sizeof(struct sys_vmthread));
    vmtd->td.L = NL;
    vmtd->td.vmtd = vmtd;
    luaL_getmetatable(L, THREAD_TYPENAME);
    lua_setmetatable(L, -2);

    if (reftd) {
	struct sys_vmthread *vmref = reftd->vmtd;

	vmtd->td.reftd = reftd;
	vmtd->cpu = vmref->cpu;
	vmtd->stack_size = vmref->stack_size;
    }

    if (thread_critsect_new(&vmtd->vmcs))
	return NULL;
    vmtd->td.vmcsp = &vmtd->vmcs;

#ifndef _WIN32
    if (thread_cond_new(&vmtd->td.cond))
#else
    if (thread_cond_new(&vmtd->cond))
#endif
	return NULL;

    thread_settable(L, &vmtd->td);  /* save thread to avoid GC */
    return &vmtd->td;
}

/*
 * Returns: [thread_udata]
 */
struct sys_thread *
sys_thread_new (lua_State *L, struct sys_thread *vmtd,
                struct sys_thread *vmtd2, const int push_udata)
{
    struct sys_vmthread *vmref = vmtd->vmtd;
    struct sys_thread *td;
    lua_State *NL;

    if (vmtd2) {
	NL = vmref->td.L;
	lua_pushnil(L);
    } else {
	NL = lua_newthread(L);
	if (!NL) return NULL;
    }

    td = lua_newuserdata(L, sizeof(struct sys_thread));
    memset(td, 0, sizeof(struct sys_thread));
    td->vmcsp = vmtd->vmcsp;
    td->L = NL;
    td->vmtd = vmref;
    luaL_getmetatable(L, THREAD_TYPENAME);
    lua_setmetatable(L, -2);

    td->reftd = vmtd2 ? vmtd2 : &vmref->td;
    vmref->nref++;

#ifndef _WIN32
    if (thread_cond_new(&td->cond))
	return NULL;
#endif

    if (push_udata) {
	lua_pushvalue(L, -1);
	lua_insert(L, -3);  /* thread_udata */
    }

    thread_settable(L, td);  /* save thread to avoid GC */
    return td;
}

void
sys_thread_del (struct sys_thread *td)
{
    lua_State *L = td->L;

    lua_rawgetp(L, LUA_REGISTRYINDEX, THREAD_KEY_ADDRESS);
    lua_pushnil(L);
    lua_rawsetp(L, -2, td); /* thread_udata */
    lua_pop(L, 1);
}

/*
 * Arguments: thread_udata
 */
static int
thread_done (lua_State *L)
{
    struct sys_thread *td = checkudata(L, 1, THREAD_TYPENAME);

    if (td->L) {
	if (thread_isvm(td)) {
	    thread_critsect_del(td->vmcsp);
#ifdef _WIN32
	    thread_cond_del(&td->vmtd->cond);
#endif
	} else {
	    sys_vm2_leave(td);
#ifndef _WIN32
	    {
		THREAD_FUNC_RES v;
		pthread_join(td->tid, &v);
	    }
#else
	    WaitForSingleObject(td->tid, INFINITE);
	    CloseHandle(td->tid);
#endif
	    sys_vm2_enter(td);
	}
#ifndef _WIN32
	thread_cond_del(&td->cond);
#endif

	lua_rawgetp(L, LUA_REGISTRYINDEX, THREAD_KEY_ADDRESS);
	lua_pushnil(L);
	lua_rawsetp(L, -2, td->L); /* coroutine */
	lua_pop(L, 1);

	td->L = NULL;
    }
    return 0;
}

/*
 * Arguments: [stack_size (number)]
 * Returns: [boolean]
 */
static int
thread_init (lua_State *L)
{
    const size_t stack_size = luaL_optinteger(L, 1, THREAD_STACK_SIZE);
    struct sys_thread *td;

    /* TLS Index */
    if (g_TLSIndex == INVALID_TLS_INDEX) {
#ifndef _WIN32
	const int res = pthread_key_create(&g_TLSIndex, NULL);
	if (res) {
	    errno = res;
	    goto err;
	}
#else
	if ((g_TLSIndex = TlsAlloc()) == INVALID_TLS_INDEX)
	    goto err;
#endif
    }
    /* VM Mutex */
    td = sys_thread_get();
    if (!td) {
	td = thread_newvm(L, NULL);
	if (!td) goto err;

	sys_thread_set(td);
	sys_vm2_enter(td);
    }
    td->vmtd->stack_size = stack_size;

    lua_pushboolean(L, 1);
    return 1;
 err:
    return sys_seterror(L, 0);
}


/*
 * Arguments: ..., error_message (string)
 */
static void
thread_error_abort (lua_State *L)
{
    const char *msg = (lua_type(L, -1) == LUA_TSTRING)
     ? lua_tostring(L, -1) : NULL;

    if (!msg) msg = "(error object is not a string)";
    luai_writestringerror("%s\n", msg);
    abort();
}

/*
 * Arguments: function, [arguments (any) ...]
 */
static THREAD_FUNC_API
thread_start (struct sys_thread *td)
{
    lua_State *L = td->L;

    sys_thread_set(td);
    sys_vm2_enter(td);

    if (lua_pcall(L, lua_gettop(L) - 1, 1, 0))
	thread_error_abort(L);

    return thread_exit(td);
}

static int
thread_create (struct sys_thread *td, const int is_affin)
{
#ifndef _WIN32
    pthread_attr_t attr;
    int res;

    if ((res = pthread_attr_init(&attr)))
	goto err;
    if ((res = pthread_attr_setstacksize(&attr, td->vmtd->stack_size))) {
	pthread_attr_destroy(&attr);
	goto err;
    }

#ifdef USE_MACH_AFFIN
    if (is_affin) {
	res = pthread_create_suspended_np(&td->tid, &attr,
	 (thread_func_t) thread_start, td);
	if (!res) {
	    mach_port_t mt = pthread_mach_thread_np(td->tid);
	    affin_cpu_set(mt, td->vmtd->cpu);
	    thread_resume(mt);
	}
    } else
#endif
    res = pthread_create(&td->tid, &attr, (thread_func_t) thread_start, td);
    pthread_attr_destroy(&attr);
    if (!res) {
#if defined(USE_PTHREAD_AFFIN)
	if (is_affin)
	    affin_cpu_set(td->tid, td->vmtd->cpu);
#else
	(void) is_affin;
#endif
	return 0;
    }
 err:
    errno = res;
#else
    unsigned int tid;
    const unsigned long hThr = _beginthreadex(NULL, td->vmtd->stack_size,
     (thread_func_t) thread_start, td, 0, &tid);

    (void) is_affin;

    if (hThr) {
	td->tid = (HANDLE) hThr;
	if (is_WinNT && td->vmtd->cpu)
	    affin_cpu_set(td->tid, td->vmtd->cpu);
	return 0;
    }
#endif
    return -1;
}

/*
 * Arguments: [bind_cpu (number)], filename (string) | function_dump (string),
 *	[arguments (string | number | boolean | ludata | share_object) ...]
 * Returns: [thread_udata]
 */
static int
thread_runvm (lua_State *L)
{
    const int is_affin = (lua_type(L, 1) == LUA_TNUMBER);
    const int cpu = is_affin ? lua_tointeger(L, 1) : 0;
    const char *path = luaL_checkstring(L, 2);
    struct sys_thread *vmtd = sys_thread_get();
    struct sys_thread *td, *faketd;
    lua_State *NL;

#undef ARG_LAST
#define ARG_LAST	2

    if (!vmtd) luaL_argerror(L, 0, "Threading not initialized");

    td = thread_newvm(NULL, vmtd);
    if (!td) goto err;

    faketd = sys_thread_new(L, vmtd, td, 1);
    if (!faketd) goto err;

    lua_replace(L, 1);  /* fake thread_udata */

    if (is_affin)
	td->vmtd->cpu = cpu;

    NL = td->L;

    /* function */
    if (path[0] == LUA_SIGNATURE[0]
     ? luaL_loadbuffer(NL, path, lua_rawlen(L, ARG_LAST), "thread")
     : luaL_loadfile(NL, path)) {
	lua_pushstring(L, lua_tostring(NL, -1));  /* error message */
	lua_close(NL);
	lua_error(L);
    }

    /* arguments */
    {
	int i, top = lua_gettop(L);

	luaL_checkstack(NL, top + LUA_MINSTACK, "too many arguments");

	for (i = ARG_LAST + 1; i <= top; ++i) {
	    switch (lua_type(L, i)) {
	    case LUA_TSTRING:
		lua_pushstring(NL, lua_tostring(L, i));
		break;
	    case LUA_TNUMBER:
		lua_pushnumber(NL, lua_tonumber(L, i));
		break;
	    case LUA_TBOOLEAN:
		lua_pushboolean(NL, lua_toboolean(L, i));
		break;
	    case LUA_TLIGHTUSERDATA:
		lua_pushlightuserdata(NL, lua_touserdata(L, i));
		break;
	    case LUA_TUSERDATA:
		if (!luaL_getmetafield(L, i, THREAD_XDUP_TAG))
		    luaL_argerror(L, i, "shareable object expected");
		lua_pushvalue(L, i);
		lua_pushlightuserdata(L, NL);
		lua_call(L, 2, 0);
		break;
	    case LUA_TNIL:
		lua_pushnil(NL);
		break;
	    default:
		luaL_argerror(L, i, "primitive type expected");
	    }
	}
    }

    if (!thread_create(td, is_affin)) {
	faketd->tid = td->tid;
	lua_settop(L, 1);
	return 1;
    }
    lua_close(NL);
 err:
    return sys_seterror(L, 0);
}

/*
 * Arguments: function, [arguments (any) ...]
 * Returns: [thread_udata]
 */
static int
thread_run (lua_State *L)
{
    struct sys_thread *td, *vmtd = sys_thread_get();

    if (!vmtd) luaL_argerror(L, 0, "Threading not initialized");
    luaL_checktype(L, 1, LUA_TFUNCTION);

    td = sys_thread_new(L, vmtd, NULL, 1);
    if (!td) goto err;

    lua_insert(L, 1);  /* thread_udata */

    /* function and arguments */
    {
	const int n = lua_gettop(L) - 1;
	luaL_checkstack(td->L, n, NULL);
	lua_xmove(L, td->L, n);
    }

    if (!thread_create(td, 0)) {
	return 1;
    }
    sys_thread_del(td);
 err:
    return sys_seterror(L, 0);
}

/*
 * Returns: thread_udata, is_main (boolean)
 */
static int
thread_self (lua_State *L)
{
    struct sys_thread *td = sys_thread_get();

    if (!td) luaL_argerror(L, 0, "Threading not initialized");

    lua_rawgetp(L, LUA_REGISTRYINDEX, THREAD_KEY_ADDRESS);
    lua_rawgetp(L, -1, td);
    lua_pushboolean(L, thread_isvm(td));
    return 2;
}


static void
sys_thread_sleep (const int msec, const int not_intr)
{
#ifndef _WIN32
    {
	struct timespec req;
	struct timespec *rem = not_intr ? &req : NULL;
	int res;

	req.tv_sec = msec / 1000;
	req.tv_nsec = (msec % 1000) * 1000000;

	do res = nanosleep(&req, rem);
	while (res == -1 && sys_eintr() && not_intr);
    }
#else
    (void) not_intr;

    Sleep(msec);
#endif
}

/*
 * Arguments: milliseconds (number), [don't interrupt (boolean)]
 */
static int
thread_sleep (lua_State *L)
{
    const int msec = lua_tointeger(L, 1);
    const int not_intr = lua_toboolean(L, 2);

    sys_vm_leave();
    sys_thread_sleep(msec, not_intr);
    sys_vm_enter();
    return 0;
}

static int
thread_yield (lua_State *L)
{
    (void) L;

    sys_thread_yield(1);
    return 0;
}

/*
 * Arguments: [object (any)]
 * Returns: old_object (any)
 */
static int
thread_interrupt_error (lua_State *L)
{
    lua_settop(L, 1);
    lua_rawgetp(L, LUA_REGISTRYINDEX, THREAD_KEY_ADDRESS);
    lua_rawgeti(L, -1, THREAD_TABLE_EINTR);

    if (!lua_isnil(L, 1)) {
	lua_pushvalue(L, 1);
	lua_rawseti(L, 2, THREAD_TABLE_EINTR);
    }
    return 1;
}

/*
 * Arguments: thread_udata
 * Returns: boolean
 */
static int
thread_interrupted (lua_State *L)
{
    struct sys_thread *td = checkudata(L, 1, THREAD_TYPENAME);

    td = thread_unfake(td);
    lua_pushboolean(L, (td->state == THREAD_INTERRUPTED));
    return 1;
}

/*
 * Arguments: thread_udata, [recover/interrupt (boolean)]
 */
static int
thread_set_interrupt (lua_State *L)
{
    struct sys_thread *td = checkudata(L, 1, THREAD_TYPENAME);
    const int recover = lua_toboolean(L, 2);

    td = thread_unfake(td);
    if (recover) {
	td->state &= ~THREAD_INTERRUPTED;
	return 0;
    }

    td->state = THREAD_INTERRUPTED;
    if (td == sys_thread_get())
	sys_thread_yield(1);
    else
	(void) thread_cancel_syncio(td->tid);

    return 0;
}

/*
 * Arguments: thread_udata,
 *	[success/failure (boolean) | status (number)]
 */
static int
thread_set_terminate (lua_State *L)
{
    struct sys_thread *td = checkudata(L, 1, THREAD_TYPENAME);
    const lua_Integer status = !lua_isboolean(L, 2) ? lua_tointeger(L, 2)
     : (lua_toboolean(L, 2) ? EXIT_SUCCESS : EXIT_FAILURE);

    td = thread_unfake(td);
    td->exit_status = status;
    td->state = THREAD_SET_TERMINATE;

    if (td == sys_thread_get())
	thread_exit(td);
    else
	(void) thread_cancel_syncio(td->tid);

    return 0;
}

/*
 * Arguments: thread_udata, [timeout (milliseconds)]
 * Returns: [status (number) | timeout (false) | no_workers (true)]
 */
static int
thread_wait (lua_State *L)
{
    struct sys_thread *td = checkudata(L, 1, THREAD_TYPENAME);
    const msec_t timeout = lua_isnoneornil(L, 2)
     ? TIMEOUT_INFINITE : (msec_t) lua_tointeger(L, 2);
    int res;

    if (thread_isvm(td)) {
	res = thread_waitvm(td->vmtd, timeout);
	if (!res) {
	    lua_pushboolean(L, 1);  /* no workers */
	    return 1;
	}
    } else {
	if (td->state == THREAD_TERMINATED)
	    goto result;

	sys_vm_leave();
#ifndef _WIN32
	res = thread_cond_wait_value(&td->cond, td->vmcsp,
	 &td->state, THREAD_TERMINATED, 0, timeout);
#else
	res = thread_handle_wait(td->tid, timeout);
#endif
	sys_vm_enter();
    }

    if (res >= 0) {
	if (res == 1) {
	    lua_pushboolean(L, 0);
	    return 1;  /* timed out */
	}
 result:
	lua_pushinteger(L, td->exit_status);
	return 1;
    }
    return sys_seterror(L, 0);
}

/*
 * Arguments: thread_udata
 * Returns: string
 */
static int
thread_tostring (lua_State *L)
{
    struct sys_thread *td = checkudata(L, 1, THREAD_TYPENAME);

    lua_pushfstring(L, THREAD_TYPENAME " (%p)", td);
    return 1;
}


#include "thread_dpool.c"
#include "thread_pipe.c"
#include "thread_sched.c"


static luaL_Reg thread_meth[] = {
    {"interrupted",	thread_interrupted},
    {"interrupt",	thread_set_interrupt},
    {"terminate",	thread_set_terminate},
    {"wait",		thread_wait},
    {"__tostring",	thread_tostring},
    {"__gc",		thread_done},
    {NULL, NULL}
};

static luaL_Reg thread_lib[] = {
    {"init",		thread_init},
    {"runvm",		thread_runvm},
    {"run",		thread_run},
    {"self",		thread_self},
    {"sleep",		thread_sleep},
    {"yield",		thread_yield},
    {"interrupt_error",	thread_interrupt_error},
    AFFIN_METHODS,
    DPOOL_METHODS,
    PIPE_METHODS,
    SCHED_METHODS,
    {NULL, NULL}
};


static void
thread_createmeta (lua_State *L)
{
    const struct meta_s {
	const char *tname;
	luaL_Reg *meth;
    } meta[] = {
	{THREAD_TYPENAME,	thread_meth},
	{DPOOL_TYPENAME,	dpool_meth},
	{PIPE_TYPENAME,		pipe_meth},
	{SCHED_TYPENAME,	sched_meth},
    };
    int i;

    /* already created? */
    luaL_getmetatable(L, THREAD_TYPENAME);
    {
	const int created = !lua_isnil(L, -1);
	lua_pop(L, 1);
	if (created) return;
    }

    for (i = 0; i < (int) (sizeof(meta) / sizeof(struct meta_s)); ++i) {
	luaL_newmetatable(L, meta[i].tname);
	lua_pushvalue(L, -1);  /* push metatable */
	lua_setfield(L, -2, "__index");  /* metatable.__index = metatable */
	luaL_setfuncs(L, meta[i].meth, 0);
	lua_pop(L, 1);
    }

    /* create threads table */
    lua_newtable(L);
    lua_pushlightuserdata(L, THREAD_KEY_ADDRESS);  /* Thread Interrupt Error */
    lua_rawseti(L, -2, THREAD_TABLE_EINTR);
    lua_rawsetp(L, LUA_REGISTRYINDEX, THREAD_KEY_ADDRESS);
}

/*
 * Arguments: ..., sys_lib (table)
 */
static void
luaopen_sys_thread (lua_State *L)
{
    luaL_newlib(L, thread_lib);
    lua_setfield(L, -2, "thread");

    thread_createmeta(L);
}
