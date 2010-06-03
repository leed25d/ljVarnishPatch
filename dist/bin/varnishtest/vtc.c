/*
 * Copyright (c) 2008-2010 Redpill Linpro AS
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
 */

#include "config.h"

#include "svnid.h"
SVNID("$Id: vtc.c 4595 2010-02-26 19:16:39Z phk $")

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "libvarnish.h"
#include "vsb.h"
#include "vqueue.h"
#include "miniobj.h"

#include "vtc.h"

#define		MAX_FILESIZE		(1024 * 1024)
#define		MAX_TOKENS		200

static const char	*vtc_file;
static char		*vtc_desc;
int			vtc_error;	/* Error encountered */
int			vtc_stop;	/* Stops current test without error */
pthread_t		vtc_thread;
char			*vtc_tmpdir;
static struct vtclog	*vltop;
static pthread_mutex_t	vtc_mtx;
static pthread_cond_t	vtc_cond;

/**********************************************************************
 * Macro facility
 */

struct macro {
	VTAILQ_ENTRY(macro)	list;
	char			*name;
	char			*val;
};

static VTAILQ_HEAD(,macro) macro_list = VTAILQ_HEAD_INITIALIZER(macro_list);

static pthread_mutex_t		macro_mtx;

static void
init_macro(void)
{
	AZ(pthread_mutex_init(&macro_mtx, NULL));
}

void
macro_def(struct vtclog *vl, const char *instance, const char *name,
    const char *fmt, ...)
{
	char buf1[256];
	char buf2[256];
	struct macro *m;
	va_list ap;

	if (instance != NULL) {
		bprintf(buf1, "%s_%s", instance, name);
		name = buf1;
	}

	AZ(pthread_mutex_lock(&macro_mtx));
	VTAILQ_FOREACH(m, &macro_list, list)
		if (!strcmp(name, m->name))
			break;
	if (m == NULL && fmt != NULL) {
		m = calloc(sizeof *m, 1);
		AN(m);
		REPLACE(m->name, name);
		VTAILQ_INSERT_TAIL(&macro_list, m, list);
	}
	if (fmt != NULL) {
		AN(m);
		va_start(ap, fmt);
		free(m->val);
		m->val = NULL;
		vbprintf(buf2, fmt, ap);
		va_end(ap);
		m->val = strdup(buf2);
		AN(m->val);
		vtc_log(vl, 4, "macro def %s=%s", name, m->val);
	} else if (m != NULL) {
		vtc_log(vl, 4, "macro undef %s", name);
		VTAILQ_REMOVE(&macro_list, m, list);
		free(m->name);
		free(m->val);
		free(m);
	}
	AZ(pthread_mutex_unlock(&macro_mtx));
}

static char *
macro_get(const char *name)
{
	struct macro *m;

	char *retval = NULL;
	AZ(pthread_mutex_lock(&macro_mtx));
	VTAILQ_FOREACH(m, &macro_list, list)
		if (!strcmp(name, m->name))
			break;
	if (m != NULL)
		retval = strdup(m->val);
	AZ(pthread_mutex_unlock(&macro_mtx));
	return (retval);
}

struct vsb *
macro_expand(const char *name)
{
	struct vsb *vsb;
	char *p, *q;

	vsb = vsb_newauto();
	AN(vsb);
	while (*name != '\0') {
		p = strstr(name, "${");
		if (p == NULL) {
			vsb_cat(vsb, name);
			break;
		}
		vsb_bcat(vsb, name, p - name);
		q = strchr(p, '}');
		if (q == NULL) {
			vsb_cat(vsb, name);
			break;
		}
		assert(p[0] == '$');
		assert(p[1] == '{');
		assert(q[0] == '}');
		p += 2;
		*q = '\0';
		vsb_cat(vsb, macro_get(p));
		name = q + 1;
	}
	vsb_finish(vsb);
	return (vsb);
}

/**********************************************************************
 * Read a file into memory
 */

static char *
read_file(const char *fn)
{
	char *buf;
	ssize_t sz = MAX_FILESIZE;
	ssize_t s;
	int fd;

	fd = open(fn, O_RDONLY);
	if (fd < 0)
		return (NULL);
	buf = malloc(sz);
	assert(buf != NULL);
	s = read(fd, buf, sz - 1);
	if (s <= 0) {
		free(buf);
		return (NULL);
	}
	AZ(close (fd));
	assert(s < sz);		/* XXX: increase MAX_FILESIZE */
	buf[s] = '\0';
	buf = realloc(buf, s + 1);
	assert(buf != NULL);
	return (buf);
}

/**********************************************************************
 * Execute a file
 */

void
parse_string(char *buf, const struct cmds *cmd, void *priv, struct vtclog *vl)
{
	char *token_s[MAX_TOKENS], *token_e[MAX_TOKENS];
	struct vsb *token_exp[MAX_TOKENS];
	char *p, *q;
	int nest_brace;
	int tn;
	const struct cmds *cp;

	assert(buf != NULL);
	for (p = buf; *p != '\0'; p++) {
		if (vtc_error || vtc_stop)
			break;
		/* Start of line */
		if (isspace(*p))
			continue;
		if (*p == '#') {
			for (; *p != '\0' && *p != '\n'; p++)
				;
			if (*p == '\0')
				break;
			continue;
		}

		/* First content on line, collect tokens */
		tn = 0;
		while (*p != '\0') {
			assert(tn < MAX_TOKENS);
			if (*p == '\n') { /* End on NL */
				break;
			}
			if (isspace(*p)) { /* Inter-token whitespace */
				p++;
				continue;
			}
			if (*p == '\\' && p[1] == '\n') { /* line-cont */
				p += 2;
				continue;
			}
			if (*p == '"') { /* quotes */
				token_s[tn] = ++p;
				q = p;
				for (; *p != '\0'; p++) {
					if (*p == '"')
						break;
					if (*p == '\\') {
						p += BackSlash(p, q) - 1;
						q++;
					} else {
						if (*p == '\n')
							fprintf(stderr,
				"Unterminated quoted string in line:\n%s", p);
						assert(*p != '\n');
						*q++ = *p;
					}
				}
				token_e[tn++] = q;
				p++;
			} else if (*p == '{') { /* Braces */
				nest_brace = 0;
				token_s[tn] = p + 1;
				for (; *p != '\0'; p++) {
					if (*p == '{')
						nest_brace++;
					else if (*p == '}') {
						if (--nest_brace == 0)
							break;
					}
				}
				assert(*p == '}');
				token_e[tn++] = p++;
			} else { /* other tokens */
				token_s[tn] = p;
				for (; *p != '\0' && !isspace(*p); p++)
					;
				token_e[tn++] = p;
			}
		}
		assert(tn < MAX_TOKENS);
		token_s[tn] = NULL;
		for (tn = 0; token_s[tn] != NULL; tn++) {
			token_exp[tn] = NULL;
			AN(token_e[tn]);	/*lint !e771 */
			*token_e[tn] = '\0';	/*lint !e771 */
			if (NULL == strstr(token_s[tn], "${"))
				continue;
			token_exp[tn] = macro_expand(token_s[tn]);
			token_s[tn] = vsb_data(token_exp[tn]);
			token_e[tn] = strchr(token_s[tn], '\0');
		}

		for (cp = cmd; cp->name != NULL; cp++)
			if (!strcmp(token_s[0], cp->name))
				break;
		if (cp->name == NULL)
			vtc_log(vl, 0, "Unknown command: \"%s\"", token_s[0]);

		assert(cp->cmd != NULL);
		cp->cmd(token_s, priv, cmd, vl);
	}
}

/**********************************************************************
 * Reset commands (between tests)
 */

static void
reset_cmds(const struct cmds *cmd)
{

	for (; cmd->name != NULL; cmd++)
		cmd->cmd(NULL, NULL, NULL, NULL);
}

/**********************************************************************
 * Output test description
 */

static void
cmd_test(CMD_ARGS)
{

	(void)priv;
	(void)cmd;
	(void)vl;

	if (av == NULL)
		return;
	assert(!strcmp(av[0], "test"));

	vtc_log(vl, 1, "TEST %s", av[1]);
	AZ(av[2]);
	vtc_desc = strdup(av[1]);
}

/**********************************************************************
 * Shell command execution
 */

static void
cmd_shell(CMD_ARGS)
{
	(void)priv;
	(void)cmd;
	int r;

	if (av == NULL)
		return;
	AN(av[1]);
	AZ(av[2]);
	vtc_dump(vl, 4, "shell", av[1]);
	r = system(av[1]);
	assert(WEXITSTATUS(r) == 0);
}

/**********************************************************************
 * Dump command arguments
 */

void
cmd_delay(CMD_ARGS)
{
	double f;

	(void)priv;
	(void)cmd;
	if (av == NULL)
		return;
	AN(av[1]);
	AZ(av[2]);
	f = strtod(av[1], NULL);
	vtc_log(vl, 3, "delaying %g second(s)", f);
	TIM_sleep(f);
}

/**********************************************************************
 * Dump command arguments
 */

void
cmd_dump(CMD_ARGS)
{

	(void)cmd;
	(void)vl;
	if (av == NULL)
		return;
	printf("cmd_dump(%p)\n", priv);
	while (*av)
		printf("\t<%s>\n", *av++);
}

/**********************************************************************
 * Check random generator
 */

#define NRNDEXPECT	12
static const unsigned long random_expect[NRNDEXPECT] = {
	1804289383,	846930886,	1681692777,	1714636915,
	1957747793,	424238335,	719885386,	1649760492,
	 596516649,	1189641421,	1025202362,	1350490027
};

#define RND_NEXT_1K	0x3bdcbe30

static void
cmd_random(CMD_ARGS)
{
	uint32_t l;
	int i;

	(void)cmd;
	(void)priv;
	if (av == NULL)
		return;
	srandom(1);
	for (i = 0; i < NRNDEXPECT; i++) {
		l = random();
		if (l == random_expect[i])
			continue;
		vtc_log(vl, 4, "random[%d] = 0x%x (expect 0x%x)",
		    i, l, random_expect[i]);
		vtc_log(vl, 1, "SKIPPING test: unknown srandom(1) sequence.");
		vtc_stop = 1;
		break;
	}
	l = 0;
	for (i = 0; i < 1000; i++)
		l += random();
	if (l != RND_NEXT_1K) {
		vtc_log(vl, 4, "sum(random[%d...%d]) = 0x%x (expect 0x%x)",
		    NRNDEXPECT, NRNDEXPECT + 1000,
		    l, RND_NEXT_1K);
		vtc_log(vl, 1, "SKIPPING test: unknown srandom(1) sequence.");
		vtc_stop = 1;
	}
}

/**********************************************************************
 * Check features.
 */

static void
cmd_feature(CMD_ARGS)
{
	int i;

	(void)priv;
	(void)cmd;
	if (av == NULL)
		return;

	for (i = 1; av[i] != NULL; i++) {
#ifdef SO_RCVTIMEO_WORKS
		if (!strcmp(av[i], "SO_RCVTIMEO_WORKS"))
			continue;
#endif
		vtc_log(vl, 1, "SKIPPING test, missing feature %s", av[i]);
		vtc_stop = 1;
		return;
	}
}

/**********************************************************************
 * Execute a file
 */

static const struct cmds cmds[] = {
	{ "server",	cmd_server },
	{ "client",	cmd_client },
	{ "varnish",	cmd_varnish },
	{ "delay",	cmd_delay },
	{ "test",	cmd_test },
	{ "shell",	cmd_shell },
	{ "sema",	cmd_sema },
	{ "random",	cmd_random },
	{ "feature",	cmd_feature },
	{ NULL,		NULL }
};

struct priv_exec {
	const char	*fn;
	char		*buf;
};

static void *
exec_file_thread(void *priv)
{
	unsigned old_err;
	struct priv_exec *pe;

	pe = priv;

	parse_string(pe->buf, cmds, NULL, vltop);
	old_err = vtc_error;
	vtc_stop = 1;
	vtc_log(vltop, 1, "RESETTING after %s", pe->fn);
	reset_cmds(cmds);
	vtc_error = old_err;
	AZ(pthread_mutex_lock(&vtc_mtx));
	AZ(pthread_cond_signal(&vtc_cond));
	AZ(pthread_mutex_unlock(&vtc_mtx));
	return (NULL);
}

static double
exec_file(const char *fn, unsigned dur)
{
	double t0, t;
	struct priv_exec pe;
	pthread_t pt;
	struct timespec ts;
	void *v;
	int i;

	t0 = TIM_mono();
	vtc_stop = 0;
	vtc_file = fn;
	vtc_desc = NULL;
	vtc_log(vltop, 1, "TEST %s starting", fn);
	pe.buf = read_file(fn);
	if (pe.buf == NULL)
		vtc_log(vltop, 0, "Cannot read file '%s': %s",
		    fn, strerror(errno));
	pe.fn = fn;

	t = TIM_real() + dur;
	ts.tv_sec = floor(t);
	ts.tv_nsec = (t - ts.tv_sec) * 1e9;

	AZ(pthread_mutex_lock(&vtc_mtx));
	AZ(pthread_create(&pt, NULL, exec_file_thread, &pe));
	i = pthread_cond_timedwait(&vtc_cond, &vtc_mtx, &ts);

	if (i == 0) {
		AZ(pthread_mutex_unlock(&vtc_mtx));
		AZ(pthread_join(pt, &v));
	} else {
		if (i != ETIMEDOUT)
			vtc_log(vltop, 1, "Weird condwait return: %d %s",
			    i, strerror(i));
		/*
		 * We are all going to die anyway, so don't waste time
		 * trying to clean things up, it seems to trigger a
		 * problem in the tinderbox
		 *   AZ(pthread_mutex_unlock(&vtc_mtx));
		 *   AZ(pthread_cancel(pt));
		 *   AZ(pthread_join(pt, &v));
		 */
		vtc_log(vltop, 1, "Test timed out");
		vtc_error = 1;
	}

	if (vtc_error)
		vtc_log(vltop, 1, "TEST %s FAILED", fn);
	else {
		vtc_log(vltop, 1, "TEST %s completed", fn);
		vtc_logreset();
	}

	t0 = TIM_mono() - t0;

	if (vtc_error && vtc_verbosity == 0)
		printf("%s", vtc_logfull());
	else if (vtc_verbosity == 0)
		printf("#    top  TEST %s passed (%.3fs)\n", fn, t0);

	vtc_file = NULL;
	free(vtc_desc);
	return (t0);
}

/**********************************************************************
 * Print usage
 */

static void
usage(void)
{
	fprintf(stderr, "usage: varnishtest [-n iter] [-qv] file ...\n");
	exit(1);
}

/**********************************************************************
 * Main
 */

/**********************************************************************
 * Main
 */

int
main(int argc, char * const *argv)
{
	int ch, i, ntest = 1, ncheck = 0;
	FILE *fok;
	double tmax, t0, t00;
	unsigned dur = 30;
	const char *nmax;
	char cmd[BUFSIZ];

	setbuf(stdout, NULL);
	setbuf(stderr, NULL);
	vtc_loginit();
	vltop = vtc_logopen("top");
	AN(vltop);
	while ((ch = getopt(argc, argv, "n:qt:v")) != -1) {
		switch (ch) {
		case 'n':
			ntest = strtoul(optarg, NULL, 0);
			break;
		case 'q':
			vtc_verbosity--;
			break;
		case 't':
			dur = strtoul(optarg, NULL, 0);
			break;
		case 'v':
			vtc_verbosity++;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (argc == 0)
		usage();

	init_macro();
	init_sema();

	setenv("TMPDIR", "/tmp", 0);
	vtc_tmpdir = tempnam(NULL, "vtc");
	AN(vtc_tmpdir);
	AZ(mkdir(vtc_tmpdir, 0700));
	macro_def(vltop, NULL, "tmpdir", vtc_tmpdir);
	vtc_thread = pthread_self();

	AZ(pthread_mutex_init(&vtc_mtx, NULL));
	AZ(pthread_cond_init(&vtc_cond, NULL));

	macro_def(vltop, NULL, "bad_ip", "10.255.255.255");
	tmax = 0;
	nmax = NULL;
	t00 = TIM_mono();
	for (i = 0; i < ntest; i++) {
		for (ch = 0; ch < argc; ch++) {
			t0 = exec_file(argv[ch], dur);
			ncheck++;
			if (t0 > tmax) {
				tmax = t0;
				nmax = argv[ch];
			}
			if (vtc_error)
				break;
		}
		if (vtc_error)
			break;
	}

	/* Remove tmpdir on success or non-verbosity */
	if (vtc_error == 0 || vtc_verbosity == 0) {
		bprintf(cmd, "rm -rf %s", vtc_tmpdir);
		AZ(system(cmd));
		free(vtc_tmpdir);
	}

	if (vtc_error)
		return (2);

	t00 = TIM_mono() - t00;
	if (ncheck > 1) {
		printf("#    top  Slowest test: %s %.3fs\n", nmax, tmax);
		printf("#    top  Total tests run:   %d\n", ncheck);
		printf("#    top  Total duration: %.3fs\n", t00);
	}

	fok = fopen("_.ok", "w");
	if (fok != NULL)
		AZ(fclose(fok));
	return (0);
}
