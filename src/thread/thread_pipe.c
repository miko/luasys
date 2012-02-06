/* Lua System: Threading: Pipes (VM-threads IPC) */

#define PIPE_TYPENAME	"sys.thread.pipe"

#define MSG_MAXSIZE		512
#define MSG_ITEM_ALIGN		4

struct message_item {
    int type: 8;  /* lua type */
    int len: 16;  /* length of value in bytes */
    union {
	lua_Number num;
	char boolean;
	void *ptr;
	char str[1];
    } v;
};

struct message {
    unsigned short size;  /* size of message in bytes */
    char items[MSG_MAXSIZE];  /* array of message items */
};

struct pipe_buf {
    unsigned int begin, end;  /* buffer indexes */
    unsigned int len;  /* size of buffer */

    struct pipe_buf *next_buf;  /* circular list */
};

#define PIPE_BUF_SENTINEL_SIZE	sizeof(unsigned short)
#define PIPE_BUF_MINSIZE	(8U * MSG_MAXSIZE)
#define PIPE_BUF_MAXSIZE	(2U * 1024 * 1024 * 1024)

struct pipe {
    thread_critsect_t cs;  /* guard access to pipe */
    thread_cond_t put_cond, get_cond;

    unsigned int volatile nmsg;  /* number of messages */

    struct pipe_buf * volatile rbuf;
    struct pipe_buf * volatile wbuf;

    unsigned int volatile signal_on_put;
    unsigned int volatile signal_on_get;

    unsigned int volatile buf_size;
    unsigned int buf_max_size;

    unsigned int volatile nref;
};

#define pipe_buf_ptr(pb)	(char *) ((struct pipe_buf *) pb + 1)
#define pipe_critsect_ptr(pp)	(&pp->cs)


/*
 * Arguments: [buffer_max_size (number), buffer_min_size (number)]
 * Returns: [pipe_udata]
 */
static int
pipe_new (lua_State *L)
{
    const unsigned int max_size = luaL_optunsigned(L, 1, PIPE_BUF_MAXSIZE);
    const unsigned int min_size = luaL_optunsigned(L, 2, PIPE_BUF_MINSIZE);
    struct pipe *pp, **ppp;

    if (min_size > max_size
     || min_size < PIPE_BUF_MINSIZE
     || max_size > PIPE_BUF_MAXSIZE
     /* (max_size / min_size) should be power of 2 */
     || ((max_size / min_size) & (max_size / min_size - 1)) != 0)
	luaL_argerror(L, 1, "invalid size");

    ppp = (struct pipe **) lua_newuserdata(L, sizeof(void *));
    pp = calloc(sizeof(struct pipe), 1);
    if (!pp) goto err;

    *ppp = pp;
    luaL_getmetatable(L, PIPE_TYPENAME);
    lua_setmetatable(L, -2);

    if (thread_critsect_new(&pp->cs)
     || thread_cond_new(&pp->put_cond)
     || thread_cond_new(&pp->get_cond))
	goto err;

    /* allocate initial buffer */
    {
	struct pipe_buf *pb = malloc(min_size);

	if (!pb) goto err;

	memset(pb, 0, sizeof(struct pipe_buf));
	pb->len = min_size - sizeof(struct pipe_buf) - PIPE_BUF_SENTINEL_SIZE;
	pb->next_buf = pb;

	pp->rbuf = pp->wbuf = pb;
	pp->buf_size = min_size;
    }
    pp->buf_max_size = max_size;

    return 1;
 err:
    return sys_seterror(L, 0);
}

/*
 * Returns: pipe_udata, dest. thread (ludata)
 */
static int
pipe_xdup (lua_State *L)
{
    struct pipe *pp = lua_unboxpointer(L, 1, PIPE_TYPENAME);
    lua_State *L2 = (lua_State *) lua_touserdata(L, 2);
    thread_critsect_t *csp = pipe_critsect_ptr(pp);

    if (!L2) luaL_argerror(L, 2, "VM-thread expected");

    lua_boxpointer(L2, pp);
    luaL_getmetatable(L2, PIPE_TYPENAME);
    lua_setmetatable(L2, -2);

    thread_critsect_enter(csp);
    pp->nref++;
    thread_critsect_leave(csp);
    return 0;
}

/*
 * Arguments: pipe_udata
 */
static int
pipe_close (lua_State *L)
{
    struct pipe **ppp = (struct pipe **) checkudata(L, 1, PIPE_TYPENAME);

    if (*ppp) {
	struct pipe *pp = *ppp;
	thread_critsect_t *csp = pipe_critsect_ptr(pp);
	int nref;

	thread_critsect_enter(csp);
	nref = pp->nref--;
	thread_critsect_leave(csp);

	if (!nref) {
	    thread_critsect_del(&pp->cs);
	    thread_cond_del(&pp->put_cond);
	    thread_cond_del(&pp->get_cond);

	    /* deallocate buffers */
	    if (pp->rbuf) {
		struct pipe_buf *rpb = pp->rbuf, *wpb = pp->wbuf;
		do {
		    struct pipe_buf *pb = rpb->next_buf;
		    free(rpb);
		    rpb = pb;
		} while (rpb != wpb);
	    }
	    free(pp);
	}
	*ppp = NULL;
    }
    return 0;
}

/*
 * Arguments: ..., message_items (any) ...
 */
static void
pipe_msg_build (lua_State *L, struct message *msg, int idx)
{
    char *cp = msg->items;
    char *endp = cp + MSG_MAXSIZE - MSG_ITEM_ALIGN;
    const int top = lua_gettop(L);

    for (; idx <= top; ++idx) {
	struct message_item *item = (struct message_item *) cp;
	const int type = lua_type(L, idx);
	const char *s = NULL;
	size_t len = sizeof(item->v);

	cp += offsetof(struct message_item, v);
	if (type == LUA_TSTRING)
	    s = lua_tolstring(L, idx, &len);

	if (cp + len >= endp)
	    luaL_argerror(L, idx, "message is too big");

	switch (type) {
	case LUA_TSTRING:
	    memcpy(&item->v, s, len);
	    break;
	case LUA_TNUMBER:
	    item->v.num = lua_tonumber(L, idx);
	    len = sizeof(item->v.num);
	    break;
	case LUA_TBOOLEAN:
	    item->v.boolean = (char) lua_toboolean(L, idx);
	    len = sizeof(item->v.boolean);
	    break;
	case LUA_TNIL:
	    len = 0;
	    break;
	case LUA_TLIGHTUSERDATA:
	case LUA_TUSERDATA:
	    item->v.ptr = lua_touserdata(L, idx);
	    len = sizeof(item->v.ptr);
	    break;
	default:
	    luaL_argerror(L, idx, "primitive type expected");
	}
	item->type = type;
	item->len = len;
	cp += (len + (MSG_ITEM_ALIGN-1)) & ~(MSG_ITEM_ALIGN-1);
    }
    msg->size = offsetof(struct message, items) + cp - msg->items;
}

/*
 * Returns: message_items (any) ...
 */
static int
pipe_msg_parse (lua_State *L, struct message *msg)
{
    char *cp = msg->items;
    char *endp = (char *) msg + msg->size;
    int i, stack_checked = 0;

    for (i = 1; cp < endp; ++i) {
	struct message_item *item = (struct message_item *) cp;
	const int len = item->len;

	if (!stack_checked--) {
	    stack_checked = 16;
	    luaL_checkstack(L, stack_checked, "too large message");
	}

	switch (item->type) {
	case LUA_TSTRING:
	    lua_pushlstring(L, (char *) &item->v, len);
	    break;
	case LUA_TNUMBER:
	    lua_pushnumber(L, item->v.num);
	    break;
	case LUA_TBOOLEAN:
	    lua_pushboolean(L, item->v.boolean);
	    break;
	case LUA_TNIL:
	    lua_pushnil(L);
	    break;
	default:
	    lua_pushlightuserdata(L, item->v.ptr);
	}
	cp += offsetof(struct message_item, v);
	cp += (len + (MSG_ITEM_ALIGN-1)) & ~(MSG_ITEM_ALIGN-1);
    }
    return i - 1;
}


static int
pipe_cond_wait (thread_cond_t *condp, thread_critsect_t *csp,
                const msec_t timeout)
{
    int res;

    sys_vm_leave();
#ifndef _WIN32
    res = thread_cond_wait_nolock(condp, csp, timeout);
#else
    thread_critsect_leave(csp);
    res = thread_cond_wait(*condp, timeout);
    thread_critsect_enter(csp);
#endif
    sys_vm_enter();
    return res;
}

/*
 * Arguments: pipe_udata, message_items (any) ...
 * Returns: [pipe_udata]
 */
static int
pipe_put (lua_State *L)
{
    struct pipe *pp = lua_unboxpointer(L, 1, PIPE_TYPENAME);
    thread_critsect_t *csp = pipe_critsect_ptr(pp);
    struct message msg;

    pipe_msg_build(L, &msg, 2);  /* construct the message */

    /* write message to buffer */
    thread_critsect_enter(csp);
    for (; ; ) {
	struct pipe_buf *pb = pp->wbuf;
	struct pipe_buf buf = *pb;
	const int wrapped = (buf.end < buf.begin);
	const unsigned int len = (wrapped ? buf.begin : buf.len) - buf.end;

	if (msg.size > len) {
	    if (!wrapped && buf.begin > msg.size) {
		/* wrap the buffer */
		struct message *mp = (struct message *) (pipe_buf_ptr(pb) + buf.end);
		mp->size = 0;  /* sentinel tag */
		buf.end = 0;
	    } else if (pb != buf.next_buf
	     && !buf.next_buf->begin && !buf.next_buf->end) {
		/* use next free buffer */
		pb = pp->wbuf = buf.next_buf;
		buf = *pb;
	    } else {
		const unsigned int buf_size = pp->buf_size;
		struct pipe_buf *wpb = (2 * buf_size <= pp->buf_max_size)
		 ? malloc(buf_size) : NULL;

		if (wpb) {
		    /* allocate new buffer */
		    buf.begin = buf.end = 0;
		    buf.len = buf_size - sizeof(struct pipe_buf) - PIPE_BUF_SENTINEL_SIZE;
		    /* buf->next_buf is already correct */
		    pb->next_buf = wpb;

		    pp->buf_size = buf_size * 2;
		    pp->wbuf = wpb;
		    pb = wpb;
		} else {
		    /* wait `get' signal */
		    int res;

		    pp->signal_on_get++;
		    res = pipe_cond_wait(&pp->get_cond, csp, TIMEOUT_INFINITE);

		    if (--pp->signal_on_get) {
			(void) thread_cond_signal(&pp->get_cond);
		    }
		    if (!res) continue;
		    thread_critsect_leave(csp);

		    return sys_seterror(L, 0);
		}
	    }
	}

	memcpy(pipe_buf_ptr(pb) + buf.end, &msg, msg.size);
	buf.end += msg.size;
	*pb = buf;
	pp->nmsg++;
	break;
    }
    if (pp->signal_on_put) {
	(void) thread_cond_signal(&pp->put_cond);
    }
    thread_critsect_leave(csp);

    lua_settop(L, 1);
    return 1;
}

/*
 * Arguments: pipe_udata, [timeout (milliseconds)]
 * Returns: [message_items (any) ... | timedout (false)]
 */
static int
pipe_get (lua_State *L)
{
    struct pipe *pp = lua_unboxpointer(L, 1, PIPE_TYPENAME);
    const msec_t timeout = lua_isnoneornil(L, 2)
     ? TIMEOUT_INFINITE : (msec_t) lua_tointeger(L, 2);
    thread_critsect_t *csp = pipe_critsect_ptr(pp);
    struct message msg;

    /* read message from buffer */
    thread_critsect_enter(csp);
    for (; ; ) {
	if (pp->nmsg) {
	    struct pipe_buf *pb = pp->rbuf;
	    struct pipe_buf buf = *pb;
	    struct message *mp = (struct message *) (pipe_buf_ptr(pb) + buf.begin);

	    if (!mp->size) {  /* buffer is wrapped */
		mp = (struct message *) pipe_buf_ptr(pb);
		buf.begin = 0;
	    }

	    memcpy(&msg, mp, mp->size);
	    buf.begin += mp->size;
	    if (buf.begin == buf.end) {
		buf.begin = buf.end = 0;
		if (pp->nmsg > 1)
		    pp->rbuf = buf.next_buf;
	    } else if (buf.begin == buf.len) {
		buf.begin = 0;
	    }
	    *pb = buf;
	    if (--pp->nmsg && pp->signal_on_put) {
		(void) thread_cond_signal(&pp->put_cond);
	    }
	} else {
	    /* wait `put' signal */
	    int res;

	    pp->signal_on_put++;
	    res = pipe_cond_wait(&pp->put_cond, csp, timeout);
	    pp->signal_on_put--;

	    if (!res) continue;
	    thread_critsect_leave(csp);

	    if (res == 1) {
		lua_pushboolean(L, 0);
		return 1;  /* timed out */
	    }
	    return sys_seterror(L, 0);
	}
	break;
    }
    if (pp->signal_on_get) {
	(void) thread_cond_signal(&pp->get_cond);
    }
    thread_critsect_leave(csp);

    return pipe_msg_parse(L, &msg);  /* deconstruct the message */
}

/*
 * Arguments: pipe_udata
 * Returns: [number]
 */
static int
pipe_count (lua_State *L)
{
    struct pipe *pp = lua_unboxpointer(L, 1, PIPE_TYPENAME);
    thread_critsect_t *csp = pipe_critsect_ptr(pp);
    unsigned int nmsg;

    thread_critsect_enter(csp);
    nmsg = pp->nmsg;
    thread_critsect_leave(csp);

    lua_pushunsigned(L, nmsg);
    return 1;
}

/*
 * Arguments: pipe_udata
 * Returns: string
 */
static int
pipe_tostring (lua_State *L)
{
    struct pipe *pp = lua_unboxpointer(L, 1, PIPE_TYPENAME);

    lua_pushfstring(L, PIPE_TYPENAME " (%p)", pp);
    return 1;
}


#define PIPE_METHODS \
    {"pipe",		pipe_new}

static luaL_Reg pipe_meth[] = {
    {THREAD_XDUP_TAG,	pipe_xdup},
    {"put",		pipe_put},
    {"get",		pipe_get},
    {"__len",		pipe_count},
    {"__tostring",	pipe_tostring},
    {"__gc",		pipe_close},
    {NULL, NULL}
};
