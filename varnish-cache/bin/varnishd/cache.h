/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2010 Redpill Linpro AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $Id: cache.h 4682 2010-04-19 11:38:52Z tfheen $
 */

/*
 * This macro can be used in .h files to isolate bits that the manager
 * should not (need to) see, such as pthread mutexes etc.
 */
#define VARNISH_CACHE_CHILD	1

#include <sys/time.h>
#include <sys/uio.h>
#include <sys/socket.h>

#include <pthread.h>
#ifdef HAVE_PTHREAD_NP_H
#include <pthread_np.h>
#endif
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

#if defined(HAVE_EPOLL_CTL)
#include <sys/epoll.h>
#endif

#include "vqueue.h"

#include "vsb.h"

#include "libvarnish.h"

#include "common.h"
#include "heritage.h"
#include "miniobj.h"

enum {
	/* Fields from the first line of HTTP proto */
	HTTP_HDR_REQ,
	HTTP_HDR_URL,
	HTTP_HDR_PROTO,
	HTTP_HDR_STATUS,
	HTTP_HDR_RESPONSE,
	/* HTTP header lines */
	HTTP_HDR_FIRST,
};

struct cli;
struct vsb;
struct sess;
struct director;
struct object;
struct objhead;
struct objcore;
struct workreq;
struct esidata;
struct vrt_backend;
struct cli_proto;
struct ban;
struct SHA256Context;

struct smp_object;
struct smp_seg;

struct lock { void *priv; };		// Opaque

#define DIGEST_LEN		32


/*--------------------------------------------------------------------
 * Pointer aligment magic
 */

#define PALGN		(sizeof(void *) - 1)
#define PAOK(p)		(((uintptr_t)(p) & PALGN) == 0)
#define PRNDDN(p)	((uintptr_t)(p) & ~PALGN)
#define PRNDUP(p)	(((uintptr_t)(p) + PALGN) & ~PALGN)

/*--------------------------------------------------------------------*/

typedef struct {
	char			*b;
	char			*e;
} txt;

/*--------------------------------------------------------------------*/

enum step {
#define STEP(l, u)	STP_##u,
#include "steps.h"
#undef STEP
};

/*--------------------------------------------------------------------
 * Workspace structure for quick memory allocation.
 */

struct ws {
	unsigned		magic;
#define WS_MAGIC		0x35fac554
	const char		*id;		/* identity */
	char			*s;		/* (S)tart of buffer */
	char			*f;		/* (F)ree pointer */
	char			*r;		/* (R)eserved length */
	char			*e;		/* (E)nd of buffer */
	int			overflow;	/* workspace overflowed */
};

/*--------------------------------------------------------------------
 * HTTP Request/Response/Header handling structure.
 */

enum httpwhence {
	HTTP_Rx	 = 1,
	HTTP_Tx  = 2,
	HTTP_Obj = 3
};

/* NB: remember to update http_Copy() if you add fields */
struct http {
	unsigned		magic;
#define HTTP_MAGIC		0x6428b5c9

	struct ws		*ws;

	unsigned char		conds;		/* If-* headers present */
	enum httpwhence		logtag;
	int			status;
	double			protover;

	unsigned		shd;		/* Size of hd space */
	txt			*hd;
	unsigned char		*hdf;
#define HDF_FILTER		(1 << 0)	/* Filtered by Connection */
#define HDF_COPY		(1 << 1)	/* Copy this field */
	unsigned		nhd;		/* Next free hd */
};

/*--------------------------------------------------------------------
 * HTTP Protocol connection structure
 */

struct http_conn {
	unsigned		magic;
#define HTTP_CONN_MAGIC		0x3e19edd1

	int			fd;
	struct ws		*ws;
	txt			rxbuf;
	txt			pipeline;
};

/*--------------------------------------------------------------------*/

struct acct {
	double			first;
#define ACCT(foo)	uint64_t	foo;
#include "acct_fields.h"
#undef ACCT
};

/*--------------------------------------------------------------------*/

#define L0(n)
#define L1(n)			int n;
#define MAC_STAT(n, t, l, f, e)	L##l(n)
struct dstat {
#include "stat_field.h"
};
#undef MAC_STAT
#undef L0
#undef L1

/*--------------------------------------------------------------------*/

struct worker {
	unsigned		magic;
#define WORKER_MAGIC		0x6391adcf
	struct objhead		*nobjhead;
	struct objcore		*nobjcore;
	struct dstat		stats;

	double			lastused;

	pthread_cond_t		cond;

	VTAILQ_ENTRY(worker)	list;
	struct workreq		*wrq;

	int			*wfd;
	unsigned		werr;	/* valid after WRK_Flush() */
	struct iovec		*iov;
	unsigned		siov;
	unsigned		niov;
	ssize_t			liov;

	struct VCL_conf		*vcl;

	unsigned char		*wlb, *wlp, *wle;
	unsigned		wlr;

	struct SHA256Context	*sha256ctx;

	struct http_conn	htc[1];
	struct ws		ws[1];
	struct http		*http[3];
	struct http		*bereq;
	struct http		*beresp1;
	struct http		*beresp;
	struct http		*resp;

	unsigned		cacheable;
	double			age;
	double			entered;
	double			ttl;
	double			grace;
	unsigned		do_esi;

	/* Timeouts */
	double			connect_timeout;
	double			first_byte_timeout;
	double			between_bytes_timeout;

};

/* Work Request for worker thread ------------------------------------*/

/*
 * This is a worker-function.
 * XXX: typesafety is probably not worth fighting for
 */

typedef void workfunc(struct worker *, void *priv);

struct workreq {
	VTAILQ_ENTRY(workreq)	list;
	workfunc		*func;
	void			*priv;
};

/* Storage -----------------------------------------------------------*/

struct storage {
	unsigned		magic;
#define STORAGE_MAGIC		0x1a4e51c0
	VTAILQ_ENTRY(storage)	list;
	struct stevedore	*stevedore;
	void			*priv;

	unsigned char		*ptr;
	unsigned		len;
	unsigned		space;

	int			fd;
	off_t			where;
};

/* Object core structure ---------------------------------------------
 * Objects have sideways references in the binary heap and the LRU list
 * and we want to avoid paging in a lot of objects just to move them up
 * or down the binheap or to move a unrelated object on the LRU list.
 * To avoid this we use a proxy object, objcore, to hold the relevant
 * housekeeping fields parts of an object.
 */

struct objcore {
	unsigned		magic;
#define OBJCORE_MAGIC		0x4d301302
	unsigned		refcnt;
	struct object		*obj;
	struct objhead		*objhead;
	double			timer_when;
	unsigned		flags;
#define OC_F_ONLRU		(1<<0)
#define OC_F_BUSY		(1<<1)
#define OC_F_PASS		(1<<2)
#define OC_F_PERSISTENT		(1<<3)
#define OC_F_LRUDONTMOVE	(1<<4)
	unsigned		timer_idx;
	VTAILQ_ENTRY(objcore)	list;
	VLIST_ENTRY(objcore)	lru_list;
	VTAILQ_ENTRY(objcore)	ban_list;
	struct smp_seg		*smp_seg;
	struct ban		*ban;
};

/*--------------------------------------------------------------------*/

struct lru {
	unsigned		magic;
#define LRU_MAGIC		0x3fec7bb0
	VLIST_HEAD(,objcore)	lru_head;
	struct objcore		senteniel;
};

/* Object structure --------------------------------------------------*/

struct object {
	unsigned		magic;
#define OBJECT_MAGIC		0x32851d42
	unsigned		xid;
	struct storage		*objstore;
	struct objcore		*objcore;

	unsigned		smp_index;

	struct ws		ws_o[1];
	unsigned char		*vary;

	double			ban_t;
	struct ban		*ban;	/* XXX --> objcore */
	unsigned		response;

	unsigned		cacheable;

	unsigned		len;

	double			ttl;
	double			age;
	double			entered;
	double			grace;

	double			last_modified;
	double			last_lru;

	struct http		*http;

	VTAILQ_HEAD(, storage)	store;

	struct esidata		*esidata;

	double			last_use;

	int			hits;
};

/* -------------------------------------------------------------------*/

struct sess {
	unsigned		magic;
#define SESS_MAGIC		0x2c2f9c5a
	int			fd;
	int			id;
	unsigned		xid;

	int			restarts;
	int			esis;
	int			disable_esi;

	struct worker		*wrk;

	socklen_t		sockaddrlen;
	socklen_t		mysockaddrlen;
	struct sockaddr		*sockaddr;
	struct sockaddr		*mysockaddr;
	struct listen_sock	*mylsock;

	/* formatted ascii client address */
	char			*addr;
	char			*port;

	/* HTTP request */
	const char		*doclose;
	struct http		*http;
	struct http		*http0;

	struct ws		ws[1];
	char			*ws_ses;	/* WS above session data */
	char			*ws_req;	/* WS above request data */

	unsigned char		digest[DIGEST_LEN];

	struct http_conn	htc[1];

	/* Timestamps, all on TIM_real() timescale */
	double			t_open;
	double			t_req;
	double			t_resp;
	double			t_end;

	/* Acceptable grace period */
	double			grace;

	enum step		step;
	unsigned		cur_method;
	unsigned		handling;
	unsigned char		sendbody;
	unsigned char		wantbody;
	int			err_code;
	const char		*err_reason;

	VTAILQ_ENTRY(sess)	list;

	struct director		*director;
	struct vbe_conn		*vbe;
	struct object		*obj;
	struct objcore		*objcore;
	struct objhead		*objhead;
	struct VCL_conf		*vcl;

	/* Various internal stuff */
	struct sessmem		*mem;

	struct workreq		workreq;
	struct acct		acct;
	struct acct		acct_req;

#if defined(HAVE_EPOLL_CTL)
	struct epoll_event ev;
#endif
};

/* -------------------------------------------------------------------*/

/* Backend connection */
struct vbe_conn {
	unsigned		magic;
#define VBE_CONN_MAGIC		0x0c5e6592
	VTAILQ_ENTRY(vbe_conn)	list;
	struct backend		*backend;
	int			fd;

	/* Timeouts */
	double			first_byte_timeout;
	double			between_bytes_timeout;
};

/* Prototypes etc ----------------------------------------------------*/

/* cache_acceptor.c */
void vca_return_session(struct sess *sp);
void vca_close_session(struct sess *sp, const char *why);
void VCA_Prep(struct sess *sp);
void VCA_Init(void);
void VCA_Shutdown(void);
const char *VCA_waiter_name(void);
extern pthread_t VCA_thread;

/* cache_backend.c */

struct vbe_conn *VBE_GetFd(const struct director *, struct sess *sp);
int VBE_Healthy(double now, const struct director *, uintptr_t target);
int VBE_Healthy_sp(const struct sess *sp, const struct director *);
void VBE_ClosedFd(struct sess *sp);
void VBE_RecycleFd(struct sess *sp);
void VBE_AddHostHeader(const struct sess *sp);
void VBE_Poll(void);

/* cache_backend_cfg.c */
void VBE_Init(void);
struct backend *VBE_AddBackend(struct cli *cli, const struct vrt_backend *vb);

/* cache_backend_poll.c */
void VBP_Init(void);

/* cache_ban.c */
struct ban *BAN_New(void);
int BAN_AddTest(struct cli *, struct ban *, const char *, const char *,
    const char *);
void BAN_Free(struct ban *b);
void BAN_Insert(struct ban *b);
void BAN_Init(void);
void BAN_NewObj(struct object *o);
void BAN_DestroyObj(struct object *o);
int BAN_CheckObject(struct object *o, const struct sess *sp);
void BAN_Reload(double t0, unsigned flags, const char *ban);
struct ban *BAN_TailRef(void);
void BAN_Compile(void);
struct ban *BAN_RefBan(struct objcore *oc, double t0, const struct ban *tail);
void BAN_Deref(struct ban **ban);

/* cache_center.c [CNT] */
void CNT_Session(struct sess *sp);
void CNT_Init(void);

/* cache_cli.c [CLI] */
void CLI_Init(void);
void CLI_Run(void);
void CLI_AddFuncs(struct cli_proto *p);
extern pthread_t cli_thread;
#define ASSERT_CLI() do {assert(pthread_self() == cli_thread);} while (0)

/* cache_expiry.c */
void EXP_Insert(struct object *o);
void EXP_Inject(struct objcore *oc, struct lru *lru, double ttl);
void EXP_Init(void);
void EXP_Rearm(const struct object *o);
int EXP_Touch(const struct object *o);
int EXP_NukeOne(const struct sess *sp, const struct lru *lru);

/* cache_fetch.c */
int FetchHdr(struct sess *sp);
int FetchBody(struct sess *sp);
int FetchReqBody(struct sess *sp);
void Fetch_Init(void);

/* cache_http.c */
unsigned HTTP_estimate(unsigned nhttp);
void HTTP_Copy(struct http *to, const struct http * const fm);
struct http *HTTP_create(void *p, unsigned nhttp);
const char *http_StatusMessage(unsigned);
unsigned http_EstimateWS(const struct http *fm, unsigned how, unsigned *nhd);
void HTTP_Init(void);
void http_ClrHeader(struct http *to);
unsigned http_Write(struct worker *w, const struct http *hp, int resp);
void http_CopyResp(const struct http *to, const struct http *fm);
void http_SetResp(const struct http *to, const char *proto, const char *status,
    const char *response);
void http_FilterFields(struct worker *w, int fd, struct http *to,
    const struct http *fm, unsigned how);
void http_FilterHeader(const struct sess *sp, unsigned how);
void http_PutProtocol(struct worker *w, int fd, const struct http *to,
    const char *protocol);
void http_PutStatus(struct worker *w, int fd, struct http *to, int status);
void http_PutResponse(struct worker *w, int fd, const struct http *to,
    const char *response);
void http_PrintfHeader(struct worker *w, int fd, struct http *to,
    const char *fmt, ...);
void http_SetHeader(struct worker *w, int fd, struct http *to, const char *hdr);
void http_SetH(const struct http *to, unsigned n, const char *fm);
void http_ForceGet(const struct http *to);
void http_Setup(struct http *ht, struct ws *ws);
int http_GetHdr(const struct http *hp, const char *hdr, char **ptr);
int http_GetHdrField(const struct http *hp, const char *hdr,
    const char *field, char **ptr);
int http_GetStatus(const struct http *hp);
const char *http_GetReq(const struct http *hp);
int http_HdrIs(const struct http *hp, const char *hdr, const char *val);
int http_DissectRequest(struct sess *sp);
int http_DissectResponse(struct worker *w, const struct http_conn *htc,
    struct http *sp);
const char *http_DoConnection(const struct http *hp);
void http_CopyHome(struct worker *w, int fd, const struct http *hp);
void http_Unset(struct http *hp, const char *hdr);

/* cache_httpconn.c */
void HTC_Init(struct http_conn *htc, struct ws *ws, int fd);
int HTC_Reinit(struct http_conn *htc);
int HTC_Rx(struct http_conn *htc);
int HTC_Read(struct http_conn *htc, void *d, unsigned len);
int HTC_Complete(struct http_conn *htc);

#define HTTPH(a, b, c, d, e, f, g) extern char b[];
#include "http_headers.h"
#undef HTTPH

/* cache_main.c */
void THR_SetName(const char *name);
const char* THR_GetName(void);
void THR_SetSession(const struct sess *sp);
const struct sess * THR_GetSession(void);

/* cache_lck.c */

/* Internal functions, call only through macros below */
void Lck__Lock(struct lock *lck, const char *p, const char *f, int l);
void Lck__Unlock(struct lock *lck, const char *p, const char *f, int l);
int Lck__Trylock(struct lock *lck, const char *p, const char *f, int l);
void Lck__New(struct lock *lck, const char *w);
void Lck__Assert(const struct lock *lck, int held);

/* public interface: */
void LCK_Init(void);
void Lck_Delete(struct lock *lck);
void Lck_CondWait(pthread_cond_t *cond, struct lock *lck);

#define Lck_New(a) Lck__New(a, #a);
#define Lck_Lock(a) Lck__Lock(a, __func__, __FILE__, __LINE__)
#define Lck_Unlock(a) Lck__Unlock(a, __func__, __FILE__, __LINE__)
#define Lck_Trylock(a) Lck__Trylock(a, __func__, __FILE__, __LINE__)
#define Lck_AssertHeld(a) Lck__Assert(a, 1)
#define Lck_AssertNotHeld(a) Lck__Assert(a, 0)

/* cache_panic.c */
void PAN_Init(void);

/* cache_pipe.c */
void PipeSession(struct sess *sp);

/* cache_pool.c */
void WRK_Init(void);
int WRK_Queue(struct workreq *wrq);
int WRK_QueueSession(struct sess *sp);
void WRK_SumStat(struct worker *w);

void WRW_Reserve(struct worker *w, int *fd);
unsigned WRW_Flush(struct worker *w);
unsigned WRW_FlushRelease(struct worker *w);
unsigned WRW_Write(struct worker *w, const void *ptr, int len);
unsigned WRW_WriteH(struct worker *w, const txt *hh, const char *suf);
#ifdef SENDFILE_WORKS
void WRW_Sendfile(struct worker *w, int fd, off_t off, unsigned len);
#endif  /* SENDFILE_WORKS */

typedef void *bgthread_t(struct sess *, void *priv);
void WRK_BgThread(pthread_t *thr, const char *name, bgthread_t *func,
    void *priv);

/* cache_session.c [SES] */
void SES_Init(void);
struct sess *SES_New(void);
struct sess *SES_Alloc(void);
void SES_Delete(struct sess *sp);
void SES_Charge(struct sess *sp);

/* cache_shmlog.c */
void VSL_Init(void);
#ifdef SHMLOGHEAD_MAGIC
void VSL(enum shmlogtag tag, int id, const char *fmt, ...);
void WSLR(struct worker *w, enum shmlogtag tag, int id, txt t);
void WSL(struct worker *w, enum shmlogtag tag, int id, const char *fmt, ...);
void WSL_Flush(struct worker *w, int overflow);

#define DSL(flag, tag, id, ...)					\
	do {							\
		if (params->diag_bitmap & (flag))		\
			VSL((tag), (id), __VA_ARGS__);		\
	} while (0)

#define WSP(sess, tag, ...)					\
	WSL((sess)->wrk, tag, (sess)->fd, __VA_ARGS__)

#define WSPR(sess, tag, txt)					\
	WSLR((sess)->wrk, tag, (sess)->fd, txt)

#define INCOMPL() do {							\
	VSL(SLT_Debug, 0, "INCOMPLETE AT: %s(%d)", __func__, __LINE__); \
	fprintf(stderr,							\
	    "INCOMPLETE AT: %s(%d)\n",					\
	    (const char *)__func__, __LINE__);				\
	abort();							\
	} while (0)
#endif

/* cache_response.c */
void RES_BuildHttp(struct sess *sp);
void RES_WriteObj(struct sess *sp);

/* cache_vary.c */
struct vsb *VRY_Create(const struct sess *sp, const struct http *hp);
int VRY_Match(const struct sess *sp, const unsigned char *vary);

/* cache_vcl.c */
void VCL_Init(void);
void VCL_Refresh(struct VCL_conf **vcc);
void VCL_Rel(struct VCL_conf **vcc);
void VCL_Get(struct VCL_conf **vcc);
void VCL_Poll(void);

#define VCL_MET_MAC(l,u,b) void VCL_##l##_method(struct sess *);
#include "vcl_returns.h"
#undef VCL_MET_MAC

/* cache_vrt_esi.c */

void ESI_Deliver(struct sess *);
void ESI_Destroy(struct object *);
void ESI_Parse(struct sess *);

/* cache_ws.c */

void WS_Init(struct ws *ws, const char *id, void *space, unsigned len);
unsigned WS_Reserve(struct ws *ws, unsigned bytes);
void WS_Release(struct ws *ws, unsigned bytes);
void WS_ReleaseP(struct ws *ws, char *ptr);
void WS_Assert(const struct ws *ws);
void WS_Reset(struct ws *ws, char *p);
char *WS_Alloc(struct ws *ws, unsigned bytes);
char *WS_Dup(struct ws *ws, const char *);
char *WS_Snapshot(struct ws *ws);
unsigned WS_Free(const struct ws *ws);

/* rfc2616.c */
double RFC2616_Ttl(const struct sess *sp);

/* storage_synth.c */
struct vsb *SMS_Makesynth(struct object *obj);
void SMS_Finish(struct object *obj);

/* storage_persistent.c */
void SMP_Fixup(struct sess *sp, const struct objhead *oh, struct objcore *oc);
void SMP_BANchanged(const struct object *o, double t);
void SMP_TTLchanged(const struct object *o);
void SMP_FreeObj(const struct object *o);
void SMP_Ready(void);
void SMP_NewBan(double t0, const char *ban);

/*
 * A normal pointer difference is signed, but we never want a negative value
 * so this little tool will make sure we don't get that.
 */

static inline unsigned
pdiff(const void *b, const void *e)
{

	assert(b <= e);
	return
	    ((unsigned)((const unsigned char *)e - (const unsigned char *)b));
}

static inline void
Tcheck(const txt t)
{

	AN(t.b);
	AN(t.e);
	assert(t.b <= t.e);
}

/*
 * unsigned length of a txt
 */

static inline unsigned
Tlen(const txt t)
{

	Tcheck(t);
	return ((unsigned)(t.e - t.b));
}

static inline void
Tadd(txt *t, const char *p, int l)
{
	Tcheck(*t);

	if (l <= 0) {
	} if (t->b + l < t->e) {
		memcpy(t->b, p, l);
		t->b += l;
	} else {
		t->b = t->e;
	}
}

static inline unsigned
ObjIsBusy(const struct object *o)
{
	CHECK_OBJ_NOTNULL(o, OBJECT_MAGIC);
	CHECK_OBJ_NOTNULL(o->objcore, OBJCORE_MAGIC);
	return (o->objcore->flags & OC_F_BUSY);
}

