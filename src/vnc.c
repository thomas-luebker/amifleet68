/*
 * vnc.c - an RFB 3.3 client, run as one long job on its own worker process.
 *
 * Aimed at AmiVNC on other Amigas, and shaped by what amimcp's macOS amifleet
 * learned talking to it (RFBClient.swift / VNCLauncher.swift):
 *
 *  - Speak RFB 3.3 whatever the server offers - the simplest security flow,
 *    and what AmiVNC speaks.
 *  - Never SetPixelFormat. Adapt to the format ServerInit announces (8/16/32
 *    bpp, true colour or palette) and advertise only Raw + CopyRect, so the
 *    2001-era server never has to do anything clever.
 *  - AmiVNC quirk: 16-bit pixels arrive in the Amiga's native BIG-endian
 *    order while the format still says little-endian. Read 16-bit pixels
 *    big-endian regardless (32-bit ones do honour the flag).
 *  - Starting it through amiagent: `AmiVNC.020 -p<pw>` stores the password
 *    (max 7 characters - an 8-character one is stored one short and every
 *    login fails), then `Run >NIL: <NIL: AmiVNC.020 -s<port> [-a]`. The <NIL:
 *    is load-bearing: without it the server dies with the launching shell.
 *    -a asks for BGR233, one byte per pixel - the "fast" mode, and the
 *    sensible default for a 68k viewer.
 *
 * The frame buffer is 0RGB ULONGs, shared with the GUI's view object. Pixel
 * writes take the session semaphore one row at a time (never across network
 * I/O), and the GUI draws under a shared lock - without it a redraw caught
 * the next update half-written and the picture tore.
 *
 * Runs on a worker process: no malloc, no stdio (see worker.c).
 */

#ifndef __amigaos__
#error "amifleet68 targets AmigaOS - build with m68k-amigaos-gcc (see Makefile)"
#endif

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "net.h"
#include "des.h"

#define AMIVNC_EXE  "SYS:Programs/AmiVNC/AmiVNC.020"
#define READ_MS     15000UL
/* Pacing. The first session against a real AmiVNC (PiStorm, 2026-09-30) ran
 * ~37 request/update round trips a second - the server answers at once - and
 * the PiStorm crashed 25 s in, its agent starved before that. Never ask for
 * the next update sooner than this; back off further when nothing changed. */
#define REQ_MIN_MS    100UL      /* <= 10 updates a second */
#define REQ_IDLE_MS   250UL      /* the last update was empty */
#define MAX_DIM     4096

struct Rfb {
    struct Worker     *wk;
    struct VncSession *vs;
    LONG   s;
    UWORD  w, h;
    UBYTE  bypp, big, truecol;
    UWORD  rmax, gmax, bmax;
    UBYTE  rsh, gsh, bsh;
    ULONG  pal[256];
    ULONG *lut;             /* 1 and 2 byte pixels: value -> 0RGB */
    ULONG  lutlen;
    UBYTE *buf;             /* one row of wire pixels, or scratch */
    ULONG  bufcap;
    ULONG  bytes;
    UWORD  last_rects;
    char   err[100];
};

/* ------------------------------------------------------------------ *
 * small helpers
 * ------------------------------------------------------------------ */

static void scat(char *d, const char *s, ULONG n)
{
    ULONG l = slen(d);
    if (l + 1 < n) scopy(d + l, s, n - l);
}

static void ucat(char *d, ULONG v, ULONG n)
{
    char t[12];
    int i = 11;
    t[i] = 0;
    do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v && i > 0);
    scat(d, t + i, n);
}

static void status(struct VncSession *vs, UBYTE state, const char *a, const char *b)
{
    ObtainSemaphore(&vs->sem);
    if (state) vs->state = state;
    if (a) {
        scopy(vs->status, a, sizeof vs->status);
        if (b) scat(vs->status, b, sizeof vs->status);
    }
    ReleaseSemaphore(&vs->sem);
    Signal(vs->gui, vs->gui_sig);
}

static int rd(struct Rfb *r, void *p, ULONG n)
{
    if (!recv_all(r->wk, r->s, (UBYTE *)p, n, READ_MS)) return 0;
    r->bytes += n;
    return 1;
}

static int wr(struct Rfb *r, const void *p, ULONG n)
{
    return send_all(r->wk, r->s, (const UBYTE *)p, n);
}

static int rd16(struct Rfb *r, UWORD *v)
{
    UBYTE b[2];
    if (!rd(r, b, 2)) return 0;
    *v = (UWORD)((b[0] << 8) | b[1]);
    return 1;
}

static int rd32(struct Rfb *r, ULONG *v)
{
    UBYTE b[4];
    if (!rd(r, b, 4)) return 0;
    *v = get_be32(b);
    return 1;
}

static int ensure_buf(struct Rfb *r, ULONG n)
{
    if (n <= r->bufcap) return 1;
    if (r->buf) FreeVec(r->buf);
    r->buf = (UBYTE *)AllocVec(n, MEMF_ANY);
    r->bufcap = r->buf ? n : 0;
    return r->buf != NULL;
}

/* ------------------------------------------------------------------ *
 * pixels
 * ------------------------------------------------------------------ */

static ULONG scale8(ULONG v, UWORD max)
{
    if (max == 255) return v & 255;
    if (!max) return 0;
    v = v * 255 / max;
    return v > 255 ? 255 : v;
}

static ULONG to_rgb(struct Rfb *r, ULONG v)
{
    if (!r->truecol) return r->pal[v & 255];
    return (scale8((v >> r->rsh) & r->rmax, r->rmax) << 16) |
           (scale8((v >> r->gsh) & r->gmax, r->gmax) << 8) |
            scale8((v >> r->bsh) & r->bmax, r->bmax);
}

static int build_lut(struct Rfb *r)
{
    ULONG i, n = r->bypp == 1 ? 256UL : r->bypp == 2 ? 65536UL : 0;
    if (!n) return 1;
    if (!r->lut) {
        r->lut = (ULONG *)AllocVec(n * 4, MEMF_ANY);
        if (!r->lut) return 0;
        r->lutlen = n;
    }
    for (i = 0; i < n; i++) r->lut[i] = to_rgb(r, i);
    return 1;
}

/* Wire pixels -> 0RGB, count of them. */
static void convert(struct Rfb *r, const UBYTE *p, ULONG *d, ULONG count)
{
    ULONG i;
    switch (r->bypp) {
    case 1:
        for (i = 0; i < count; i++) d[i] = r->lut[p[i]];
        break;
    case 2:     /* big-endian regardless of the flag: the AmiVNC quirk */
        for (i = 0; i < count; i++, p += 2) d[i] = r->lut[(p[0] << 8) | p[1]];
        break;
    default:
        for (i = 0; i < count; i++, p += 4) {
            ULONG v = r->big ? get_be32(p)
                             : (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
            d[i] = to_rgb(r, v);
        }
    }
}

/* ------------------------------------------------------------------ *
 * the conversation
 * ------------------------------------------------------------------ */

static int handshake(struct Rfb *r)
{
    struct VncSession *vs = r->vs;
    UBYTE ver[12], si[24];
    ULONG sec, namelen;

    if (!rd(r, ver, 12) || ver[0] != 'R' || ver[1] != 'F' || ver[2] != 'B') {
        scopy(r->err, "not a VNC server", sizeof r->err);
        return 0;
    }
    if (!wr(r, "RFB 003.003\n", 12) || !rd32(r, &sec)) {
        scopy(r->err, "connection dropped in the handshake", sizeof r->err);
        return 0;
    }
    if (sec == 0) {
        ULONG n = 0;
        scopy(r->err, "server refused: ", sizeof r->err);
        if (rd32(r, &n) && n && n < 200 && ensure_buf(r, n + 1) && rd(r, r->buf, n)) {
            r->buf[n] = 0;
            scat(r->err, (char *)r->buf, sizeof r->err);
        }
        return 0;
    }
    if (sec == 2) {
        UBYTE ch[16], resp[16];
        ULONG res;
        if (!rd(r, ch, 16)) { scopy(r->err, "no auth challenge", sizeof r->err); return 0; }
        vnc_auth_response(vs->password, ch, resp);
        if (!wr(r, resp, 16) || !rd32(r, &res)) {
            scopy(r->err, "connection dropped during login", sizeof r->err);
            return 0;
        }
        if (res != 0) { scopy(r->err, "wrong VNC password", sizeof r->err); return 0; }
    } else if (sec != 1) {
        scopy(r->err, "unsupported VNC security type ", sizeof r->err);
        ucat(r->err, sec, sizeof r->err);
        return 0;
    }

    {   UBYTE shared = 1; if (!wr(r, &shared, 1)) return 0; }
    if (!rd(r, si, 24)) { scopy(r->err, "no ServerInit", sizeof r->err); return 0; }
    r->w = (UWORD)((si[0] << 8) | si[1]);
    r->h = (UWORD)((si[2] << 8) | si[3]);
    r->bypp = (UBYTE)(si[4] / 8);
    r->big = si[6] != 0;
    r->truecol = si[7] != 0;
    r->rmax = (UWORD)((si[8] << 8) | si[9]);
    r->gmax = (UWORD)((si[10] << 8) | si[11]);
    r->bmax = (UWORD)((si[12] << 8) | si[13]);
    r->rsh = si[14]; r->gsh = si[15]; r->bsh = si[16];
    namelen = get_be32(si + 20);

    if ((r->bypp != 1 && r->bypp != 2 && r->bypp != 4) || !r->w || !r->h ||
        r->w > MAX_DIM || r->h > MAX_DIM || namelen > 4096) {
        scopy(r->err, "implausible ServerInit", sizeof r->err);
        return 0;
    }
    if (namelen) {
        if (!ensure_buf(r, namelen + 1) || !rd(r, r->buf, namelen)) return 0;
        r->buf[namelen] = 0;
    }

    {   /* grey palette until the server sends one */
        int i;
        for (i = 0; i < 256; i++) r->pal[i] = ((ULONG)i << 16) | ((ULONG)i << 8) | (ULONG)i;
    }
    if (!build_lut(r)) { scopy(r->err, "out of memory", sizeof r->err); return 0; }

    {   /* the frame, published to the GUI */
        ULONG n = (ULONG)r->w * r->h;
        ULONG *fb = (ULONG *)AllocVec(n * 4, MEMF_ANY | MEMF_CLEAR);
        if (!fb) { scopy(r->err, "not enough memory for the frame", sizeof r->err); return 0; }
        ObtainSemaphore(&vs->sem);
        vs->fb = fb; vs->w = r->w; vs->h = r->h;
        vs->newframe = 1;
        scopy(vs->name, namelen ? (char *)r->buf : "", sizeof vs->name);
        ReleaseSemaphore(&vs->sem);
    }

    {   /* SetEncodings: Raw, CopyRect */
        static const UBYTE enc[12] = { 2, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 1 };
        if (!wr(r, enc, sizeof enc)) return 0;
    }
    return 1;
}

static int request_update(struct Rfb *r, int incremental)
{
    UBYTE m[10];
    m[0] = 3; m[1] = (UBYTE)incremental;
    m[2] = 0; m[3] = 0; m[4] = 0; m[5] = 0;
    m[6] = (UBYTE)(r->w >> 8); m[7] = (UBYTE)r->w;
    m[8] = (UBYTE)(r->h >> 8); m[9] = (UBYTE)r->h;
    return wr(r, m, 10);
}

static int read_raw(struct Rfb *r, UWORD x, UWORD y, UWORD w, UWORD h)
{
    ULONG rowbytes = (ULONG)w * r->bypp, row;
    ULONG *fb = r->vs->fb;
    if (!ensure_buf(r, rowbytes ? rowbytes : 1)) return 0;
    for (row = 0; row < h; row++) {
        if (!rd(r, r->buf, rowbytes)) return 0;
        if (y + row < r->h && x < r->w) {
            ULONG cnt = (ULONG)(x + w <= r->w ? w : r->w - x);
            ObtainSemaphore(&r->vs->sem);
            convert(r, r->buf, fb + (ULONG)(y + row) * r->w + x, cnt);
            ReleaseSemaphore(&r->vs->sem);
        }
    }
    return 1;
}

static int read_copyrect(struct Rfb *r, UWORD x, UWORD y, UWORD w, UWORD h)
{
    UWORD sx, sy;
    ULONG *fb = r->vs->fb;
    LONG row, col;
    if (!rd16(r, &sx) || !rd16(r, &sy)) return 0;
    if (x + w > r->w || y + h > r->h || sx + w > r->w || sy + h > r->h) return 1;
    ObtainSemaphore(&r->vs->sem);
    for (row = 0; row < h; row++) {
        LONG ry = sy < y ? (LONG)h - 1 - row : row;     /* overlap-safe */
        ULONG *d = fb + (ULONG)(y + ry) * r->w + x;
        ULONG *s = fb + (ULONG)(sy + ry) * r->w + sx;
        if (sx < x) for (col = (LONG)w - 1; col >= 0; col--) d[col] = s[col];
        else        for (col = 0; col < (LONG)w; col++) d[col] = s[col];
    }
    ReleaseSemaphore(&r->vs->sem);
    return 1;
}

static int read_update(struct Rfb *r)
{
    struct VncSession *vs = r->vs;
    UBYTE pad;
    UWORD n, i;
    WORD x0 = 32767, y0 = 32767, x1 = -1, y1 = -1;

    if (!rd(r, &pad, 1) || !rd16(r, &n)) return 0;
    r->last_rects = n;
    for (i = 0; i < n; i++) {
        UWORD x, y, w, h;
        ULONG enc;
        if (!rd16(r, &x) || !rd16(r, &y) || !rd16(r, &w) || !rd16(r, &h) || !rd32(r, &enc))
            return 0;
        if (enc == 0) { if (!read_raw(r, x, y, w, h)) return 0; }
        else if (enc == 1) { if (!read_copyrect(r, x, y, w, h)) return 0; }
        else {
            scopy(r->err, "server sent an encoding we did not ask for: ", sizeof r->err);
            ucat(r->err, enc, sizeof r->err);
            return 0;
        }
        if ((WORD)x < x0) x0 = (WORD)x;
        if ((WORD)y < y0) y0 = (WORD)y;
        if ((WORD)(x + w) > x1) x1 = (WORD)(x + w);
        if ((WORD)(y + h) > y1) y1 = (WORD)(y + h);
    }
    ObtainSemaphore(&vs->sem);
    if (x1 > x0 && y1 > y0) {
        if (!vs->dirty) { vs->dx0 = x0; vs->dy0 = y0; vs->dx1 = x1; vs->dy1 = y1; }
        else {
            if (x0 < vs->dx0) vs->dx0 = x0;
            if (y0 < vs->dy0) vs->dy0 = y0;
            if (x1 > vs->dx1) vs->dx1 = x1;
            if (y1 > vs->dy1) vs->dy1 = y1;
        }
        vs->dirty = 1;
    }
    vs->frames++;
    vs->kbytes = r->bytes >> 10;
    ReleaseSemaphore(&vs->sem);
    Signal(vs->gui, vs->gui_sig);
    return 1;
}

static int read_colourmap(struct Rfb *r)
{
    UBYTE pad, e[6];
    UWORD first, count, i;
    if (!rd(r, &pad, 1) || !rd16(r, &first) || !rd16(r, &count)) return 0;
    for (i = 0; i < count; i++) {
        if (!rd(r, e, 6)) return 0;
        if (first + i < 256)
            r->pal[first + i] = ((ULONG)e[0] << 16) | ((ULONG)e[2] << 8) | e[4];
    }
    if (!r->truecol) build_lut(r);
    ObtainSemaphore(&r->vs->sem);    /* every pixel may have changed colour */
    r->vs->dx0 = 0; r->vs->dy0 = 0; r->vs->dx1 = (WORD)r->w; r->vs->dy1 = (WORD)r->h;
    r->vs->dirty = 1;
    ReleaseSemaphore(&r->vs->sem);
    return 1;
}

static int skip(struct Rfb *r, ULONG n)
{
    if (!ensure_buf(r, 4096)) return 0;
    while (n) {
        ULONG k = n > 4096 ? 4096 : n;
        if (!rd(r, r->buf, k)) return 0;
        n -= k;
    }
    return 1;
}

static int send_input(struct Rfb *r)
{
    struct VncSession *vs = r->vs;
    struct VncInput q[VNC_QLEN];
    int n = 0, i;

    ObtainSemaphore(&vs->sem);
    while (vs->qhead != vs->qtail && n < VNC_QLEN) {
        q[n++] = vs->q[vs->qhead];
        vs->qhead = (UWORD)((vs->qhead + 1) % VNC_QLEN);
    }
    ReleaseSemaphore(&vs->sem);

    for (i = 0; i < n; i++) {
        UBYTE m[8];
        if (q[i].type == VE_POINTER) {
            UWORD x = q[i].x < r->w ? q[i].x : (UWORD)(r->w - 1);
            UWORD y = q[i].y < r->h ? q[i].y : (UWORD)(r->h - 1);
            m[0] = 5; m[1] = q[i].arg;
            m[2] = (UBYTE)(x >> 8); m[3] = (UBYTE)x;
            m[4] = (UBYTE)(y >> 8); m[5] = (UBYTE)y;
            if (!wr(r, m, 6)) return 0;
        } else {
            m[0] = 4; m[1] = q[i].arg; m[2] = 0; m[3] = 0;
            put_be32(m + 4, q[i].keysym);
            if (!wr(r, m, 8)) return 0;
        }
    }
    return 1;
}

/* The message pump: returns when the session ends. */
static void pump(struct Rfb *r)
{
    struct Worker *wk = r->wk;
    struct VncSession *vs = r->vs;

    int want_req = 0;
    ULONG next_req = 0;

    if (!request_update(r, 0)) return;
    while (!vs->stop && !g_quitting) {
        fd_set set;
        struct timeval tv;
        ULONG sigs = SIGBREAKF_CTRL_C | vs->in_sig;
        ULONG wait_ms = 1000, now = now_ms(wk);
        LONG n;
        UBYTE type;

        if (want_req) {
            if ((LONG)(now - next_req) >= 0) {
                if (!request_update(r, 1)) { scopy(r->err, "connection lost", sizeof r->err); break; }
                want_req = 0;
            } else wait_ms = next_req - now;
        }

        FD_ZERO(&set);
        FD_SET(r->s, &set);
        tv.tv_secs = wait_ms / 1000; tv.tv_micro = (wait_ms % 1000) * 1000;
        n = WaitSelect(r->s + 1, &set, NULL, NULL, &tv, &sigs);
        if ((sigs & SIGBREAKF_CTRL_C) || vs->stop) break;
        if (!send_input(r)) { scopy(r->err, "connection lost", sizeof r->err); break; }
        if (n < 0) { scopy(r->err, "socket error", sizeof r->err); break; }
        if (n == 0 || !FD_ISSET(r->s, &set)) continue;

        if (!rd(r, &type, 1)) { scopy(r->err, "the server closed the connection", sizeof r->err); break; }
        switch (type) {
        case 0:
            if (!read_update(r)) { if (!r->err[0]) scopy(r->err, "connection lost", sizeof r->err); return; }
            want_req = 1;
            next_req = now_ms(wk) + (r->last_rects ? REQ_MIN_MS : REQ_IDLE_MS);
            break;
        case 1:
            if (!read_colourmap(r)) return;
            break;
        case 2:         /* Bell */
            break;
        case 3: {       /* ServerCutText: read and drop */
            UBYTE pad[3];
            ULONG len;
            if (!rd(r, pad, 3) || !rd32(r, &len) || !skip(r, len)) return;
            break; }
        default:
            scopy(r->err, "unknown server message ", sizeof r->err);
            ucat(r->err, type, sizeof r->err);
            return;
        }
    }
}

/* ------------------------------------------------------------------ *
 * connecting, and starting AmiVNC through the agent
 * ------------------------------------------------------------------ */

static LONG rfb_connect(struct Worker *wk, ULONG addr, UWORD port)
{
    LONG s = connect_start(wk, addr, port), zero = 0;
    if (s < 0) return -1;
    if (sock_wait(wk, s, 1, 3000) != 1 || !connect_ok(wk, s)) { CloseSocket(s); return -1; }
    IoctlSocket(s, FIONBIO, (char *)&zero);
    return s;
}

/* EXEC through the agent; the command's rc, or -1 if the agent said no. */
static LONG agent_exec(struct Worker *wk, struct Job *j, const char *cmd, UWORD deadline)
{
    struct Job a = *j;
    UBYTE pl[300];
    ULONG n = slen(cmd), i;
    LONG rc = -1;
    if (n > sizeof pl - 2) n = sizeof pl - 2;
    pl[0] = (UBYTE)(deadline >> 8); pl[1] = (UBYTE)deadline;
    for (i = 0; i < n; i++) pl[2 + i] = (UBYTE)cmd[i];
    request(wk, &a, CMD_EXEC, pl, n + 2, (ULONG)(deadline + 15) * 1000UL);
    if (a.status == JS_OK && a.outlen >= 4) rc = (LONG)get_be32(a.out);
    if (a.out) FreeVec(a.out);
    if (a.status != JS_OK) scopy(j->err, a.err, sizeof j->err);
    return rc;
}

static int start_amivnc(struct Worker *wk, struct Job *j, struct VncSession *vs)
{
    char cmd[160];

    status(vs, VS_STARTING, "Nothing listening - starting AmiVNC through amiagent...", NULL);
    if (agent_exec(wk, j, "List " AMIVNC_EXE " LFORMAT=%N", 15) != 0) {
        if (j->err[0]) status(vs, 0, "Could not ask the agent: ", j->err);
        else status(vs, 0, "AmiVNC is not installed there. On that Amiga: amipkg install amivnc", NULL);
        return 0;
    }
    scopy(cmd, AMIVNC_EXE " -p", sizeof cmd);
    scat(cmd, vs->password, sizeof cmd);
    agent_exec(wk, j, cmd, 20);

    scopy(cmd, "Run >NIL: <NIL: " AMIVNC_EXE " -s", sizeof cmd);
    ucat(cmd, vs->rfb_port, sizeof cmd);
    if (vs->fast) scat(cmd, " -a", sizeof cmd);
    if (agent_exec(wk, j, cmd, 10) != 0) {
        status(vs, 0, "Starting AmiVNC failed: ", j->err[0] ? j->err : "Run returned an error");
        return 0;
    }
    return 1;
}

void job_vnc(struct Worker *wk, struct Job *j)
{
    struct VncSession *vs = j->vnc;
    struct Rfb r;
    BYTE sig;
    ULONG addr;
    char where[80];
    int tries;

    j->status = JS_OK;
    {
        UBYTE *z = (UBYTE *)&r; ULONG i;
        for (i = 0; i < sizeof r; i++) z[i] = 0;
    }
    r.wk = wk; r.vs = vs; r.s = -1;

    sig = AllocSignal(-1);
    if (sig < 0) { status(vs, VS_CLOSED, "No free signal for the session.", NULL); return; }
    vs->in_sig = 1UL << sig;
    SetSignal(0, SIGBREAKF_CTRL_C | vs->in_sig);   /* a stale stop from last time */
    vs->task = FindTask(NULL);

    scopy(where, vs->host, sizeof where);
    scat(where, ":", sizeof where);
    ucat(where, vs->rfb_port, sizeof where);
    status(vs, VS_CONNECTING, "Connecting to ", where);

    addr = resolve(wk, vs->host);
    if (!addr) { status(vs, VS_CLOSED, "Unknown host ", vs->host); goto out; }

    r.s = rfb_connect(wk, addr, vs->rfb_port);
    if (r.s < 0 && vs->may_start && !vs->stop) {
        if (!start_amivnc(wk, j, vs)) { status(vs, VS_CLOSED, NULL, NULL); goto out; }
        for (tries = 0; tries < 10 && r.s < 0 && !vs->stop && !g_quitting; tries++) {
            Delay(50);
            status(vs, VS_CONNECTING, "Waiting for AmiVNC on ", where);
            r.s = rfb_connect(wk, addr, vs->rfb_port);
        }
    }
    if (r.s < 0) {
        if (!vs->stop) status(vs, VS_CLOSED, "Nothing answers on ", where);
        goto out;
    }

    status(vs, VS_CONNECTING, "Logging in to ", where);
    if (!handshake(&r)) {
        status(vs, VS_CLOSED, "VNC login failed: ", r.err[0] ? r.err : "connection lost");
        goto out;
    }
    status(vs, VS_RUNNING, "Connected to ", vs->name[0] ? vs->name : where);
    pump(&r);
    if (vs->stop || g_quitting) status(vs, VS_CLOSED, "Disconnected.", NULL);
    else status(vs, VS_CLOSED, "Session ended: ", r.err[0] ? r.err : "connection lost");

out:
    /* The status line keeps whatever was said last; just mark it closed. */
    ObtainSemaphore(&vs->sem);
    vs->state = VS_CLOSED;
    ReleaseSemaphore(&vs->sem);
    if (r.s >= 0) CloseSocket(r.s);
    if (r.lut) FreeVec(r.lut);
    if (r.buf) FreeVec(r.buf);
    vs->task = NULL;
    vs->in_sig = 0;
    FreeSignal(sig);
    Signal(vs->gui, vs->gui_sig);
}
