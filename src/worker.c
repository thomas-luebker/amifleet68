/*
 * worker.c - the network side of amifleet68.
 *
 * A worker is a plain AmigaDOS process with its own message port, its own
 * bsdsocket.library base and its own timer.device base. It takes Jobs off the
 * port one at a time, holds exactly one conversation with one amiagent per
 * Job (the protocol is one request per connection, AUTH first if there is a
 * token), and replies the Job to the GUI.
 *
 * No C runtime in here: no stdio, no malloc. The worker is a second task
 * running on the same newlib the GUI uses, and newlib's allocator and stdio
 * are not re-entrant. Everything below is exec, dos and bsdsocket.
 *
 * Library bases: the bsdsocket and timer inlines name their base through
 * BSDSOCKET_BASE_NAME / TIMER_BASE_NAME. Pointing those at the worker's own
 * fields means every call in this file goes through the base the *calling
 * process* opened, which is what Roadshow requires.
 */

#ifndef __amigaos__
#error "amifleet68 targets AmigaOS - build with m68k-amigaos-gcc (see Makefile)"
#endif

/* The NDK's sys/socket.h uses ssize_t, but newlib gates its typedef behind a
 * feature macro. Same dance as amiagent and amipkg's http.c. */
#include <sys/types.h>
#ifndef _SSIZE_T_DECLARED
typedef long ssize_t;
#define _SSIZE_T_DECLARED
#endif

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <devices/timer.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "net.h"

volatile int g_quitting = 0;

#define CONNECT_SECS 2      /* a machine that is off costs this much */
#define SCAN_BATCH   16     /* parallel connects during a sweep */
#define SCAN_WAIT_MS 900    /* how long one batch may take to answer */
#define MAX_REPLY    AMI_MAXFRAME       /* a full RTG SHOT is several MB */

/* ------------------------------------------------------------------ *
 * Small helpers (no libc)
 * ------------------------------------------------------------------ */

void scopy(char *d, const char *s, ULONG n)
{
    if (!n) return;
    while (--n && *s) *d++ = *s++;
    *d = 0;
}

ULONG slen(const char *s) { ULONG n = 0; while (s[n]) n++; return n; }

void put_be32(UBYTE *p, ULONG v)
{
    p[0] = (UBYTE)(v >> 24); p[1] = (UBYTE)(v >> 16);
    p[2] = (UBYTE)(v >> 8);  p[3] = (UBYTE)v;
}

ULONG get_be32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3];
}

/* "a.b.c.d" -> host-order ULONG; 0 when it is not a dotted quad. */
static ULONG parse_quad(const char *s)
{
    ULONG v = 0, part = 0;
    int dots = 0, digits = 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            part = part * 10 + (ULONG)(*s - '0');
            if (part > 255 || ++digits > 3) return 0;
        } else if (*s == '.' || *s == 0) {
            if (!digits) return 0;
            v = (v << 8) | part;
            part = 0; digits = 0;
            if (*s == 0) break;
            if (++dots > 3) return 0;
        } else return 0;
    }
    return dots == 3 ? v : 0;
}

static void fmt_quad(char *d, ULONG a)
{
    int i;
    for (i = 3; i >= 0; i--) {
        UBYTE o = (UBYTE)(a >> (i * 8));
        if (o >= 100) *d++ = (char)('0' + o / 100);
        if (o >= 10)  *d++ = (char)('0' + (o / 10) % 10);
        *d++ = (char)('0' + o % 10);
        if (i) *d++ = '.';
    }
    *d = 0;
}

ULONG now_ms(struct Worker *wk)
{
    struct timeval tv;
    if (!wk->tb) return 0;
    GetSysTime(&tv);
    return (ULONG)tv.tv_secs * 1000UL + (ULONG)tv.tv_micro / 1000UL;
}

/* ------------------------------------------------------------------ *
 * Sockets
 * ------------------------------------------------------------------ */

/* Wait for readable (write=0) or writable (write=1). 1 ready, 0 timeout,
 * -1 cancelled or error. Ctrl-C is how the GUI wakes a worker at quit. */
int sock_wait(struct Worker *wk, LONG s, int write, ULONG ms)
{
    fd_set set;
    struct timeval tv;
    ULONG sigs = SIGBREAKF_CTRL_C;
    LONG n;

    FD_ZERO(&set);
    FD_SET(s, &set);
    tv.tv_secs = ms / 1000;
    tv.tv_micro = (ms % 1000) * 1000;
    n = WaitSelect(s + 1, write ? NULL : &set, write ? &set : NULL, NULL, &tv, &sigs);
    if (sigs & SIGBREAKF_CTRL_C) return -1;
    if (n < 0) return -1;
    return n > 0 && FD_ISSET(s, &set) ? 1 : 0;
}

int send_all(struct Worker *wk, LONG s, const UBYTE *p, ULONG n)
{
    while (n) {
        LONG put = send(s, (APTR)p, (LONG)(n > 0x4000 ? 0x4000 : n), 0);
        if (put <= 0) return 0;
        p += put; n -= (ULONG)put;
    }
    return 1;
}

int recv_all(struct Worker *wk, LONG s, UBYTE *p, ULONG n, ULONG ms)
{
    while (n) {
        LONG got;
        if (sock_wait(wk, s, 0, ms) != 1) return 0;
        got = recv(s, (APTR)p, (LONG)(n > 0x4000 ? 0x4000 : n), 0);
        if (got <= 0) return 0;
        p += got; n -= (ULONG)got;
    }
    return 1;
}

ULONG resolve(struct Worker *wk, const char *host)
{
    ULONG a = parse_quad(host);
    if (!a) {
        struct hostent *he = gethostbyname((STRPTR)host);
        if (he && he->h_addr_list && he->h_addr_list[0])
            a = get_be32((UBYTE *)he->h_addr_list[0]);
    }
    return a;
}

/* Start a non-blocking connect. Returns the socket, or -1. */
LONG connect_start(struct Worker *wk, ULONG addr, UWORD port)
{
    struct sockaddr_in sa;
    LONG s, one = 1;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    IoctlSocket(s, FIONBIO, (char *)&one);
    {
        UBYTE *z = (UBYTE *)&sa; ULONG i;
        for (i = 0; i < sizeof sa; i++) z[i] = 0;
    }
    sa.sin_len = sizeof sa;
    sa.sin_family = AF_INET;
    sa.sin_port = port;               /* m68k is big-endian: host == network */
    sa.sin_addr.s_addr = addr;
    connect(s, (struct sockaddr *)&sa, sizeof sa);   /* EINPROGRESS expected */
    return s;
}

int connect_ok(struct Worker *wk, LONG s)
{
    LONG err = 0, len = sizeof err;
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, (APTR)&err, (APTR)&len) < 0) return 0;
    return err == 0;
}

/* One framed exchange on an open socket. */
static int frame_send(struct Worker *wk, LONG s, UBYTE code, const UBYTE *pl, ULONG n)
{
    UBYTE h[AMI_HDRLEN];
    h[0] = AMI_MAGIC0; h[1] = AMI_MAGIC1; h[2] = AMI_MAGIC2; h[3] = AMI_MAGIC3;
    h[4] = code; h[5] = 0; h[6] = 0; h[7] = 0;
    put_be32(h + 8, n);
    return send_all(wk, s, h, AMI_HDRLEN) && (!n || send_all(wk, s, pl, n));
}

/* Reads a response; the payload lands in *out (AllocVec'd, NUL-terminated)
 * unless the caller passes out = NULL, in which case it is drained. */
static int frame_recv(struct Worker *wk, LONG s, UBYTE *status, UBYTE **out,
                      ULONG *outlen, ULONG ms)
{
    UBYTE h[AMI_HDRLEN];
    ULONG len;
    UBYTE *buf;

    if (!recv_all(wk, s, h, AMI_HDRLEN, ms)) return 0;
    if (h[0] != AMI_MAGIC0 || h[1] != AMI_MAGIC1 || h[2] != AMI_MAGIC2 || h[3] != AMI_MAGIC3)
        return 0;
    *status = h[4];
    len = get_be32(h + 8);
    if (len > MAX_REPLY) return 0;
    buf = (UBYTE *)AllocVec(len + 1, MEMF_ANY);
    if (!buf) return 0;
    if (len && !recv_all(wk, s, buf, len, ms)) { FreeVec(buf); return 0; }
    buf[len] = 0;
    if (out) { *out = buf; *outlen = len; } else FreeVec(buf);
    return 1;
}

/* The whole protocol conversation for one Job: connect, AUTH if there is a
 * token, one request, one response. Fills j->status/out/err/ms. */
/* Connect and AUTH. The open socket, or -1 with j->status/err set. */
static LONG agent_open(struct Worker *wk, struct Job *j, ULONG ms)
{
    ULONG addr;
    LONG s;
    UBYTE st = 0;
    int r;

    addr = resolve(wk, j->host);
    if (!addr) { j->status = JS_NET; scopy(j->err, "unknown host", sizeof j->err); return -1; }

    s = connect_start(wk, addr, j->port);
    if (s < 0) { j->status = JS_NET; scopy(j->err, "no socket", sizeof j->err); return -1; }
    r = sock_wait(wk, s, 1, CONNECT_SECS * 1000UL);
    if (r != 1 || !connect_ok(wk, s)) {
        CloseSocket(s);
        j->status = r < 0 ? JS_CANCEL : JS_NET;
        scopy(j->err, r == 0 ? "timed out" : "connection refused", sizeof j->err);
        return -1;
    }
    {   /* back to blocking; reads are still bounded by WaitSelect */
        LONG zero = 0;
        IoctlSocket(s, FIONBIO, (char *)&zero);
    }

    if (j->token[0]) {
        UBYTE *e = NULL; ULONG el = 0;
        if (!frame_send(wk, s, CMD_AUTH, (const UBYTE *)j->token, slen(j->token)) ||
            !frame_recv(wk, s, &st, &e, &el, ms)) {
            CloseSocket(s);
            j->status = JS_NET; scopy(j->err, "connection dropped during AUTH", sizeof j->err);
            return -1;
        }
        if (st != ST_OK) {
            CloseSocket(s);
            j->status = JS_AUTH;
            scopy(j->err, el ? (char *)e : "token refused", sizeof j->err);
            FreeVec(e);
            return -1;
        }
        FreeVec(e);
    }
    return s;
}

/* The response half: status, error text, round-trip time. Closes s. */
static void agent_finish(struct Worker *wk, struct Job *j, LONG s, ULONG t0, ULONG ms)
{
    UBYTE st = 0;
    if (!frame_recv(wk, s, &st, &j->out, &j->outlen, ms)) {
        CloseSocket(s);
        j->status = JS_NET; scopy(j->err, "no answer (timed out or dropped)", sizeof j->err);
        return;
    }
    CloseSocket(s);
    j->ms = now_ms(wk) - t0;
    if (st == ST_OK) { j->status = JS_OK; return; }
    j->status = st == ST_AUTH ? JS_AUTH : JS_ERR;
    scopy(j->err, j->outlen ? (char *)j->out : (st == ST_AUTH ? "token required" : "error"),
          sizeof j->err);
    FreeVec(j->out); j->out = NULL; j->outlen = 0;
}

void request(struct Worker *wk, struct Job *j, UBYTE code,
             const UBYTE *pl, ULONG pllen, ULONG ms)
{
    ULONG t0;
    LONG s;

    j->out = NULL; j->outlen = 0; j->err[0] = 0;
    t0 = now_ms(wk);
    if ((s = agent_open(wk, j, ms)) < 0) return;
    if (!frame_send(wk, s, code, pl, pllen)) {
        CloseSocket(s);
        j->status = JS_NET; scopy(j->err, "connection dropped while sending", sizeof j->err);
        return;
    }
    agent_finish(wk, j, s, t0, ms);
}

/* ------------------------------------------------------------------ *
 * File transfer
 * ------------------------------------------------------------------ */

#define XFER_CHUNK (256UL * 1024UL)

static void fail(struct Job *j, UBYTE st, const char *a, const char *b)
{
    j->status = st;
    scopy(j->err, a, sizeof j->err);
    if (b) { ULONG l = slen(j->err); scopy(j->err + l, b, sizeof j->err - l); }
}

/* Download j->arg (remote) to j->local, in GETRANGE chunks so a big file
 * never has to fit in one frame, or in memory. */
static void job_get(struct Worker *wk, struct Job *j)
{
    ULONG t0 = now_ms(wk), off = 0, n = slen(j->arg), i;
    BPTR fh;
    int ok = 1;

    j->done = 0;
    fh = Open((STRPTR)j->local, MODE_NEWFILE);
    if (!fh) { fail(j, JS_ERR, "cannot create ", j->local); return; }

    if (!j->total) {                        /* size unknown: one GET */
        struct Job c = *j;
        request(wk, &c, CMD_GET, (const UBYTE *)j->arg, n, 60000);
        j->status = c.status; scopy(j->err, c.err, sizeof j->err);
        if (c.status == JS_OK && c.outlen && Write(fh, c.out, (LONG)c.outlen) != (LONG)c.outlen)
            { ok = 0; fail(j, JS_ERR, "write error on ", j->local); }
        if (c.status != JS_OK) ok = 0;
        j->done = c.outlen;
        if (c.out) FreeVec(c.out);
    } else {
        UBYTE pl[8 + 512];
        for (i = 0; i < n && i < 512; i++) pl[8 + i] = (UBYTE)j->arg[i];
        while (ok && off < j->total) {
            struct Job c = *j;
            ULONG len = j->total - off > XFER_CHUNK ? XFER_CHUNK : j->total - off;
            if (g_quitting) { ok = 0; j->status = JS_CANCEL; break; }
            put_be32(pl, off);
            put_be32(pl + 4, len);
            request(wk, &c, CMD_GETRANGE, pl, 8 + n, 60000);
            if (c.status != JS_OK) {
                ok = 0; j->status = c.status; scopy(j->err, c.err, sizeof j->err);
            } else if (!c.outlen || Write(fh, c.out, (LONG)c.outlen) != (LONG)c.outlen) {
                ok = 0; fail(j, JS_ERR, c.outlen ? "write error on " : "short read from the Amiga", c.outlen ? j->local : NULL);
            } else {
                off += c.outlen;
                j->done = off;
            }
            if (c.out) FreeVec(c.out);
        }
        if (ok) j->status = JS_OK;
    }
    Close(fh);
    if (!ok) DeleteFile((STRPTR)j->local);       /* no half files left behind */
    j->ms = now_ms(wk) - t0;
}

/* Upload j->local to remote path j->arg: one PUT frame, streamed from disk
 * so the file never sits in memory. The protocol caps a frame at 16 MiB. */
static void job_put(struct Worker *wk, struct Job *j)
{
    ULONG t0 = now_ms(wk), n = slen(j->arg), size, sent = 0;
    UBYTE h[AMI_HDRLEN + 2];
    UBYTE *buf;
    BPTR fh;
    LONG s;

    j->done = 0;
    fh = Open((STRPTR)j->local, MODE_OLDFILE);
    if (!fh) { fail(j, JS_ERR, "cannot open ", j->local); return; }
    Seek(fh, 0, OFFSET_END);
    size = (ULONG)Seek(fh, 0, OFFSET_BEGINNING);
    j->total = size;
    if (size + n + 2 > AMI_MAXFRAME) {
        Close(fh);
        fail(j, JS_ERR, "too big for one transfer (16 MB limit)", NULL);
        return;
    }
    buf = (UBYTE *)AllocVec(XFER_CHUNK, MEMF_ANY);
    if (!buf) { Close(fh); fail(j, JS_ERR, "out of memory", NULL); return; }

    j->out = NULL; j->outlen = 0; j->err[0] = 0;
    if ((s = agent_open(wk, j, 20000)) < 0) { FreeVec(buf); Close(fh); return; }

    h[0] = AMI_MAGIC0; h[1] = AMI_MAGIC1; h[2] = AMI_MAGIC2; h[3] = AMI_MAGIC3;
    h[4] = CMD_PUT; h[5] = 0; h[6] = 0; h[7] = 0;
    put_be32(h + 8, 2 + n + size);
    h[12] = (UBYTE)(n >> 8); h[13] = (UBYTE)n;
    if (!send_all(wk, s, h, sizeof h) || !send_all(wk, s, (const UBYTE *)j->arg, n)) goto broken;
    while (sent < size) {
        LONG got = Read(fh, buf, (LONG)(size - sent > XFER_CHUNK ? XFER_CHUNK : size - sent));
        if (got <= 0) {
            /* The frame length is promised; a short file cannot be undone. */
            CloseSocket(s); FreeVec(buf); Close(fh);
            fail(j, JS_ERR, "read error on ", j->local);
            return;
        }
        if (g_quitting || !send_all(wk, s, buf, (ULONG)got)) goto broken;
        sent += (ULONG)got;
        j->done = sent;
    }
    FreeVec(buf);
    Close(fh);
    agent_finish(wk, j, s, t0, 60000);
    return;

broken:
    CloseSocket(s); FreeVec(buf); Close(fh);
    fail(j, g_quitting ? JS_CANCEL : JS_NET, "connection dropped during the upload", NULL);
}

/* ------------------------------------------------------------------ *
 * Jobs
 * ------------------------------------------------------------------ */

static void job_exec(struct Worker *wk, struct Job *j)
{
    ULONG n = slen(j->arg);
    UBYTE *pl = (UBYTE *)AllocVec(n + 2, MEMF_ANY);
    ULONG i;
    if (!pl) { j->status = JS_NET; scopy(j->err, "out of memory", sizeof j->err); return; }
    pl[0] = (UBYTE)(j->deadline >> 8); pl[1] = (UBYTE)j->deadline;
    for (i = 0; i < n; i++) pl[2 + i] = (UBYTE)j->arg[i];
    /* The agent answers when the command ends or the deadline passes. */
    request(wk, j, CMD_EXEC, pl, n + 2, (ULONG)(j->deadline + 15) * 1000UL);
    FreeVec(pl);
    if (j->status == JS_OK && j->outlen >= 4) {
        /* rc first, then output: shift the output down, keep it terminated. */
        ULONG k;
        j->rc = (LONG)get_be32(j->out);
        for (k = 4; k <= j->outlen; k++) j->out[k - 4] = j->out[k];
        j->outlen -= 4;
    }
}

static void job_scan(struct Worker *wk, struct Job *j)
{
    ULONG base = j->scan_net & 0xFFFFFF00UL;
    ULONG host;
    LONG socks[SCAN_BATCH];
    UBYTE octs[SCAN_BATCH];

    for (host = 0; host < 256; host++) j->scan_hit[host] = SCAN_NONE;
    j->scan_done = 0;

    for (host = 1; host < 255 && !g_quitting; ) {
        int n = 0, i;
        ULONG t0 = now_ms(wk);

        while (n < SCAN_BATCH && host < 255) {
            LONG s = connect_start(wk, base | host, j->port);
            if (s >= 0) { socks[n] = s; octs[n] = (UBYTE)host; n++; }
            host++;
        }

        /* Collect answers until everyone has spoken or the batch time is up. */
        for (;;) {
            fd_set wr;
            struct timeval tv;
            ULONG sigs = SIGBREAKF_CTRL_C, el;
            LONG maxfd = -1, r;
            int pending = 0;

            FD_ZERO(&wr);
            for (i = 0; i < n; i++)
                if (socks[i] >= 0) { FD_SET(socks[i], &wr); if (socks[i] > maxfd) maxfd = socks[i]; pending++; }
            if (!pending) break;
            el = now_ms(wk) - t0;
            if (el >= SCAN_WAIT_MS) break;
            tv.tv_secs = 0; tv.tv_micro = (SCAN_WAIT_MS - el) * 1000UL;
            r = WaitSelect(maxfd + 1, NULL, &wr, NULL, &tv, &sigs);
            if ((sigs & SIGBREAKF_CTRL_C) || r <= 0) break;
            for (i = 0; i < n; i++) {
                if (socks[i] >= 0 && FD_ISSET(socks[i], &wr)) {
                    if (connect_ok(wk, socks[i])) j->scan_hit[octs[i]] = SCAN_OPEN;
                    CloseSocket(socks[i]);
                    socks[i] = -1;
                }
            }
        }
        for (i = 0; i < n; i++) if (socks[i] >= 0) CloseSocket(socks[i]);
        j->scan_done = host - 1;
    }

    /* Anything listening on the agent port: is it an agent, and does it take
     * our token? */
    for (host = 1; host < 255 && !g_quitting; host++) {
        struct Job probe;
        if (j->scan_hit[host] != SCAN_OPEN) continue;
        probe = *j;
        fmt_quad(probe.host, base | host);
        request(wk, &probe, CMD_PING, NULL, 0, 4000);
        if (probe.status == JS_OK) j->scan_hit[host] = SCAN_AGENT;
        else if (probe.status == JS_AUTH) j->scan_hit[host] = SCAN_LOCKED;
        else if (probe.status == JS_ERR && probe.err[0] == 'b')  /* "bad token" */
            j->scan_hit[host] = SCAN_LOCKED;
        if (probe.out) FreeVec(probe.out);
    }
    j->status = g_quitting ? JS_CANCEL : JS_OK;
}

/* SHOT reply -> a 0RGB frame the view can blit directly. Done here, on the
 * worker, so converting a 2-megapixel RTG frame never stalls the GUI. */
static void job_shot(struct Worker *wk, struct Job *j)
{
    ULONG t0 = now_ms(wk);
    const UBYTE *p;
    UBYTE fmt;
    UWORD w, h, nc;
    ULONG n, i, need;
    ULONG *fb;

    request(wk, j, CMD_SHOT, NULL, 0, 60000);
    j->fetch_ms = now_ms(wk) - t0;
    if (j->status != JS_OK) return;
    p = j->out;
    if (j->outlen < 8) goto bad;
    fmt = p[0];
    w = (UWORD)((p[2] << 8) | p[3]);
    h = (UWORD)((p[4] << 8) | p[5]);
    nc = (UWORD)((p[6] << 8) | p[7]);
    n = (ULONG)w * h;
    if (!n || nc > 256) goto bad;
    need = 8 + (fmt == 1 ? (ULONG)nc * 3 + n : n * 3);
    if ((fmt != 1 && fmt != 2) || j->outlen < need) goto bad;

    fb = (ULONG *)AllocVec(n * 4, MEMF_ANY);
    if (!fb) {
        FreeVec(j->out); j->out = NULL;
        j->status = JS_ERR; scopy(j->err, "not enough memory for the frame", sizeof j->err);
        return;
    }
    if (fmt == 1) {
        ULONG lut[256];
        const UBYTE *pal = p + 8, *px = p + 8 + (ULONG)nc * 3;
        for (i = 0; i < 256; i++)
            lut[i] = i < nc ? ((ULONG)pal[i * 3] << 16) | ((ULONG)pal[i * 3 + 1] << 8) | pal[i * 3 + 2] : 0;
        for (i = 0; i < n; i++) fb[i] = lut[px[i]];
    } else {
        const UBYTE *px = p + 8;
        for (i = 0; i < n; i++, px += 3)
            fb[i] = ((ULONG)px[0] << 16) | ((ULONG)px[1] << 8) | px[2];
    }
    FreeVec(j->out);
    j->out = (UBYTE *)fb;
    j->outlen = n * 4;
    j->fw = w; j->fh = h;
    return;
bad:
    FreeVec(j->out); j->out = NULL; j->outlen = 0;
    j->status = JS_ERR;
    scopy(j->err, "malformed SHOT reply", sizeof j->err);
}


/* ------------------------------------------------------------------ *
 * Copying between machines
 *
 * Same agent at both ends: one AmigaDOS "Copy ... ALL CLONE" on that
 * machine - fastest, keeps dates and bits. Different machines: stream every
 * file from the source's GETRANGE straight into the destination's PUT,
 * 64 KB at a time, recursing through drawers here on the worker. A PUT frame
 * is capped at 16 MiB, so bigger files go in 8 MB parts that the
 * destination Joins.
 *
 * Deadlock trap: one agent serves ONE connection at a time. Holding a PUT
 * open to machine B while asking machine B for GETRANGE would wait forever,
 * which is why "same agent" must be detected by address (127.0.0.1 on this
 * Amiga is the same agent as this Amiga's LAN address), not by name.
 * ------------------------------------------------------------------ */

#define COPY_CHUNK  (256UL * 1024UL)   /* 64 KB: ~150 KB/s, per-request cost dominated */
#define COPY_PART   (8UL * 1024UL * 1024UL)
#define COPY_SINGLE (15UL * 1024UL * 1024UL)

static void wjoin(char *d, const char *dir, const char *name, ULONG n)
{
    ULONG l;
    scopy(d, dir, n);
    l = slen(d);
    if (l && d[l - 1] != ':' && d[l - 1] != '/' && l + 1 < n) { d[l++] = '/'; d[l] = 0; }
    scopy(d + l, name, n - l);
}

/* Append a quoted AmigaDOS argument ("*" escapes " and *). */
static void qcat(char *d, const char *arg, ULONG n)
{
    ULONG l = slen(d);
    if (l + 3 >= n) return;
    d[l++] = ' '; d[l++] = '"';
    while (*arg && l + 3 < n) {
        if (*arg == '"' || *arg == '*') d[l++] = '*';
        d[l++] = *arg++;
    }
    d[l++] = '"'; d[l] = 0;
}

static void cat(char *d, const char *s, ULONG n)
{
    ULONG l = slen(d);
    scopy(d + l, s, n - l);
}

static void num(char *d, ULONG v, ULONG n)
{
    char t[12];
    int i = 11;
    t[i] = 0;
    do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v && i > 0);
    cat(d, t + i, n);
}

/* EXEC on the agent described by t; the rc, or -1 (err in t->err). */
static LONG wexec(struct Worker *wk, struct Job *t, const char *cmd, UWORD deadline)
{
    UBYTE pl[2 + 2048];
    ULONG n = slen(cmd), i;
    LONG rc = -1;
    if (n > sizeof pl - 2) n = sizeof pl - 2;
    pl[0] = (UBYTE)(deadline >> 8); pl[1] = (UBYTE)deadline;
    for (i = 0; i < n; i++) pl[2 + i] = (UBYTE)cmd[i];
    request(wk, t, CMD_EXEC, pl, n + 2, (ULONG)(deadline + 15) * 1000UL);
    if (t->status == JS_OK && t->outlen >= 4) {
        rc = (LONG)get_be32(t->out);
        if (rc != 0) {      /* keep what the command said */
            ULONG k, m = t->outlen - 4 < sizeof t->err - 1 ? t->outlen - 4 : sizeof t->err - 1;
            for (k = 0; k < m; k++) t->err[k] = (char)(t->out[4 + k] < 32 ? ' ' : t->out[4 + k]);
            t->err[m] = 0;
        }
    }
    if (t->out) { FreeVec(t->out); t->out = NULL; }
    return rc;
}

static void agent_of(struct Job *t, const char *host, UWORD port, const char *token)
{
    UBYTE *z = (UBYTE *)t; ULONG i;
    for (i = 0; i < sizeof *t; i++) z[i] = 0;
    scopy(t->host, host, sizeof t->host);
    scopy(t->token, token, sizeof t->token);
    t->port = port;
}

struct Copier {
    struct Worker *wk;
    struct CopySpec *cs;
    struct Job src, dst;        /* agent templates */
    UBYTE *buf;
};

static int stopped(struct Copier *c) { return c->cs->cancel || g_quitting; }

static int fail_at(struct Copier *c, const char *what, const char *path, const char *why)
{
    struct CopySpec *cs = c->cs;
    if (cs->err[0]) return 0;
    scopy(cs->err, what, sizeof cs->err);
    cat(cs->err, path, sizeof cs->err);
    if (why && why[0]) { cat(cs->err, ": ", sizeof cs->err); cat(cs->err, why, sizeof cs->err); }
    return 0;
}

/* One PUT of [off, off+len) of src into dst, streamed. */
static int put_range(struct Copier *c, const char *src, const char *dst, ULONG off, ULONG len)
{
    struct Worker *wk = c->wk;
    struct Job d = c->dst;
    UBYTE h[AMI_HDRLEN + 2], pl[8 + 256];
    ULONG n = slen(dst), sn = slen(src), done = 0, i, t0 = now_ms(wk);
    LONG s;

    if (sn > 256) sn = 256;
    for (i = 0; i < sn; i++) pl[8 + i] = (UBYTE)src[i];
    if ((s = agent_open(wk, &d, 20000)) < 0) return fail_at(c, "cannot reach the destination for ", dst, d.err);
    h[0] = AMI_MAGIC0; h[1] = AMI_MAGIC1; h[2] = AMI_MAGIC2; h[3] = AMI_MAGIC3;
    h[4] = CMD_PUT; h[5] = 0; h[6] = 0; h[7] = 0;
    put_be32(h + 8, 2 + n + len);
    h[12] = (UBYTE)(n >> 8); h[13] = (UBYTE)n;
    if (!send_all(wk, s, h, sizeof h) || !send_all(wk, s, (const UBYTE *)dst, n)) {
        CloseSocket(s);
        return fail_at(c, "connection dropped writing ", dst, NULL);
    }
    while (done < len) {
        struct Job g = c->src;
        ULONG k = len - done > COPY_CHUNK ? COPY_CHUNK : len - done;
        if (stopped(c)) { CloseSocket(s); return fail_at(c, "cancelled at ", src, NULL); }
        put_be32(pl, off + done);
        put_be32(pl + 4, k);
        request(wk, &g, CMD_GETRANGE, pl, 8 + sn, 60000);
        if (g.status != JS_OK || g.outlen != k) {
            if (g.out) FreeVec(g.out);
            CloseSocket(s);
            return fail_at(c, "cannot read ", src, g.status != JS_OK ? g.err : "short read");
        }
        if (!send_all(wk, s, g.out, k)) {
            FreeVec(g.out);
            CloseSocket(s);
            return fail_at(c, "connection dropped writing ", dst, NULL);
        }
        FreeVec(g.out);
        done += k;
        c->cs->bytes += k;
    }
    agent_finish(wk, &d, s, t0, 60000);
    if (d.out) FreeVec(d.out);
    if (d.status != JS_OK) return fail_at(c, "the destination refused ", dst, d.err);
    return 1;
}

static int copy_file(struct Copier *c, const char *src, const char *dst, ULONG size)
{
    char cmd[2048], part[300];
    ULONG parts, i;
    int ok = 1;

    if (size <= COPY_SINGLE) return put_range(c, src, dst, 0, size);

    /* Big: 8 MB parts beside the target, Joined there, then deleted. */
    parts = (size + COPY_PART - 1) / COPY_PART;
    for (i = 0; i < parts && ok; i++) {
        scopy(part, dst, sizeof part); cat(part, ".amifleet-part", sizeof part); num(part, i, sizeof part);
        ok = put_range(c, src, part, i * COPY_PART,
                       size - i * COPY_PART > COPY_PART ? COPY_PART : size - i * COPY_PART);
    }
    if (ok) {
        struct Job d = c->dst;
        scopy(cmd, "Join", sizeof cmd);
        for (i = 0; i < parts; i++) {
            scopy(part, dst, sizeof part); cat(part, ".amifleet-part", sizeof part); num(part, i, sizeof part);
            qcat(cmd, part, sizeof cmd);
        }
        cat(cmd, " AS", sizeof cmd);
        qcat(cmd, dst, sizeof cmd);
        if (wexec(c->wk, &d, cmd, 600) != 0) ok = fail_at(c, "Join failed for ", dst, d.err);
    }
    {   /* the parts go either way */
        struct Job d = c->dst;
        scopy(cmd, "Delete", sizeof cmd);
        scopy(part, dst, sizeof part); cat(part, ".amifleet-part#?", sizeof part);
        qcat(cmd, part, sizeof cmd);
        cat(cmd, " QUIET", sizeof cmd);
        wexec(c->wk, &d, cmd, 60);
    }
    return ok;
}

/* A drawer: create it at the destination, then everything in it. */
static int copy_item(struct Copier *c, const char *src, const char *dst, int isdir, ULONG size);

static int copy_dir(struct Copier *c, const char *src, const char *dst)
{
    struct Job d = c->dst, g = c->src;
    char cmd[400];
    char *p;
    int ok = 1;

    scopy(cmd, "MakeDir", sizeof cmd);
    qcat(cmd, dst, sizeof cmd);
    wexec(c->wk, &d, cmd, 30);          /* fails harmlessly if it exists */

    request(c->wk, &g, CMD_LIST, (const UBYTE *)src, slen(src), 60000);
    if (g.status != JS_OK) { if (g.out) FreeVec(g.out); return fail_at(c, "cannot list ", src, g.err); }

    /* Lines: type TAB size TAB prot TAB date TAB name */
    for (p = (char *)g.out; ok && p && *p; ) {
        char *nl = p, *f[5];
        int k = 0;
        char s2[300], d2[300];
        while (*nl && *nl != '\n') nl++;
        if (*nl) *nl++ = 0; else nl = NULL;
        f[0] = p;
        for (k = 1; k < 5; k++) {
            char *q = f[k - 1];
            while (*q && *q != '\t') q++;
            if (!*q) break;
            *q++ = 0;
            f[k] = q;
        }
        if (k == 5) {
            ULONG sz = 0;
            char *q = f[1];
            while (*q >= '0' && *q <= '9') sz = sz * 10 + (ULONG)(*q++ - '0');
            wjoin(s2, src, f[4], sizeof s2);
            wjoin(d2, dst, f[4], sizeof d2);
            ok = copy_item(c, s2, d2, f[0][0] == 'D', sz);
        }
        p = nl;
    }
    FreeVec(g.out);
    return ok;
}

static int copy_item(struct Copier *c, const char *src, const char *dst, int isdir, ULONG size)
{
    int ok;
    if (stopped(c)) return fail_at(c, "cancelled at ", src, NULL);
    {   /* name only, for the progress line */
        const char *b = src, *q;
        for (q = src; *q; q++) if (*q == '/' || *q == ':') b = q + 1;
        scopy(c->cs->current, b, sizeof c->cs->current);
    }
    ok = isdir ? copy_dir(c, src, dst) : copy_file(c, src, dst, size);
    if (ok && !isdir) c->cs->files++;
    return ok;
}

static ULONG norm_addr(struct Worker *wk, ULONG a)
{
    if ((a >> 24) == 127) { ULONG me = (ULONG)gethostid(); return me ? me : a; }
    return a;
}

static void job_copy(struct Worker *wk, struct Job *j)
{
    struct CopySpec *cs = j->cs;
    struct Copier c;
    UWORD i;
    int ok = 1;
    ULONG a, b;

    SetSignal(0, SIGBREAKF_CTRL_C);         /* a stale cancel from last time */
    c.wk = wk; c.cs = cs; c.buf = NULL;
    agent_of(&c.src, cs->shost, cs->sport, cs->stoken);
    agent_of(&c.dst, cs->dhost, cs->dport, cs->dtoken);
    cs->err[0] = 0;
    cs->files = 0; cs->bytes = 0;

    a = resolve(wk, cs->shost);
    b = resolve(wk, cs->dhost);
    if (!a || !b) { scopy(cs->err, "unknown host", sizeof cs->err); j->status = JS_ERR; return; }
    cs->same = norm_addr(wk, a) == norm_addr(wk, b) && cs->sport == cs->dport;

    for (i = 0; i < cs->n && ok; i++) {
        char src[300], dst[300];
        wjoin(src, cs->srcdir, cs->name[i], sizeof src);
        wjoin(dst, cs->dstdir, cs->name[i], sizeof dst);
        if (cs->same) {
            /* One Copy on that machine does it all, drawers included. */
            struct Job t = c.src;
            char cmd[800];
            if (stopped(&c)) { ok = fail_at(&c, "cancelled at ", src, NULL); break; }
            scopy(cs->current, cs->name[i], sizeof cs->current);
            scopy(cmd, "Copy", sizeof cmd);
            qcat(cmd, src, sizeof cmd);
            cat(cmd, " TO", sizeof cmd);
            qcat(cmd, dst, sizeof cmd);
            cat(cmd, cs->isdir[i] ? " ALL CLONE QUIET" : " CLONE QUIET", sizeof cmd);
            if (wexec(wk, &t, cmd, 3600) != 0) ok = fail_at(&c, "Copy failed for ", src, t.err);
            else { cs->files++; cs->bytes += cs->size[i]; }
        } else {
            ok = copy_item(&c, src, dst, cs->isdir[i], cs->size[i]);
        }
    }
    j->status = ok ? JS_OK : (cs->cancel ? JS_CANCEL : JS_ERR);
    if (j->status == JS_CANCEL && !g_quitting) j->status = JS_ERR;   /* show the message */
}

static void run_job(struct Worker *wk, struct Job *j)
{
    j->ms = 0; j->rc = 0;
    switch (j->type) {
    case JOB_POLL:
        request(wk, j, CMD_INFO, NULL, 0, 5000);
        break;
    case JOB_HELLO:
        request(wk, j, CMD_HELLO, (const UBYTE *)j->arg, slen(j->arg), 5000);
        break;
    case JOB_EXEC:
        job_exec(wk, j);
        break;
    case JOB_BREAK:
        request(wk, j, CMD_BREAK, NULL, 0, 5000);
        break;
    case JOB_LIST:
        request(wk, j, CMD_LIST, (const UBYTE *)j->arg, slen(j->arg), 20000);
        break;
    case JOB_REXXPORTS:
        request(wk, j, CMD_REXXPORTS, NULL, 0, 5000);
        break;
    case JOB_SCREENS:
        request(wk, j, CMD_SCREENS, NULL, 0, 5000);
        break;
    case JOB_SCAN:
        job_scan(wk, j);
        break;
    case JOB_HASH:
        request(wk, j, CMD_HASH, NULL, 0, 20000);
        if (j->status == JS_OK && j->outlen >= 4) j->hash = get_be32(j->out);
        break;
    case JOB_SHOT:
        job_shot(wk, j);
        break;
    case JOB_INPUT:
        request(wk, j, CMD_INPUT, (const UBYTE *)j->arg, j->arglen, 10000);
        break;
    case JOB_VNC:
        job_vnc(wk, j);
        break;
    case JOB_GET:
        job_get(wk, j);
        break;
    case JOB_PUT:
        job_put(wk, j);
        break;
    case JOB_COPY:
        job_copy(wk, j);
        break;
    case JOB_FOP:
        job_exec(wk, j);
        break;
    case JOB_HOSTID:
        j->scan_net = (ULONG)gethostid();
        j->status = JS_OK;
        break;
    default:
        j->status = JS_ERR;
        scopy(j->err, "unknown job", sizeof j->err);
    }
}

/* ------------------------------------------------------------------ *
 * The process
 * ------------------------------------------------------------------ */

/* Handed from worker_start to the new process; worker_start waits for the
 * ready signal before starting the next one, so one slot is enough. */
static struct Worker *volatile g_handoff;

static void worker_close(struct Worker *wk)
{
    if (wk->sb) { CloseLibrary(wk->sb); wk->sb = NULL; }
    if (wk->treq) {
        if (wk->tb) CloseDevice((struct IORequest *)wk->treq);
        DeleteIORequest((struct IORequest *)wk->treq);
        wk->treq = NULL;
    }
    wk->tb = NULL;
    if (wk->tport) { DeleteMsgPort(wk->tport); wk->tport = NULL; }
}

static void worker_main(void)
{
    struct Worker *wk = g_handoff;
    struct Job *quit = NULL;

    wk->port = CreateMsgPort();
    wk->sb = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    wk->tport = CreateMsgPort();
    if (wk->tport) {
        wk->treq = (struct timerequest *)CreateIORequest(wk->tport, sizeof(struct timerequest));
        if (wk->treq && OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ,
                                   (struct IORequest *)wk->treq, 0) == 0)
            wk->tb = wk->treq->tr_node.io_Device;
    }
    wk->ok = wk->port && wk->sb;

    if (!wk->ok) {
        worker_close(wk);
        if (wk->port) { DeleteMsgPort(wk->port); wk->port = NULL; }
        Forbid();       /* the process ends before the GUI can run again */
        Signal(wk->parent, 1UL << wk->readysig);
        return;
    }
    Signal(wk->parent, 1UL << wk->readysig);

    while (!quit) {
        struct Job *j;
        WaitPort(wk->port);
        while ((j = (struct Job *)GetMsg(wk->port))) {
            if (j->type == JOB_QUIT) { quit = j; continue; }
            if (g_quitting) { j->status = JS_CANCEL; j->out = NULL; j->outlen = 0; }
            else run_job(wk, j);
            ReplyMsg(&j->msg);
        }
    }

    worker_close(wk);
    DeleteMsgPort(wk->port);
    wk->port = NULL;
    /* Forbid, then reply: the GUI may unload our code the moment it has the
     * QUIT back, and this process must be gone by then. Exiting a task
     * breaks the Forbid. */
    Forbid();
    quit->status = JS_OK;
    ReplyMsg(&quit->msg);
}

int worker_start(struct Worker *wk, const char *name, ULONG stack)
{
    BYTE sig = AllocSignal(-1);
    if (sig < 0) return 0;

    wk->name = name;
    wk->parent = FindTask(NULL);
    wk->readysig = sig;
    wk->ok = 0;
    SetSignal(0, 1UL << sig);
    g_handoff = wk;

    wk->proc = CreateNewProcTags(
        NP_Entry,     (ULONG)worker_main,
        NP_Name,      (ULONG)name,
        NP_StackSize, stack,
        NP_Priority,  0,
        NP_Output,    0,
        NP_Input,     0,
        NP_CloseOutput, FALSE,
        NP_CloseInput,  FALSE,
        TAG_DONE);
    if (wk->proc) Wait(1UL << sig);
    FreeSignal(sig);
    if (!wk->proc || !wk->ok) { wk->proc = NULL; return 0; }
    return 1;
}
