/*
 * net.h - socket plumbing shared by the worker processes (worker.c, vnc.c).
 *
 * Every function here takes the calling worker, because the bsdsocket and
 * timer inlines are pointed at THAT worker's library bases: a socket base
 * belongs to the task that opened it. Only ever include this from code that
 * runs on a worker process, never from the GUI.
 */

#ifndef AMIFLEET_NET_H
#define AMIFLEET_NET_H

#include <sys/types.h>
#ifndef _SSIZE_T_DECLARED
typedef long ssize_t;
#define _SSIZE_T_DECLARED
#endif

#include <exec/types.h>

#define BSDSOCKET_BASE_NAME wk->sb
#define TIMER_BASE_NAME     wk->tb
#include <proto/bsdsocket.h>
#include <proto/timer.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/filio.h>
#include <sys/time.h>
#include <netdb.h>

#include "fleet.h"
#include "proto.h"

/* no-libc helpers */
void  scopy(char *d, const char *s, ULONG n);
ULONG slen(const char *s);
void  put_be32(UBYTE *p, ULONG v);
ULONG get_be32(const UBYTE *p);
ULONG now_ms(struct Worker *wk);

/* sockets: 1 ready / 0 timeout / -1 cancelled (Ctrl-C) or error */
int   sock_wait(struct Worker *wk, LONG s, int write, ULONG ms);
int   send_all(struct Worker *wk, LONG s, const UBYTE *p, ULONG n);
int   recv_all(struct Worker *wk, LONG s, UBYTE *p, ULONG n, ULONG ms);
ULONG resolve(struct Worker *wk, const char *host);
LONG  connect_start(struct Worker *wk, ULONG addr, UWORD port);
int   connect_ok(struct Worker *wk, LONG s);

/* One amiagent conversation (connect, AUTH, request, response) for job j. */
void  request(struct Worker *wk, struct Job *j, UBYTE code,
              const UBYTE *pl, ULONG pllen, ULONG ms);

/* vnc.c: runs a whole RFB session; returns when it ends. */
void  job_vnc(struct Worker *wk, struct Job *j);

#endif
