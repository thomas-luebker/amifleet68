/*
 * fleet.h - what the GUI and the network workers share.
 *
 * The GUI never touches a socket. Every network conversation happens on a
 * worker process: the GUI fills in a Job, PutMsg()s it to a worker, and gets
 * the same Job back on its reply port when the conversation is over. That is
 * what keeps the window responsive while a switched-off machine takes its two
 * seconds to time out, or while a 60-second EXEC runs.
 *
 * Each worker opens its OWN bsdsocket.library base - a socket base belongs to
 * the task that opened it - so there is no global SocketBase anywhere.
 */

#ifndef AMIFLEET_FLEET_H
#define AMIFLEET_FLEET_H

#include <exec/types.h>
#include <exec/ports.h>
#include <exec/tasks.h>
#include <dos/dosextens.h>
#include <exec/semaphores.h>

#define AMIFLEET_VERSION "0.5.0"
#define AMIFLEET_VERDATE "1.10.2026"

#define AGENT_PORT 7846

/* Job types. */
enum {
    JOB_QUIT = 0,   /* worker shuts down after replying */
    JOB_POLL,       /* INFO; fills out/outlen with the key=value text + ms */
    JOB_HELLO,      /* HELLO arg - tells the agent's status board who we are */
    JOB_EXEC,       /* EXEC arg with deadline; rc + output */
    JOB_BREAK,      /* BREAK; text reply */
    JOB_LIST,       /* LIST arg (a path); TSV reply */
    JOB_SCAN,       /* sweep net.1-254 for the agent port, PING each hit */
    JOB_HOSTID,     /* this Amiga's own IPv4 address -> scan_net */
    JOB_REXXPORTS,  /* public message ports on the target */
    JOB_SCREENS,    /* open screens, binary reply */
    JOB_HASH,       /* HASH of the front screen -> hash */
    JOB_SHOT,       /* SHOT, converted by the worker to 0RGB -> out, fw x fh */
    JOB_INPUT,      /* INPUT; binary payload in arg[0..arglen) */
    JOB_VNC,        /* a whole RFB session; see struct VncSession */
    JOB_GET,        /* download arg (remote) -> local, GETRANGE chunks */
    JOB_PUT,        /* upload local -> arg (remote), one streamed PUT */
    JOB_COPY,       /* copy items between two machines; see struct CopySpec */
    JOB_FOP         /* a file operation (Delete/Rename/MakeDir) via EXEC */
};

/* Job outcome. */
enum {
    JS_OK = 0,      /* agent answered ST_OK */
    JS_ERR,         /* agent answered ST_ERR; err holds its message */
    JS_AUTH,        /* the agent wants a token (or rejected ours) */
    JS_NET,         /* could not reach it / connection broke; err says why */
    JS_CANCEL       /* the app is quitting */
};

/* Scan results, per host octet. */
#define SCAN_NONE   0
#define SCAN_AGENT  1   /* answered PING with our token */
#define SCAN_LOCKED 2   /* an agent, but the token was refused */
#define SCAN_OPEN   3   /* port open, not recognisably an agent */

struct Job {
    struct Message msg;
    UBYTE  type;
    UBYTE  status;
    UWORD  port;
    ULONG  mid;             /* machine id the job belongs to (0 = none) */
    char   host[64];
    char   token[64];
    char   arg[512];        /* command line / path / hello text */
    UWORD  arglen;          /* non-zero: arg is binary, this long */
    UBYTE  tag;             /* GUI routing: which pane a LIST/FOP belongs to */
    UWORD  deadline;        /* EXEC deadline, seconds */
    LONG   rc;              /* EXEC return code */
    ULONG  ms;              /* round trip, milliseconds */
    UBYTE *out;             /* reply payload, AllocVec'd by the worker, */
    ULONG  outlen;          /* NUL-terminated; the GUI FreeVec()s it     */
    char   err[128];
    /* JOB_HASH / JOB_SHOT */
    ULONG  hash;
    UWORD  fw, fh;          /* SHOT: frame size; out is fw*fh ULONG 0RGB */
    ULONG  fetch_ms;
    /* JOB_GET / JOB_PUT */
    char   local[256];
    ULONG  total;           /* bytes; GET: from the listing (0 = unknown) */
    volatile ULONG done;    /* progress, read by the GUI */
    /* JOB_COPY */
    struct CopySpec *cs;
    /* JOB_VNC */
    struct VncSession *vnc;
    /* JOB_SCAN / JOB_HOSTID */
    ULONG  scan_net;        /* a.b.c.0 as a host-order ULONG */
    volatile ULONG scan_done;  /* hosts probed so far (read by the GUI) */
    UBYTE  scan_hit[256];
};

/* ------------------------------------------------------------------ *
 * A copy between two machines (or within one). Owned by the GUI; the worker
 * writes only the progress fields and err.
 * ------------------------------------------------------------------ */

#define COPY_MAX 64

struct CopySpec {
    char  shost[64], stoken[64];
    UWORD sport;
    char  dhost[64], dtoken[64];
    UWORD dport;
    char  srcdir[256], dstdir[256];
    UWORD n;
    char  name[COPY_MAX][108];
    UBYTE isdir[COPY_MAX];
    ULONG size[COPY_MAX];

    volatile ULONG files, bytes;    /* done so far */
    volatile UBYTE cancel;
    UBYTE same;                     /* worker: both ends are one agent */
    UBYTE move;                     /* delete each source item once it is copied */
    volatile ULONG moved;           /* items deleted at the source */
    char  current[108];
    char  err[160];
};

/* ------------------------------------------------------------------ *
 * A VNC session: shared between the GUI and the worker that runs it.
 * The worker owns the socket; the GUI owns this struct and the frame
 * buffer (freed only after the JOB_VNC has come back).
 * ------------------------------------------------------------------ */

enum { VE_POINTER = 1, VE_KEY };
struct VncInput {
    UBYTE type;
    UBYTE arg;          /* POINTER: button mask; KEY: down */
    UWORD x, y;
    ULONG keysym;
};
#define VNC_QLEN 64

enum { VS_IDLE = 0, VS_STARTING, VS_CONNECTING, VS_RUNNING, VS_CLOSED };

struct VncSession {
    struct SignalSemaphore sem;     /* guards everything below except fb pixels */
    struct Task *gui;               /* signalled on new frame / damage / status */
    ULONG  gui_sig;
    struct Task *volatile task;     /* the worker, once running */
    volatile ULONG in_sig;          /* worker's "input queued" signal mask */
    volatile UBYTE stop;

    /* request, set by the GUI before sending the job */
    char   host[64];
    UWORD  rfb_port;
    char   password[16];
    UBYTE  fast;                    /* AmiVNC -a: BGR233, 1 byte per pixel */
    UBYTE  may_start;               /* the host runs amiagent: start AmiVNC */

    /* state, written by the worker */
    UBYTE  state;
    UBYTE  newframe;                /* fb/w/h changed: re-bind the view */
    UBYTE  dirty;
    WORD   dx0, dy0, dx1, dy1;      /* damaged area, [x0,x1) x [y0,y1) */
    ULONG *fb;
    UWORD  w, h;
    char   status[128];
    char   name[64];
    ULONG  frames, kbytes;

    struct VncInput q[VNC_QLEN];
    UWORD  qhead, qtail;
};

struct Worker {
    const char     *name;
    struct Process *proc;
    struct MsgPort *port;       /* owned by the worker */
    struct Library *sb;         /* the worker's own bsdsocket base */
    struct Device  *tb;         /* timer.device base, for GetSysTime */
    struct MsgPort *tport;
    struct timerequest *treq;
    struct Task    *parent;
    BYTE            readysig;
    BYTE            ok;
};

/* worker.c */
int  worker_start(struct Worker *wk, const char *name, ULONG stack);   /* 1 = running */

/* Set when the app is quitting: workers answer JS_CANCEL without touching
 * the network, and blocking waits give up on the next Ctrl-C. */
extern volatile int g_quitting;

#endif
