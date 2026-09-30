/*
 * amifleet68 - the amiagent fleet console, native on the Amiga, in MUI.
 *
 * The Amiga-side sibling of amimcp/tools/amifleet (the macOS app): one row per
 * machine running amiagent, polled every few seconds for agent version, CPU,
 * OS and free memory, plus a details window with the full INFO report, an
 * AmigaDOS shell that runs through EXEC, and a directory browser over LIST.
 * It speaks the amimcp wire protocol (amimcp/PROTOCOL.md) straight over
 * bsdsocket.library - no MCP server, no Mac in the loop.
 *
 * Structure: this file is the GUI and never touches a socket. Network work
 * runs on three worker processes (worker.c) so that a switched-off machine or
 * a long EXEC never freezes the window:
 *
 *   poll   - the INFO poll of every machine, HELLO, BREAK
 *   shell  - EXEC (one at a time, like the agent itself)
 *   misc   - LIST, the LAN scan, the local address lookup
 *   screen - HASH / SHOT / INPUT for the Screen window
 *   vnc    - one RFB session at a time (vnc.c), for the VNC window
 *
 * BREAK deliberately rides the poll worker: the shell worker is the one stuck
 * waiting on the EXEC that BREAK is meant to interrupt.
 *
 * Needs MUI 3.8+ (muimaster.library v19) and a running TCP stack
 * (bsdsocket.library v4 - Roadshow, AmiTCP, Miami).
 *
 * Headers: vendor/mui/include (MUI 3.8 developer kit, freely distributable).
 * Link: -lamiga (DoMethod, HookEntry). muistubs.c MUST stay its own
 * translation unit - see its header comment.
 */

#ifndef __amigaos__
#error "amifleet68 targets AmigaOS - build with m68k-amigaos-gcc (see Makefile)"
#endif

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <devices/timer.h>
#include <libraries/mui.h>
#include <utility/hooks.h>
#include <libraries/gadtools.h>   /* NM_BARLABEL */
#include <devices/inputevent.h>
#include <intuition/intuition.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <proto/keymap.h>
#include <proto/asl.h>
#include <proto/icon.h>
#include <proto/graphics.h>
#include <workbench/workbench.h>
#include <graphics/modeid.h>
#include <libraries/asl.h>
#include <clib/alib_protos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fleet.h"
#include "view.h"

/* bebbo's startup auto-opens intuition (same arrangement as amimon-mui);
 * muimaster and utility we open by hand. */
struct Library *MUIMasterBase = NULL;
struct Library *UtilityBase = NULL;
struct Library *CyberGfxBase = NULL;   /* optional: RTG drawing in view.c */
struct Library *AslBase = NULL;        /* optional: Download/Upload requesters */

static const char verstag[] __attribute__((used)) =
    "$VER: amifleet68 " AMIFLEET_VERSION " (" AMIFLEET_VERDATE ")";

#ifndef MAKE_ID
#define MAKE_ID(a,b,c,d) ((ULONG)(a)<<24 | (ULONG)(b)<<16 | (ULONG)(c)<<8 | (ULONG)(d))
#endif

#define STACK_BYTES   (64UL * 1024UL)
#define POLL_SECS     5
/* amiagent serves ONE connection at a time: while an EXEC is inside its
 * deadline the machine answers nothing else - not our poll, not BREAK. Keep
 * the deadline short so the agent gives up waiting, says "still running",
 * and becomes reachable again; that is when Break can Ctrl-C the command. */
#define EXEC_DEADLINE 20
#define SHELL_MAX     400      /* lines kept in the shell pane */
#define PREFS_ENV     "ENV:amifleet68.prefs"
#define PREFS_ENVARC  "ENVARC:amifleet68.prefs"
#define HELLO_TEXT    "amifleet68 " AMIFLEET_VERSION " (MUI fleet console)"

enum {
    ID_ABOUT = 1, ID_ABOUTMUI, ID_MUIPREFS,
    ID_ADD, ID_EDIT, ID_REMOVE, ID_SCAN, ID_POLL, ID_DETAILS, ID_LIST_DCLICK,
    ID_EDIT_OK, ID_EDIT_CANCEL,
    ID_SCAN_GO, ID_SCAN_CLOSE,
    ID_DETAIL_CLOSE, ID_INFO_REFRESH,
    ID_SHELL_RUN, ID_SHELL_BREAK, ID_SHELL_CLEAR,
    ID_FILES_GO, ID_FILES_PARENT, ID_FILES_DCLICK, ID_SELECT,
    ID_SCREEN, ID_SCR_CLOSE, ID_SCR_REFRESH,
    ID_VNC, ID_VNC_CONNECT, ID_VNC_DISCONNECT, ID_VNC_CLOSE,
    ID_FILES_DOWNLOAD, ID_FILES_UPLOAD, ID_MANUAL,
    ID_COPYWIN, ID_CP_CLOSE, ID_CP_TO_RIGHT, ID_CP_TO_LEFT, ID_CP_DELETE, ID_CP_RENAME,
    ID_CP_MAKEDIR, ID_CP_REFRESH, ID_CP_CANCEL, ID_ASK_OK, ID_ASK_CANCEL,
    /* per pane: base + pane index (0 left, 1 right) */
    ID_CP_MACH = 200, ID_CP_PATH = 210, ID_CP_PARENT = 220, ID_CP_DCLICK = 230, ID_CP_ACTIVE = 240
};

/* ------------------------------------------------------------------ *
 * Model
 * ------------------------------------------------------------------ */

enum { MS_UNKNOWN = 0, MS_UP, MS_DOWN, MS_LOCKED };

#define MAX_MACH 32

struct Machine {
    ULONG id;
    char  name[32];
    char  host[64];
    char  token[64];
    UWORD port;

    UBYTE state;
    UBYTE polling;          /* a JOB_POLL is in flight */
    UBYTE helloed;          /* HELLO sent since it last came up */
    UBYTE seen;             /* has ever answered: the columns hold real data */
    UBYTE busy;             /* long jobs in flight (EXEC, SHOT...): don't poll */
    UBYTE fails;            /* consecutive failed polls */
    ULONG ms;
    char  agent[24], cpu[8], os[24], chip[12], fast[12];
    char  err[80];
    char  info[1024];       /* last INFO reply, verbatim */
    char *drives;           /* "Name:\tkind\n" lines from Assign LIST (AllocVec) */
    UBYTE drives_pending;

    char  c_name[40], c_state[40], c_ms[16];   /* display-hook buffers */
};

static struct Machine g_mach[MAX_MACH];
static int   g_nmach = 0;
static ULONG g_next_id = 1;

static struct Worker w_poll, w_shell, w_misc, w_screen, w_vnc;
static int g_paltest = 0;       /* "amifleet68 PALTEST": all windows on 640x256 */
static struct MsgPort *g_reply = NULL;
static int   g_outstanding = 0;

static ULONG g_detail_mid = 0;   /* machine the details window shows */
static int   g_exec_busy = 0;
static ULONG g_exec_mid = 0;     /* machine the EXEC in flight runs on */
static ULONG g_stuck_mid = 0;    /* machine with a command left running */
static int   g_scan_busy = 0;
static struct Job *g_scan_job = NULL;
static int   g_edit_idx = -1;    /* -1 = adding */
static char  g_scan_net_default[16] = "";
static ULONG g_own_ip = 0;       /* this Amiga, from gethostid() */

/* Screen window: the agent's own screen grabs (HASH, then SHOT on change) */
static ULONG  g_scr_mid = 0;
static int    g_scr_pending = 0;      /* a HASH/SHOT is in flight */
static int    g_scr_force = 0;        /* check again at the next tick */
static ULONG  g_scr_hash = 0;
static ULONG *g_scr_fb = NULL;
static UWORD  g_scr_w, g_scr_h;
static UWORD  g_scr_downx, g_scr_downy;

/* VNC window */
static struct VncSession *g_vnc = NULL;
static ULONG  g_vnc_mid = 0;
static int    g_vnc_active = 0;       /* the JOB_VNC is out */
static BYTE   g_vnc_signal = -1;
static ULONG  g_vnc_keysym[128];      /* what each held rawkey was sent as */

/* Files pane */
struct FileEnt {
    char  name[108];
    char  size[12];
    char  prot[10];
    char  date[20];
    UBYTE is_dir;
};
static struct FileEnt *g_files = NULL;
static int   g_nfiles = 0;
static char  g_files_path[256] = "SYS:";
static char  g_files_pending[256] = "";
static struct Job *g_xfer = NULL;      /* the GET/PUT in flight, for progress */
static char  g_local_dir[256] = "RAM:";

/* ------------------------------------------------------------------ *
 * MUI objects
 * ------------------------------------------------------------------ */

static Object *app, *win, *lv_mach, *lst_mach, *txt_status;
static Object *win_edit, *str_name, *str_host, *str_port, *str_token, *bt_edit_ok, *bt_edit_cancel;
static Object *win_scan, *str_scan_net, *str_scan_token, *txt_scan, *bt_scan_go, *bt_scan_close;
static Object *win_det, *txt_det_head, *ft_info, *bt_info_refresh;
static Object *lv_shell, *lst_shell, *str_cmd, *bt_run, *bt_break, *bt_clear, *txt_shell;
static Object *str_path, *bt_parent, *lv_files, *lst_files, *txt_files, *bt_down, *bt_up;
static Object *bt_add, *bt_edit, *bt_remove, *bt_scan, *bt_poll, *bt_details;
static Object *bt_screen, *bt_vnc, *bt_copy;
static Object *win_copy, *bt_cp_right, *bt_cp_left, *bt_cp_del, *bt_cp_ren, *bt_cp_mkd,
              *bt_cp_ref, *bt_cp_cancel, *txt_cp;
static Object *win_ask, *txt_ask, *str_ask, *bt_ask_ok, *bt_ask_cancel;
static Object *win_scr, *txt_scr, *view_scr, *chk_live, *bt_scr_refresh;
static Object *win_vnc, *txt_vnc, *view_vnc, *str_vncpw, *chk_fast, *bt_vnc_go, *bt_vnc_stop;

static struct Hook mach_disp_hook, file_disp_hook, scr_view_hook, vnc_view_hook;

/* ------------------------------------------------------------------ *
 * Small helpers
 * ------------------------------------------------------------------ */

static void scpy(char *d, const char *s, size_t n)
{
    if (!n) return;
    strncpy(d, s ? s : "", n - 1);
    d[n - 1] = 0;
}

static struct Machine *mach_by_id(ULONG id)
{
    int i;
    for (i = 0; i < g_nmach; i++) if (g_mach[i].id == id) return &g_mach[i];
    return NULL;
}

static struct Machine *mach_selected(void)
{
    struct Machine *m = NULL;
    DoMethod(lst_mach, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, (ULONG)&m);
    return m;
}

static void set_status(const char *s) { set(txt_status, MUIA_Text_Contents, (ULONG)s); }

static void fmt_mem(char *d, size_t n, const char *bytes)
{
    unsigned long v = strtoul(bytes, NULL, 10);
    if (v >= 10UL * 1024 * 1024) snprintf(d, n, "%lu MB", v >> 20);
    else snprintf(d, n, "%lu KB", v >> 10);
}

/* exec.library major -> the OS release a person would name. */
static const char *os_name(int major)
{
    switch (major) {
    case 34: return "1.3";   case 36: return "2.0";   case 37: return "2.04";
    case 38: return "2.1";   case 39: return "3.0";   case 40: return "3.1";
    case 44: return "3.5";   case 45: return "3.9";   case 46: return "3.1.4";
    case 47: return "3.2";
    default: return NULL;
    }
}

/* Strip what a MUI text engine or a console would misread: ESC/CSI
 * sequences (programs colour their output) and other control bytes. */
static void sanitize(char *d, size_t n, const char *s, size_t len)
{
    size_t o = 0, i = 0;
    while (i < len && o + 1 < n) {
        UBYTE c = (UBYTE)s[i];
        if (c == 0x1b || c == 0x9b) {
            if (c == 0x1b) { i++; if (i < len && s[i] != '[') { i++; continue; } }
            i++;
            while (i < len && !(((UBYTE)s[i] >= 0x40 && (UBYTE)s[i] <= 0x7e))) i++;
            i++;
            continue;
        }
        if (c == '\t') { d[o++] = ' '; if (o + 1 < n) d[o++] = ' '; i++; continue; }
        if (c < 32 || (c >= 0x80 && c < 0xa0)) { i++; continue; }
        d[o++] = (char)c;
        i++;
    }
    d[o] = 0;
}

/* ------------------------------------------------------------------ *
 * Preferences: one machine per line, name TAB host TAB port TAB token.
 * ENV: for this session, ENVARC: to survive a reboot.
 * ------------------------------------------------------------------ */

static void mach_init(struct Machine *m, const char *name, const char *host,
                      int port, const char *token)
{
    /* g_mach is a static array: a fresh slot is zero, a reused one is not
     * - never leak the old drive list. */
    if (m->drives && m >= g_mach && m < g_mach + MAX_MACH && m->id) FreeVec(m->drives);
    memset(m, 0, sizeof *m);
    m->id = g_next_id++;
    scpy(m->name, name, sizeof m->name);
    scpy(m->host, host, sizeof m->host);
    scpy(m->token, token, sizeof m->token);
    m->port = (UWORD)(port > 0 && port < 65536 ? port : AGENT_PORT);
}

static void prefs_load(void)
{
    FILE *f = fopen(PREFS_ENV, "r");
    char line[256];
    if (!f) f = fopen(PREFS_ENVARC, "r");
    if (!f) return;
    while (g_nmach < MAX_MACH && fgets(line, sizeof line, f)) {
        char *fld[4] = { NULL, NULL, NULL, NULL };
        char *p = line;
        int i;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        for (i = 0; i < 4 && p; i++) {
            fld[i] = p;
            p = strchr(p, '\t');
            if (p) *p++ = 0;
        }
        if (!fld[0] || !fld[1] || !fld[1][0]) continue;
        mach_init(&g_mach[g_nmach++], fld[0], fld[1],
                  fld[2] ? atoi(fld[2]) : AGENT_PORT, fld[3] ? fld[3] : "");
    }
    fclose(f);
}

static void prefs_write(const char *path)
{
    FILE *f = fopen(path, "w");
    int i;
    if (!f) return;
    fprintf(f, "# amifleet68 machines: name<TAB>host<TAB>port<TAB>token\n");
    for (i = 0; i < g_nmach; i++)
        fprintf(f, "%s\t%s\t%u\t%s\n", g_mach[i].name, g_mach[i].host,
                (unsigned)g_mach[i].port, g_mach[i].token);
    fclose(f);
}

static void prefs_save(void)
{
    prefs_write(PREFS_ENV);
    prefs_write(PREFS_ENVARC);
}

/* ------------------------------------------------------------------ *
 * Jobs
 * ------------------------------------------------------------------ */

static struct Job *job_new(UBYTE type, struct Machine *m)
{
    struct Job *j = (struct Job *)AllocVec(sizeof *j, MEMF_ANY | MEMF_CLEAR);
    if (!j) return NULL;
    j->msg.mn_ReplyPort = g_reply;
    j->msg.mn_Length = sizeof *j;
    j->type = type;
    if (m) {
        j->mid = m->id;
        scpy(j->host, m->host, sizeof j->host);
        scpy(j->token, m->token, sizeof j->token);
        j->port = m->port;
    }
    return j;
}

static void job_send(struct Worker *wk, struct Job *j)
{
    PutMsg(wk->port, &j->msg);
    g_outstanding++;
}

static void job_free(struct Job *j)
{
    if (j->out) FreeVec(j->out);
    FreeVec(j);
}

static void poll_machine(struct Machine *m)
{
    struct Job *j;
    if (m->polling) return;
    if (g_exec_busy && m->id == g_exec_mid) return;   /* it cannot answer now */
    if (m->busy) return;       /* the agent is serving us a long request */
    if (!(j = job_new(JOB_POLL, m))) return;
    m->polling = 1;
    job_send(&w_poll, j);
}

static void poll_all(void)
{
    int i;
    for (i = 0; i < g_nmach; i++) poll_machine(&g_mach[i]);
}

/* ------------------------------------------------------------------ *
 * Machine list
 * ------------------------------------------------------------------ */

static ULONG mach_disp_func(struct Hook *h, char **a, struct Machine *m)
{
    (void)h;
    if (!m) {
        a[0] = (char *)"\033bMachine"; a[1] = (char *)"\033bState";
        a[2] = (char *)"\033bPing";    a[3] = (char *)"\033bAgent";
        a[4] = (char *)"\033bCPU";     a[5] = (char *)"\033bOS";
        a[6] = (char *)"\033bChip";    a[7] = (char *)"\033bFast";
        return 0;
    }
    switch (m->state) {
    case MS_UP:     strcpy(m->c_state, "\033bonline"); break;
    case MS_DOWN:   strcpy(m->c_state, "\033ioffline"); break;
    case MS_LOCKED: strcpy(m->c_state, "\033itoken?"); break;
    default:        strcpy(m->c_state, m->polling ? "..." : "-"); break;
    }
    if (g_exec_busy && m->id == g_exec_mid) strcpy(m->c_state, "\033bbusy");
    if (m->state == MS_UP) snprintf(m->c_ms, sizeof m->c_ms, "%lu ms", (unsigned long)m->ms);
    else strcpy(m->c_ms, "-");
    snprintf(m->c_name, sizeof m->c_name, "%s%s", m->state == MS_UP ? "" : "\033i", m->name);

    a[0] = m->c_name;
    a[1] = m->c_state;
    a[2] = m->c_ms;
    /* A machine that dropped off keeps its last good report. */
    a[3] = m->seen ? m->agent : (char *)"";
    a[4] = m->seen ? m->cpu : (char *)"";
    a[5] = m->seen ? m->os : (char *)"";
    a[6] = m->seen ? m->chip : (char *)"";
    a[7] = m->seen ? m->fast : (char *)"";
    return 0;
}

static void cp_board_changed(void);

static void list_rebuild(int active)
{
    int i;
    set(lst_mach, MUIA_List_Quiet, TRUE);
    DoMethod(lst_mach, MUIM_List_Clear);
    for (i = 0; i < g_nmach; i++)
        DoMethod(lst_mach, MUIM_List_InsertSingle, (ULONG)&g_mach[i], MUIV_List_Insert_Bottom);
    set(lst_mach, MUIA_List_Quiet, FALSE);
    if (g_nmach) set(lst_mach, MUIA_List_Active, active < g_nmach ? active : g_nmach - 1);
    cp_board_changed();
}

static void list_redraw(struct Machine *m)
{
    DoMethod(lst_mach, MUIM_List_Redraw, (ULONG)(m - g_mach));
}

static void update_status_line(void)
{
    char b[120];
    int i, up = 0;
    for (i = 0; i < g_nmach; i++) if (g_mach[i].state == MS_UP) up++;
    snprintf(b, sizeof b, "%d machine%s, %d online.  Polling every %d s.",
             g_nmach, g_nmach == 1 ? "" : "s", up, POLL_SECS);
    set_status(b);
}

static void update_buttons(void)
{
    int sel = mach_selected() != NULL;
    set(bt_edit, MUIA_Disabled, !sel);
    set(bt_remove, MUIA_Disabled, !sel);
    set(bt_details, MUIA_Disabled, !sel);
    set(bt_screen, MUIA_Disabled, !sel);
    set(bt_vnc, MUIA_Disabled, !sel);
    set(bt_copy, MUIA_Disabled, !g_nmach);
}

/* ------------------------------------------------------------------ *
 * Details window
 * ------------------------------------------------------------------ */

static void detail_show_info(void)
{
    static char text[1600];
    struct Machine *m = mach_by_id(g_detail_mid);
    char head[160];
    const char *p;
    size_t o;

    if (!m) return;
    snprintf(head, sizeof head, "\033b%s\033n   %s:%u   %s",
             m->name, m->host, (unsigned)m->port,
             m->state == MS_UP ? "online" : m->state == MS_DOWN ? "offline" :
             m->state == MS_LOCKED ? "token refused" : "not polled yet");
    set(txt_det_head, MUIA_Text_Contents, (ULONG)head);

    o = 0;
    if (m->state != MS_UP && m->err[0])
        o += snprintf(text + o, sizeof text - o, "Last error: %s\n\n", m->err);
    if (!m->info[0]) {
        snprintf(text + o, sizeof text - o, "No report yet.");
    } else {
        /* key=value -> "key: value", one per line. */
        for (p = m->info; *p && o + 4 < sizeof text; ) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            const char *eq = memchr(p, '=', len);
            char line[300];
            if (eq) {
                char val[240];
                size_t kl = (size_t)(eq - p);
                sanitize(val, sizeof val, eq + 1, len - kl - 1);
                snprintf(line, sizeof line, "%.*s: %s\n", (int)(kl < 40 ? kl : 40), p, val);
            } else {
                sanitize(line, sizeof line - 1, p, len);
                strcat(line, "\n");
            }
            o += snprintf(text + o, sizeof text - o, "%s", line);
            if (o >= sizeof text) o = sizeof text - 1;
            p = nl ? nl + 1 : p + len;
        }
    }
    set(ft_info, MUIA_Floattext_Text, (ULONG)text);
}

static void shell_add(const char *line)
{
    LONG n = 0;
    get(lst_shell, MUIA_List_Entries, &n);
    while (n-- >= SHELL_MAX) DoMethod(lst_shell, MUIM_List_Remove, MUIV_List_Remove_First);
    DoMethod(lst_shell, MUIM_List_InsertSingle, (ULONG)line, MUIV_List_Insert_Bottom);
}

static void shell_add_output(const char *s, ULONG len)
{
    ULONG i = 0;
    char clean[256];
    set(lst_shell, MUIA_List_Quiet, TRUE);
    while (i < len) {
        ULONG st = i;
        while (i < len && s[i] != '\n') i++;
        sanitize(clean, sizeof clean, s + st, i - st);
        if (i < len || clean[0]) shell_add(clean);
        i++;
    }
    set(lst_shell, MUIA_List_Quiet, FALSE);
    DoMethod(lst_shell, MUIM_List_Jump, MUIV_List_Jump_Bottom);
}

static void shell_state(void)
{
    set(bt_run, MUIA_Disabled, g_exec_busy);
    set(bt_break, MUIA_Disabled, g_exec_busy || !g_stuck_mid || g_stuck_mid != g_detail_mid);
}

static void files_request(const char *path);

static void detail_open(struct Machine *m)
{
    char title[80];
    if (!m) return;
    if (g_detail_mid != m->id) {
        g_detail_mid = m->id;
        DoMethod(lst_shell, MUIM_List_Clear);
        set(txt_shell, MUIA_Text_Contents, (ULONG)"Runs through the agent's EXEC (20 s, then Break).");
        DoMethod(lst_files, MUIM_List_Clear);
        g_nfiles = 0;
        set(txt_files, MUIA_Text_Contents, (ULONG)"");
        strcpy(g_files_path, "SYS:");
        set(str_path, MUIA_String_Contents, (ULONG)g_files_path);
    }
    snprintf(title, sizeof title, "%s - amifleet68", m->name);
    {   /* MUI keeps the pointer: give it storage that outlives this call */
        static char wtitle[80];
        strcpy(wtitle, title);
        set(win_det, MUIA_Window_Title, (ULONG)wtitle);
    }
    detail_show_info();
    shell_state();
    set(win_det, MUIA_Window_Open, TRUE);
    if (!g_nfiles) files_request(g_files_path);
}

/* ---- files ---- */

static ULONG file_disp_func(struct Hook *h, char **a, struct FileEnt *e)
{
    (void)h;
    if (!e) {
        a[0] = (char *)"\033bName"; a[1] = (char *)"\033bSize";
        a[2] = (char *)"\033bProtection"; a[3] = (char *)"\033bDate";
        return 0;
    }
    a[0] = e->name; a[1] = e->size; a[2] = e->prot; a[3] = e->date;
    return 0;
}

static int file_cmp(const void *x, const void *y)
{
    const struct FileEnt *a = (const struct FileEnt *)x, *b = (const struct FileEnt *)y;
    if (a->is_dir != b->is_dir) return a->is_dir ? -1 : 1;
    return strcasecmp(a->name, b->name);
}

static void files_request(const char *path)
{
    struct Machine *m = mach_by_id(g_detail_mid);
    struct Job *j;
    char b[300];
    if (!m || g_files_pending[0]) return;
    if (!(j = job_new(JOB_LIST, m))) return;
    scpy(j->arg, path, sizeof j->arg);
    scpy(g_files_pending, path, sizeof g_files_pending);
    snprintf(b, sizeof b, "Reading %s ...", path);
    set(txt_files, MUIA_Text_Contents, (ULONG)b);
    job_send(&w_misc, j);
}

/* LIST reply -> sorted FileEnt array (AllocVec'd, drawers first). */
static int parse_listing(struct Job *j, struct FileEnt **out)
{
    int n = 0, cap = 0;
    char *p;
    struct FileEnt *ents;

    *out = NULL;
    for (p = (char *)j->out; p && *p; p++) if (*p == '\n') cap++;
    cap++;
    ents = (struct FileEnt *)AllocVec(sizeof(struct FileEnt) * cap, MEMF_ANY | MEMF_CLEAR);
    if (!ents) return 0;

    for (p = (char *)j->out; p && *p && n < cap; ) {
        char *nl = strchr(p, '\n');
        char *f[5];
        int k;
        if (nl) *nl = 0;
        for (k = 0; k < 5; k++) {
            f[k] = p;
            if (k < 4) { p = strchr(p, '\t'); if (!p) break; *p++ = 0; }
        }
        if (k == 5) {
            struct FileEnt *e = &ents[n++];
            e->is_dir = f[0][0] == 'D';
            sanitize(e->name, sizeof e->name, f[4], strlen(f[4]));
            if (e->is_dir) strcpy(e->size, "(dir)");
            else scpy(e->size, f[1], sizeof e->size);
            scpy(e->prot, f[2], sizeof e->prot);
            scpy(e->date, f[3], sizeof e->date);
        }
        p = nl ? nl + 1 : NULL;
    }
    qsort(ents, (size_t)n, sizeof *ents, file_cmp);
    *out = ents;
    return n;
}

static void files_fill(struct Job *j)
{
    int n, i;
    char b[300];

    if (g_files) { FreeVec(g_files); g_files = NULL; }
    g_nfiles = 0;
    DoMethod(lst_files, MUIM_List_Clear);

    if (j->status != JS_OK) {
        snprintf(b, sizeof b, "%s: %s", j->arg, j->err[0] ? j->err : "failed");
        set(txt_files, MUIA_Text_Contents, (ULONG)b);
        return;
    }
    scpy(g_files_path, j->arg, sizeof g_files_path);
    set(str_path, MUIA_String_Contents, (ULONG)g_files_path);

    n = parse_listing(j, &g_files);
    g_nfiles = n;
    set(lst_files, MUIA_List_Quiet, TRUE);
    for (i = 0; i < n; i++)
        DoMethod(lst_files, MUIM_List_InsertSingle, (ULONG)&g_files[i], MUIV_List_Insert_Bottom);
    set(lst_files, MUIA_List_Quiet, FALSE);
    snprintf(b, sizeof b, "%d entr%s in %s  (%lu ms)", n, n == 1 ? "y" : "ies",
             g_files_path, (unsigned long)j->ms);
    set(txt_files, MUIA_Text_Contents, (ULONG)b);
}

static void files_join(char *d, size_t n, const char *dir, const char *name)
{
    size_t l = strlen(dir);
    if (!l || dir[l - 1] == ':' || dir[l - 1] == '/') snprintf(d, n, "%s%s", dir, name);
    else snprintf(d, n, "%s/%s", dir, name);
}

static void files_parent(void)
{
    char p[256];
    char *s;
    size_t l;
    scpy(p, g_files_path, sizeof p);
    l = strlen(p);
    if (!l || p[l - 1] == ':') return;           /* already at a volume root */
    if (p[l - 1] == '/') p[--l] = 0;
    s = strrchr(p, '/');
    if (s) *s = 0;
    else if ((s = strchr(p, ':'))) s[1] = 0;
    else return;
    files_request(p);
}


/* ---- transfers ---- */

static void xfer_start(UBYTE type, const char *remote, const char *local, ULONG total)
{
    struct Machine *m = mach_by_id(g_detail_mid);
    struct Job *j;
    if (!m || g_xfer) return;
    if (!(j = job_new(type, m))) return;
    scpy(j->arg, remote, sizeof j->arg);
    scpy(j->local, local, sizeof j->local);
    j->total = total;
    g_xfer = j;
    set(bt_down, MUIA_Disabled, TRUE);
    set(bt_up, MUIA_Disabled, TRUE);
    set(txt_files, MUIA_Text_Contents, (ULONG)(type == JOB_GET ? "Downloading..." : "Uploading..."));
    job_send(&w_misc, j);
}

/* A drawer + file from the ASL requester, as one path. */
static void asl_path(char *d, size_t n, const char *drawer, const char *file)
{
    scpy(d, drawer ? drawer : "", n);
    AddPart((STRPTR)d, (STRPTR)(file ? file : ""), (ULONG)n);
}

static void files_download(void)
{
    struct FileEnt *e = NULL;
    struct FileRequester *fr;
    char remote[300], local[300];

    if (g_xfer) return;
    DoMethod(lst_files, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, (ULONG)&e);
    if (!e || e->is_dir) { set(txt_files, MUIA_Text_Contents, (ULONG)"Select a file to download."); return; }
    if (!AslBase) { set(txt_files, MUIA_Text_Contents, (ULONG)"asl.library is missing."); return; }
    fr = (struct FileRequester *)MUI_AllocAslRequestTags(ASL_FileRequest,
            ASLFR_TitleText,     (ULONG)"Download to...",
            ASLFR_DoSaveMode,    TRUE,
            ASLFR_InitialDrawer, (ULONG)g_local_dir,
            ASLFR_InitialFile,   (ULONG)e->name,
            TAG_DONE);
    if (!fr) return;
    if (MUI_AslRequestTags(fr, TAG_DONE) && fr->fr_File && fr->fr_File[0]) {
        scpy(g_local_dir, (char *)fr->fr_Drawer, sizeof g_local_dir);
        asl_path(local, sizeof local, (char *)fr->fr_Drawer, (char *)fr->fr_File);
        files_join(remote, sizeof remote, g_files_path, e->name);
        xfer_start(JOB_GET, remote, local, strtoul(e->size, NULL, 10));
    }
    MUI_FreeAslRequest(fr);
}

static void files_upload(void)
{
    struct FileRequester *fr;
    char remote[300], local[300];

    if (g_xfer) return;
    if (!AslBase) { set(txt_files, MUIA_Text_Contents, (ULONG)"asl.library is missing."); return; }
    fr = (struct FileRequester *)MUI_AllocAslRequestTags(ASL_FileRequest,
            ASLFR_TitleText,     (ULONG)"Upload which file?",
            ASLFR_InitialDrawer, (ULONG)g_local_dir,
            TAG_DONE);
    if (!fr) return;
    if (MUI_AslRequestTags(fr, TAG_DONE) && fr->fr_File && fr->fr_File[0]) {
        scpy(g_local_dir, (char *)fr->fr_Drawer, sizeof g_local_dir);
        asl_path(local, sizeof local, (char *)fr->fr_Drawer, (char *)fr->fr_File);
        files_join(remote, sizeof remote, g_files_path, (char *)fr->fr_File);
        xfer_start(JOB_PUT, remote, local, 0);
    }
    MUI_FreeAslRequest(fr);
}

static void xfer_progress(void)
{
    char b[160];
    struct Job *j = g_xfer;
    ULONG done, total;
    if (!j) return;
    done = j->done; total = j->total;
    if (total)
        snprintf(b, sizeof b, "%s  %lu / %lu KB  (%lu%%)", j->type == JOB_GET ? "Downloading" : "Uploading",
                 (unsigned long)(done >> 10), (unsigned long)(total >> 10),
                 (unsigned long)(total > 0x1000000UL ? done / (total / 100) : done * 100 / total));
    else
        snprintf(b, sizeof b, "%s  %lu KB", j->type == JOB_GET ? "Downloading" : "Uploading",
                 (unsigned long)(done >> 10));
    set(txt_files, MUIA_Text_Contents, (ULONG)b);
}

static void xfer_reply(struct Job *j)
{
    char b[300], e[120];
    unsigned long kb = (unsigned long)((j->done + 1023) >> 10);
    unsigned long ms = j->ms ? (unsigned long)j->ms : 1;
    g_xfer = NULL;
    set(bt_down, MUIA_Disabled, FALSE);
    set(bt_up, MUIA_Disabled, FALSE);
    if (j->status == JS_OK) {
        snprintf(b, sizeof b, "%s %s  (%lu KB in %lu.%lu s, %lu KB/s)",
                 j->type == JOB_GET ? "Saved" : "Uploaded", j->type == JOB_GET ? j->local : j->arg,
                 kb, ms / 1000, ms % 1000 / 100, (unsigned long)(kb * 1000 / ms));
        set(txt_files, MUIA_Text_Contents, (ULONG)b);
        if (j->type == JOB_PUT && j->mid == g_detail_mid) files_request(g_files_path);
    } else {
        sanitize(e, sizeof e, j->err, strlen(j->err));
        snprintf(b, sizeof b, "\033b%s failed:\033n %s", j->type == JOB_GET ? "Download" : "Upload", e);
        set(txt_files, MUIA_Text_Contents, (ULONG)b);
    }
}



/* ------------------------------------------------------------------ *
 * Drive popups
 *
 * The MUI style guide: a string gadget whose useful values are a known set
 * gets a popup button beside it, not a free-typing test of memory. Every
 * path field here is a Popobject whose list holds that machine's volumes,
 * assigns and devices - from "Assign LIST" on the machine (cached per
 * machine), with the volumes from its INFO report until that arrives.
 * ------------------------------------------------------------------ */

struct Drive { char name[40]; char kind[10]; };
#define MAX_DRIVES 96

struct DrivePop {
    Object *pop, *str, *lv, *lst;
    ULONG  *mid;                /* which machine this field browses */
    LONG    accept_id;          /* ReturnID that lists the new path */
    struct Drive d[MAX_DRIVES];
    int     n;
    struct Hook strobj, objstr;
};

static struct DrivePop g_dpop[3];      /* 0,1: copier panes; 2: Files tab */
static struct Hook drive_disp_hook;

static ULONG drive_disp_func(struct Hook *h, char **a, struct Drive *d)
{
    (void)h;
    if (!d) { a[0] = (char *)"\033bDrive"; a[1] = (char *)"\033bKind"; return 0; }
    a[0] = d->name; a[1] = d->kind;
    return 0;
}

static const char *not_disks[] = { "PIPE", "AUX", "CON", "RAW", "PAR", "SER", "PRT", "NIL",
    "KCON", "KRAW", "TCP", "ENV", "URL", "CONSOLE", "RAM", NULL };

static void dp_add(struct DrivePop *dp, const char *name, size_t len, const char *kind)
{
    int i;
    if (!len || len > 36 || dp->n >= MAX_DRIVES) return;
    for (i = 0; i < dp->n; i++)
        if (!strncasecmp(dp->d[i].name, name, len) && dp->d[i].name[len] == ':') return;
    memcpy(dp->d[dp->n].name, name, len);
    dp->d[dp->n].name[len] = ':';
    dp->d[dp->n].name[len + 1] = 0;
    scpy(dp->d[dp->n].kind, kind, sizeof dp->d[dp->n].kind);
    dp->n++;
}

/* Fill from what we know about the machine: Assign LIST if we have it,
 * else the INFO report's volume list. */
static void dp_fill(struct DrivePop *dp)
{
    struct Machine *m = mach_by_id(*dp->mid);
    int i;
    dp->n = 0;
    if (m && m->drives) {
        const char *p = m->drives;
        while (*p) {
            const char *tab = strchr(p, '\t'), *nl = strchr(p, '\n');
            if (!nl) nl = p + strlen(p);
            if (tab && tab < nl) {
                char kind[10];
                size_t kl = (size_t)(nl - tab - 1);
                if (kl >= sizeof kind) kl = sizeof kind - 1;
                memcpy(kind, tab + 1, kl); kind[kl] = 0;
                dp_add(dp, p, (size_t)(tab - p - 1), kind);   /* stored with ':' */
            }
            p = *nl ? nl + 1 : nl;
        }
    } else if (m) {
        const char *v = strstr(m->info, "volumes=");
        if (v) {
            v += 8;
            while (*v && *v != '\n') {
                const char *e = v;
                while (*e && *e != ',' && *e != '\n') e++;
                dp_add(dp, v, (size_t)(e - v), "volume");
                v = *e == ',' ? e + 1 : e;
            }
        }
    }
    if (!dp->n) dp_add(dp, "RAM", 3, "volume");
    set(dp->lst, MUIA_List_Quiet, TRUE);
    DoMethod(dp->lst, MUIM_List_Clear);
    for (i = 0; i < dp->n; i++)
        DoMethod(dp->lst, MUIM_List_InsertSingle, (ULONG)&dp->d[i], MUIV_List_Insert_Bottom);
    set(dp->lst, MUIA_List_Quiet, FALSE);
}

static void drives_request(struct Machine *m)
{
    struct Job *j;
    if (!m || m->drives_pending || m->drives) return;
    if (!(j = job_new(JOB_FOP, m))) return;
    scpy(j->arg, "Assign LIST", sizeof j->arg);
    j->deadline = 15;
    j->tag = 100;                              /* route: drive list */
    m->drives_pending = 1;
    job_send(&w_misc, j);
}

/* "Assign LIST" -> "Name:\tkind\n" lines, volumes first. */
static void drives_reply(struct Job *j)
{
    struct Machine *m = mach_by_id(j->mid);
    char *out, *p, *line;
    int sect = 0, i;
    size_t o = 0, cap;
    if (!m) return;
    m->drives_pending = 0;
    if (j->status != JS_OK || j->rc != 0 || !j->out) return;
    cap = j->outlen * 2 + 64;
    if (!(out = (char *)AllocVec(cap, MEMF_ANY))) return;
    out[0] = 0;
    for (p = (char *)j->out; p && *p; ) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        line = p;
        p = nl ? nl + 1 : NULL;
        if (!strncmp(line, "Volumes:", 8)) { sect = 1; continue; }
        if (!strncmp(line, "Directories:", 12)) { sect = 2; continue; }
        if (!strncmp(line, "Devices:", 8)) { sect = 3; continue; }
        if (!line[0] || line[0] == ' ' || line[0] == '\t') continue;   /* multi-assign tail */
        if (sect == 1) {
            char *b = strstr(line, " [");
            size_t l = b ? (size_t)(b - line) : strlen(line);
            if (l && o + l + 12 < cap) o += (size_t)snprintf(out + o, cap - o, "%.*s:\tvolume\n", (int)l, line);
        } else if (sect == 2) {
            size_t l = strcspn(line, " \t");
            if (l && o + l + 12 < cap) o += (size_t)snprintf(out + o, cap - o, "%.*s:\tassign\n", (int)l, line);
        } else if (sect == 3) {
            char *t = line;
            while (*t) {
                size_t l;
                int skip = 0;
                while (*t == ' ') t++;
                l = strcspn(t, " ");
                if (!l) break;
                for (i = 0; not_disks[i]; i++)
                    if (strlen(not_disks[i]) == l && !strncasecmp(t, not_disks[i], l)) skip = 1;
                if (!skip && o + l + 12 < cap) o += (size_t)snprintf(out + o, cap - o, "%.*s:\tdevice\n", (int)l, t);
                t += l;
            }
        }
    }
    if (m->drives) FreeVec(m->drives);
    m->drives = out;
    /* A popup may be open on this machine right now: refill (harmless if
     * closed - MUI 3.8 has no "is it open" attribute to ask). */
    for (i = 0; i < 3; i++)
        if (g_dpop[i].pop && *g_dpop[i].mid == m->id) dp_fill(&g_dpop[i]);
}

static LONG strobj_func(struct Hook *h, Object *lst, Object *str)
{
    struct DrivePop *dp = (struct DrivePop *)h->h_Data;
    char *cur = NULL;
    int i;
    (void)lst;
    dp_fill(dp);
    drives_request(mach_by_id(*dp->mid));
    /* Pre-select the drive the field is on. */
    get(str, MUIA_String_Contents, &cur);
    set(dp->lst, MUIA_List_Active, MUIV_List_Active_Off);
    for (i = 0; cur && i < dp->n; i++)
        if (!strncasecmp(cur, dp->d[i].name, strlen(dp->d[i].name))) {
            set(dp->lst, MUIA_List_Active, i);
            break;
        }
    return TRUE;
}

static void objstr_func(struct Hook *h, Object *lst, Object *str)
{
    struct DrivePop *dp = (struct DrivePop *)h->h_Data;
    struct Drive *d = NULL;
    (void)lst;
    DoMethod(dp->lst, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, (ULONG)&d);
    if (!d) return;
    set(str, MUIA_String_Contents, (ULONG)d->name);
    DoMethod(app, MUIM_Application_ReturnID, dp->accept_id);
}

/* A path field: string + popup button + the drive list. */
static Object *drive_field(int i, ULONG *mid, LONG accept_id, const char *init)
{
    struct DrivePop *dp = &g_dpop[i];
    dp->mid = mid;
    dp->accept_id = accept_id;
    dp->strobj.h_Entry = (APTR)HookEntry; dp->strobj.h_SubEntry = (APTR)strobj_func; dp->strobj.h_Data = dp;
    dp->objstr.h_Entry = (APTR)HookEntry; dp->objstr.h_SubEntry = (APTR)objstr_func; dp->objstr.h_Data = dp;
    drive_disp_hook.h_Entry = (APTR)HookEntry;
    drive_disp_hook.h_SubEntry = (APTR)drive_disp_func;
    dp->pop = PopobjectObject,
        MUIA_Popstring_String, (ULONG)(dp->str = StringObject, StringFrame,
            MUIA_String_MaxLen, 255, MUIA_String_Contents, (ULONG)init, MUIA_CycleChain, 1, End),
        MUIA_Popstring_Button, (ULONG)PopButton(MUII_PopUp),
        MUIA_Popobject_StrObjHook, (ULONG)&dp->strobj,
        MUIA_Popobject_ObjStrHook, (ULONG)&dp->objstr,
        MUIA_Popobject_Object, (ULONG)(dp->lv = ListviewObject,
            MUIA_FixHeightTxt, (ULONG)"\n\n\n\n\n\n\n\n\n\n\n\n\n\n",   /* 14 lines */
            MUIA_Listview_List, (ULONG)(dp->lst = ListObject, InputListFrame,
                MUIA_List_Format, (ULONG)"BAR,",
                MUIA_List_Title, TRUE,
                MUIA_List_DisplayHook, (ULONG)&drive_disp_hook,
                MUIA_List_AdjustWidth, TRUE,
            End),
        End),
        MUIA_CycleChain, 1,
    End;
    return dp->pop;
}

static void drive_notify(void)
{
    int i;
    for (i = 0; i < 3; i++) {
        struct DrivePop *dp = &g_dpop[i];
        if (!dp->pop) continue;
        DoMethod(dp->lv, MUIM_Notify, MUIA_Listview_DoubleClick, TRUE,
                 (ULONG)dp->pop, 2, MUIM_Popstring_Close, TRUE);
        set(dp->pop, MUIA_ShortHelp, (ULONG)"Type a path, or pick a drive, volume or\nassign of that machine from the list.");
    }
}

/* ------------------------------------------------------------------ *
 * File copy: two panes, each on any machine of the board
 *
 * The machine you sit at is on the board too (its own agent at 127.0.0.1),
 * so "local" needs no special case: every pane is an agent. Copy runs on
 * its own worker; same-machine copies become one AmigaDOS Copy there,
 * cross-machine ones stream GETRANGE -> PUT (worker.c job_copy).
 * ------------------------------------------------------------------ */

struct Pane {
    ULONG  mid;
    char   path[256];
    struct FileEnt *ents;
    int    n;
    int    pending;             /* a LIST is out */
    Object *grp, *cyc, *str, *bt_parent, *lv, *lst, *txt;
    const char *labels[MAX_MACH + 1];
    ULONG  lab_mid[MAX_MACH];
};
static struct Pane g_pane[2];
static int    g_cp_active = 0;          /* pane Delete/Rename/MakeDir act on */
static struct Job *g_copy_job = NULL;
static struct CopySpec *g_copy_spec = NULL;
static ULONG  g_copy_t0;
static int    g_fop_pending[2];         /* file ops out, per pane */
static char   g_fop_err[2][120];
enum { ASK_NONE, ASK_RENAME, ASK_MAKEDIR };
static int    g_ask_mode = ASK_NONE, g_ask_pane = 0;
static char   g_ask_old[108];

static struct Worker w_copy;

static ULONG ms_now(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    return (ULONG)ds.ds_Minute * 60000UL + (ULONG)ds.ds_Tick * 20UL;
}

/* Quote for AmigaDOS: "*" escapes " and *. */
static void dosq(char *d, size_t n, const char *s)
{
    size_t o = 0;
    if (n < 3) return;
    d[o++] = '"';
    while (*s && o + 3 < n) {
        if (*s == '"' || *s == '*') d[o++] = '*';
        d[o++] = *s++;
    }
    d[o++] = '"';
    d[o] = 0;
}

static void cp_request(int pi, const char *path)
{
    struct Pane *p = &g_pane[pi];
    struct Machine *m = mach_by_id(p->mid);
    struct Job *j;
    char b[300];
    if (!m || p->pending) return;
    if (!(j = job_new(JOB_LIST, m))) return;
    j->tag = (UBYTE)(pi + 1);
    scpy(j->arg, path, sizeof j->arg);
    p->pending = 1;
    snprintf(b, sizeof b, "Reading %s ...", path);
    set(p->txt, MUIA_Text_Contents, (ULONG)b);
    job_send(&w_misc, j);
}

static void cp_fill(int pi, struct Job *j)
{
    struct Pane *p = &g_pane[pi];
    char b[300];
    int i;
    p->pending = 0;
    if (j->mid != p->mid) return;                 /* switched machine meanwhile */
    if (j->status != JS_OK) {
        char e[120];
        sanitize(e, sizeof e, j->err, strlen(j->err));
        snprintf(b, sizeof b, "\033b%s\033n: %s", j->arg, e);
        set(p->txt, MUIA_Text_Contents, (ULONG)b);
        return;
    }
    DoMethod(p->lst, MUIM_List_Clear);
    if (p->ents) { FreeVec(p->ents); p->ents = NULL; }
    p->n = parse_listing(j, &p->ents);
    scpy(p->path, j->arg, sizeof p->path);
    set(p->str, MUIA_String_Contents, (ULONG)p->path);
    set(p->lst, MUIA_List_Quiet, TRUE);
    for (i = 0; i < p->n; i++)
        DoMethod(p->lst, MUIM_List_InsertSingle, (ULONG)&p->ents[i], MUIV_List_Insert_Bottom);
    set(p->lst, MUIA_List_Quiet, FALSE);
    snprintf(b, sizeof b, "%d entr%s", p->n, p->n == 1 ? "y" : "ies");
    set(p->txt, MUIA_Text_Contents, (ULONG)b);
}

/* The machine chooser is a Cycle; its entries are fixed at creation, so
 * it is rebuilt whenever the board changes. */
static void cp_rebuild_cycle(int pi)
{
    struct Pane *p = &g_pane[pi];
    int i, act = 0;
    Object *c;
    for (i = 0; i < g_nmach; i++) {
        p->labels[i] = g_mach[i].name;
        p->lab_mid[i] = g_mach[i].id;
        if (g_mach[i].id == p->mid) act = i;
    }
    p->labels[g_nmach] = NULL;
    if (!g_nmach) return;
    c = CycleObject, MUIA_Cycle_Entries, (ULONG)p->labels, MUIA_Cycle_Active, act,
        MUIA_CycleChain, 1, End;
    if (!c) return;
    DoMethod(p->grp, MUIM_Group_InitChange);
    if (p->cyc) { DoMethod(p->grp, OM_REMMEMBER, (ULONG)p->cyc); MUI_DisposeObject(p->cyc); }
    DoMethod(p->grp, OM_ADDMEMBER, (ULONG)c);
    DoMethod(p->grp, MUIM_Group_ExitChange);
    p->cyc = c;
    DoMethod(c, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime,
             (ULONG)app, 2, MUIM_Application_ReturnID, ID_CP_MACH + pi);
    p->mid = g_mach[act].id;
}

static void cp_open(void)
{
    struct Machine *sel = mach_selected();
    int pi;
    /* Left: this Amiga (a loopback machine) if there is one; right: the
     * selected machine. */
    if (!g_pane[0].mid || !mach_by_id(g_pane[0].mid)) {
        int i;
        g_pane[0].mid = g_nmach ? g_mach[0].id : 0;
        for (i = 0; i < g_nmach; i++)
            if (!strcmp(g_mach[i].host, "127.0.0.1")) { g_pane[0].mid = g_mach[i].id; break; }
        strcpy(g_pane[0].path, "RAM:");
    }
    if (!g_pane[1].mid || !mach_by_id(g_pane[1].mid)) {
        g_pane[1].mid = sel ? sel->id : g_pane[0].mid;
        strcpy(g_pane[1].path, "RAM:");
    }
    for (pi = 0; pi < 2; pi++) cp_rebuild_cycle(pi);
    set(win_copy, MUIA_Window_Open, TRUE);
    for (pi = 0; pi < 2; pi++) if (!g_pane[pi].n) cp_request(pi, g_pane[pi].path);
}

static void cp_machine_changed(int pi)
{
    struct Pane *p = &g_pane[pi];
    LONG a = 0;
    get(p->cyc, MUIA_Cycle_Active, &a);
    if (a < 0 || a >= g_nmach) return;
    if (p->lab_mid[a] == p->mid) return;
    p->mid = p->lab_mid[a];
    p->pending = 0;
    DoMethod(p->lst, MUIM_List_Clear);
    if (p->ents) { FreeVec(p->ents); p->ents = NULL; }
    p->n = 0;
    strcpy(p->path, "RAM:");
    cp_request(pi, p->path);
}

static void cp_parent(int pi)
{
    char path[256], *q;
    size_t l;
    scpy(path, g_pane[pi].path, sizeof path);
    l = strlen(path);
    if (!l || path[l - 1] == ':') return;
    if (path[l - 1] == '/') path[--l] = 0;
    if ((q = strrchr(path, '/'))) *q = 0;
    else if ((q = strchr(path, ':'))) q[1] = 0;
    cp_request(pi, path);
}

static void cp_dclick(int pi)
{
    struct FileEnt *e = NULL;
    char path[300];
    DoMethod(g_pane[pi].lst, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, (ULONG)&e);
    if (!e || !e->is_dir) return;
    files_join(path, sizeof path, g_pane[pi].path, e->name);
    cp_request(pi, path);
}

/* Selected entries (or the active one when nothing is marked). */
static int cp_selection(int pi, struct FileEnt **out, int max)
{
    LONG pos = MUIV_List_NextSelected_Start;
    int n = 0;
    for (;;) {
        struct FileEnt *e = NULL;
        DoMethod(g_pane[pi].lst, MUIM_List_NextSelected, (ULONG)&pos);
        if (pos == MUIV_List_NextSelected_End) break;
        DoMethod(g_pane[pi].lst, MUIM_List_GetEntry, pos, (ULONG)&e);
        if (e && n < max) out[n++] = e;
    }
    return n;
}

static void cp_buttons(void)
{
    int busy = g_copy_job != NULL;
    set(bt_cp_right, MUIA_Disabled, busy);
    set(bt_cp_left, MUIA_Disabled, busy);
    set(bt_cp_cancel, MUIA_Disabled, !busy);
}

static void cp_copy(int from)
{
    struct Pane *s = &g_pane[from], *d = &g_pane[!from];
    struct Machine *ms = mach_by_id(s->mid), *md = mach_by_id(d->mid);
    struct FileEnt *sel[COPY_MAX];
    struct CopySpec *cs;
    struct Job *j;
    char msg[400];
    int n, i;
    ULONG total = 0;

    if (g_copy_job || !ms || !md) return;
    n = cp_selection(from, sel, COPY_MAX);
    if (!n) { set(txt_cp, MUIA_Text_Contents, (ULONG)"Select something to copy first."); return; }
    if (s->mid == d->mid && !strcasecmp(s->path, d->path)) {
        set(txt_cp, MUIA_Text_Contents, (ULONG)"Source and destination are the same drawer.");
        return;
    }
    for (i = 0; i < n; i++) if (!sel[i]->is_dir) total += strtoul(sel[i]->size, NULL, 10);
    if (n == 1)
        snprintf(msg, sizeof msg, "Copy \033b%s\033n\nfrom %s:%s\nto   %s:%s ?\n\nFiles of the same name there are replaced.",
                 sel[0]->name, ms->name, s->path, md->name, d->path);
    else
        snprintf(msg, sizeof msg, "Copy \033b%d items\033n\nfrom %s:%s\nto   %s:%s ?\n\nFiles of the same name there are replaced.",
                 n, ms->name, s->path, md->name, d->path);
    if (MUI_Request(app, win_copy, 0, (char *)"Copy", (char *)"_Copy|_Cancel", msg) != 1) return;

    if (!(cs = (struct CopySpec *)AllocVec(sizeof *cs, MEMF_ANY | MEMF_CLEAR))) return;
    scpy(cs->shost, ms->host, sizeof cs->shost); scpy(cs->stoken, ms->token, sizeof cs->stoken); cs->sport = ms->port;
    scpy(cs->dhost, md->host, sizeof cs->dhost); scpy(cs->dtoken, md->token, sizeof cs->dtoken); cs->dport = md->port;
    scpy(cs->srcdir, s->path, sizeof cs->srcdir);
    scpy(cs->dstdir, d->path, sizeof cs->dstdir);
    for (i = 0; i < n; i++) {
        scpy(cs->name[i], sel[i]->name, sizeof cs->name[i]);
        cs->isdir[i] = sel[i]->is_dir;
        cs->size[i] = sel[i]->is_dir ? 0 : strtoul(sel[i]->size, NULL, 10);
    }
    cs->n = (UWORD)n;
    if (!(j = job_new(JOB_COPY, ms))) { FreeVec(cs); return; }
    j->cs = cs;
    j->tag = (UBYTE)(!from + 1);                 /* refresh the destination */
    g_copy_job = j;
    g_copy_spec = cs;
    g_copy_t0 = ms_now();
    ms->busy++;                                  /* both agents are ours for a while */
    if (md != ms) md->busy++;
    j->hash = md->id;                            /* remember the other end */
    cp_buttons();
    set(txt_cp, MUIA_Text_Contents, (ULONG)"Copying...");
    job_send(&w_copy, j);
    (void)total;
}

static void cp_progress(void)
{
    struct CopySpec *cs = g_copy_spec;
    char b[200], cur[108];
    if (!cs || !g_copy_job) return;
    sanitize(cur, sizeof cur, cs->current, strlen(cs->current));
    snprintf(b, sizeof b, "Copying %s  -  %lu file%s, %lu KB%s",
             cur[0] ? cur : "...", (unsigned long)cs->files, cs->files == 1 ? "" : "s",
             (unsigned long)(cs->bytes >> 10), cs->cancel ? "  (cancelling)" : "");
    set(txt_cp, MUIA_Text_Contents, (ULONG)b);
}

static void cp_copy_reply(struct Job *j)
{
    struct CopySpec *cs = j->cs;
    struct Machine *a = mach_by_id(j->mid), *b2 = mach_by_id(j->hash);
    char b[300];
    ULONG ms = ms_now() - g_copy_t0;
    if (!ms) ms = 1;
    if (a && a->busy) a->busy--;
    if (b2 && b2 != a && b2->busy) b2->busy--;
    if (j->status == JS_OK)
        snprintf(b, sizeof b, "Copied %lu file%s, %lu KB in %lu.%lu s%s.",
                 (unsigned long)cs->files, cs->files == 1 ? "" : "s", (unsigned long)(cs->bytes >> 10),
                 (unsigned long)(ms / 1000), (unsigned long)(ms % 1000 / 100),
                 cs->same ? " (on the machine itself)" : "");
    else {
        char e[160];
        sanitize(e, sizeof e, cs->err[0] ? cs->err : j->err, strlen(cs->err[0] ? cs->err : j->err));
        snprintf(b, sizeof b, "\033bStopped:\033n %s  (%lu files done)", e, (unsigned long)cs->files);
    }
    set(txt_cp, MUIA_Text_Contents, (ULONG)b);
    g_copy_job = NULL;
    g_copy_spec = NULL;
    FreeVec(cs);
    j->cs = NULL;
    cp_buttons();
    cp_request(j->tag - 1, g_pane[j->tag - 1].path);
}

static void cp_cancel(void)
{
    if (!g_copy_spec || !g_copy_job) return;
    g_copy_spec->cancel = 1;
    if (w_copy.proc) Signal((struct Task *)w_copy.proc, SIGBREAKF_CTRL_C);
    cp_progress();
}

static void cp_fop(int pi, const char *cmd, UWORD deadline)
{
    struct Machine *m = mach_by_id(g_pane[pi].mid);
    struct Job *j;
    if (!m || !(j = job_new(JOB_FOP, m))) return;
    scpy(j->arg, cmd, sizeof j->arg);
    j->deadline = deadline;
    j->tag = (UBYTE)(pi + 1);
    g_fop_pending[pi]++;
    m->busy++;
    job_send(&w_copy, j);
}

static void cp_fop_reply(struct Job *j)
{
    int pi = j->tag - 1;
    struct Machine *m = mach_by_id(j->mid);
    if (m && m->busy) m->busy--;
    if ((j->status != JS_OK || j->rc != 0) && !g_fop_err[pi][0]) {
        char e[120];
        if (j->status == JS_OK) sanitize(e, sizeof e, j->out ? (char *)j->out : "", j->outlen);
        else sanitize(e, sizeof e, j->err, strlen(j->err));
        snprintf(g_fop_err[pi], sizeof g_fop_err[pi], "%s", e[0] ? e : "the command failed");
    }
    if (--g_fop_pending[pi] <= 0) {
        g_fop_pending[pi] = 0;
        if (g_fop_err[pi][0]) {
            char b[160];
            snprintf(b, sizeof b, "\033bFailed:\033n %s", g_fop_err[pi]);
            set(txt_cp, MUIA_Text_Contents, (ULONG)b);
        } else set(txt_cp, MUIA_Text_Contents, (ULONG)"Done.");
        g_fop_err[pi][0] = 0;
        cp_request(pi, g_pane[pi].path);
    }
}

static void cp_delete(void)
{
    int pi = g_cp_active, n, i;
    struct FileEnt *sel[COPY_MAX];
    struct Machine *m = mach_by_id(g_pane[pi].mid);
    char msg[300];
    if (!m) return;
    n = cp_selection(pi, sel, COPY_MAX);
    if (!n) { set(txt_cp, MUIA_Text_Contents, (ULONG)"Select something to delete first."); return; }
    if (n == 1) snprintf(msg, sizeof msg, "Delete \033b%s\033n%s on %s:%s ?\n\nThis cannot be undone.",
                         sel[0]->name, sel[0]->is_dir ? " and everything in it" : "", m->name, g_pane[pi].path);
    else snprintf(msg, sizeof msg, "Delete \033b%d items\033n on %s:%s ?\nDrawers are deleted with everything in them.\n\nThis cannot be undone.",
                  n, m->name, g_pane[pi].path);
    if (MUI_Request(app, win_copy, 0, (char *)"Delete", (char *)"_Delete|_Cancel", msg) != 1) return;
    g_fop_err[pi][0] = 0;
    for (i = 0; i < n; i++) {
        char path[300], q[320], cmd[400];
        files_join(path, sizeof path, g_pane[pi].path, sel[i]->name);
        dosq(q, sizeof q, path);
        snprintf(cmd, sizeof cmd, "Delete %s%s QUIET FORCE", q, sel[i]->is_dir ? " ALL" : "");
        cp_fop(pi, cmd, 600);
    }
    set(txt_cp, MUIA_Text_Contents, (ULONG)"Deleting...");
}

static void ask_open(int mode, const char *title, const char *label, const char *init)
{
    g_ask_mode = mode;
    g_ask_pane = g_cp_active;
    set(win_ask, MUIA_Window_Title, (ULONG)title);
    set(txt_ask, MUIA_Text_Contents, (ULONG)label);
    set(str_ask, MUIA_String_Contents, (ULONG)init);
    set(win_ask, MUIA_Window_Open, TRUE);
    set(win_ask, MUIA_Window_ActiveObject, (ULONG)str_ask);
}

static void cp_rename(void)
{
    static char label[160];
    struct FileEnt *e = NULL;
    DoMethod(g_pane[g_cp_active].lst, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, (ULONG)&e);
    if (!e) { set(txt_cp, MUIA_Text_Contents, (ULONG)"Click the entry to rename first."); return; }
    scpy(g_ask_old, e->name, sizeof g_ask_old);
    snprintf(label, sizeof label, "New name for \033b%s\033n:", e->name);
    ask_open(ASK_RENAME, "Rename", label, e->name);
}

static void cp_makedir(void)
{
    ask_open(ASK_MAKEDIR, "New drawer", "Name of the new drawer:", "");
}

static void ask_commit(void)
{
    char *v = NULL, path[300], path2[300], q1[320], q2[320], cmd[700];
    int pi = g_ask_pane;
    get(str_ask, MUIA_String_Contents, &v);
    set(win_ask, MUIA_Window_Open, FALSE);
    if (!v || !v[0] || strchr(v, '/') || strchr(v, ':')) {
        if (v && v[0]) set(txt_cp, MUIA_Text_Contents, (ULONG)"A name, please - no / or : in it.");
        return;
    }
    if (g_ask_mode == ASK_RENAME) {
        if (!strcmp(v, g_ask_old)) return;
        files_join(path, sizeof path, g_pane[pi].path, g_ask_old);
        files_join(path2, sizeof path2, g_pane[pi].path, v);
        dosq(q1, sizeof q1, path); dosq(q2, sizeof q2, path2);
        snprintf(cmd, sizeof cmd, "Rename %s %s", q1, q2);
        cp_fop(pi, cmd, 30);
    } else if (g_ask_mode == ASK_MAKEDIR) {
        files_join(path, sizeof path, g_pane[pi].path, v);
        dosq(q1, sizeof q1, path);
        snprintf(cmd, sizeof cmd, "MakeDir %s", q1);
        cp_fop(pi, cmd, 30);
    }
    g_ask_mode = ASK_NONE;
}

static Object *cp_pane(int pi)
{
    struct Pane *p = &g_pane[pi];
    Object *o = VGroup, GroupFrameT(pi ? "Right" : "Left"),
        Child, HGroup,
            Child, Label2("Machine:"),
            Child, p->grp = HGroup, MUIA_Group_Spacing, 0, End,
        End,
        Child, HGroup,
            Child, drive_field(pi, &p->mid, ID_CP_PATH + pi, "RAM:"),
            Child, p->bt_parent = SimpleButton(pi ? "Pa_rent" : "_Parent"),
        End,
        Child, p->lv = ListviewObject,
            MUIA_CycleChain, 1,
            MUIA_Listview_MultiSelect, MUIV_Listview_MultiSelect_Default,
            MUIA_Listview_List, p->lst = ListObject, InputListFrame,
                MUIA_List_Format, (ULONG)"BAR WEIGHT=100,P=\033r WEIGHT=25",
                MUIA_List_Title, TRUE,
                MUIA_List_DisplayHook, (ULONG)&file_disp_hook,
            End,
        End,
        Child, p->txt = TextObject, TextFrame, MUIA_Background, MUII_TextBack,
            MUIA_Text_Contents, (ULONG)"", End,
    End;
    p->str = g_dpop[pi].str;
    return o;
}

static void cp_notify(void)
{
    int pi;
#define CRET(obj, attr, val, id) \
    DoMethod((obj), MUIM_Notify, (attr), (val), (ULONG)app, 2, MUIM_Application_ReturnID, (id))
    for (pi = 0; pi < 2; pi++) {
        struct Pane *p = &g_pane[pi];
        CRET(p->str, MUIA_String_Acknowledge, MUIV_EveryTime, ID_CP_PATH + pi);
        CRET(p->bt_parent, MUIA_Pressed, FALSE, ID_CP_PARENT + pi);
        CRET(p->lv, MUIA_Listview_DoubleClick, TRUE, ID_CP_DCLICK + pi);
        CRET(p->lst, MUIA_List_Active, MUIV_EveryTime, ID_CP_ACTIVE + pi);
    }
    CRET(win_copy, MUIA_Window_CloseRequest, TRUE, ID_CP_CLOSE);
    CRET(bt_cp_right, MUIA_Pressed, FALSE, ID_CP_TO_RIGHT);
    CRET(bt_cp_left, MUIA_Pressed, FALSE, ID_CP_TO_LEFT);
    CRET(bt_cp_del, MUIA_Pressed, FALSE, ID_CP_DELETE);
    CRET(bt_cp_ren, MUIA_Pressed, FALSE, ID_CP_RENAME);
    CRET(bt_cp_mkd, MUIA_Pressed, FALSE, ID_CP_MAKEDIR);
    CRET(bt_cp_ref, MUIA_Pressed, FALSE, ID_CP_REFRESH);
    CRET(bt_cp_cancel, MUIA_Pressed, FALSE, ID_CP_CANCEL);
    CRET(bt_ask_ok, MUIA_Pressed, FALSE, ID_ASK_OK);
    CRET(str_ask, MUIA_String_Acknowledge, MUIV_EveryTime, ID_ASK_OK);
    CRET(bt_ask_cancel, MUIA_Pressed, FALSE, ID_ASK_CANCEL);
    CRET(win_ask, MUIA_Window_CloseRequest, TRUE, ID_ASK_CANCEL);
#undef CRET
}

/* Returns 1 when the id was a copier one. */
static void cp_board_changed(void)
{
    ULONG open = 0;
    int pi;
    if (!win_copy) return;
    get(win_copy, MUIA_Window_Open, &open);
    if (!open) return;
    for (pi = 0; pi < 2; pi++) {
        if (!mach_by_id(g_pane[pi].mid)) g_pane[pi].mid = g_nmach ? g_mach[0].id : 0;
        cp_rebuild_cycle(pi);
    }
}

static int cp_handle(LONG id)
{
    if (id >= ID_CP_MACH && id < ID_CP_MACH + 2) { cp_machine_changed((int)(id - ID_CP_MACH)); return 1; }
    if (id >= ID_CP_PATH && id < ID_CP_PATH + 2) {
        char *v = NULL;
        get(g_pane[id - ID_CP_PATH].str, MUIA_String_Contents, &v);
        if (v && v[0]) cp_request((int)(id - ID_CP_PATH), v);
        return 1;
    }
    if (id >= ID_CP_PARENT && id < ID_CP_PARENT + 2) { cp_parent((int)(id - ID_CP_PARENT)); return 1; }
    if (id >= ID_CP_DCLICK && id < ID_CP_DCLICK + 2) { g_cp_active = (int)(id - ID_CP_DCLICK); cp_dclick(g_cp_active); return 1; }
    if (id >= ID_CP_ACTIVE && id < ID_CP_ACTIVE + 2) { g_cp_active = (int)(id - ID_CP_ACTIVE); return 1; }
    switch (id) {
    case ID_COPYWIN: cp_open(); return 1;
    case ID_CP_CLOSE: set(win_copy, MUIA_Window_Open, FALSE); return 1;
    case ID_CP_TO_RIGHT: cp_copy(0); return 1;
    case ID_CP_TO_LEFT: cp_copy(1); return 1;
    case ID_CP_DELETE: cp_delete(); return 1;
    case ID_CP_RENAME: cp_rename(); return 1;
    case ID_CP_MAKEDIR: cp_makedir(); return 1;
    case ID_CP_REFRESH: cp_request(0, g_pane[0].path); cp_request(1, g_pane[1].path); return 1;
    case ID_CP_CANCEL: cp_cancel(); return 1;
    case ID_ASK_OK: ask_commit(); return 1;
    case ID_ASK_CANCEL: g_ask_mode = ASK_NONE; set(win_ask, MUIA_Window_Open, FALSE); return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * Replies from the workers
 * ------------------------------------------------------------------ */

static void apply_info(struct Machine *m, const char *info)
{
    const char *p = info;
    char ks[16] = "";
    scpy(m->info, info, sizeof m->info);
    m->agent[0] = m->cpu[0] = m->chip[0] = m->fast[0] = 0;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char key[24], val[64];
        const char *eq = memchr(p, '=', len);
        if (eq && (size_t)(eq - p) < sizeof key) {
            size_t kl = (size_t)(eq - p), vl = len - kl - 1;
            memcpy(key, p, kl); key[kl] = 0;
            if (vl >= sizeof val) vl = sizeof val - 1;
            memcpy(val, eq + 1, vl); val[vl] = 0;
            if (!strcmp(key, "agent"))
                scpy(m->agent, strncmp(val, "amiagent ", 9) ? val : val + 9, sizeof m->agent);
            else if (!strcmp(key, "cpu")) scpy(m->cpu, val, sizeof m->cpu);
            else if (!strcmp(key, "kickstart")) scpy(ks, val, sizeof ks);
            else if (!strcmp(key, "chipram_free")) fmt_mem(m->chip, sizeof m->chip, val);
            else if (!strcmp(key, "fastram_free")) fmt_mem(m->fast, sizeof m->fast, val);
        }
        p = nl ? nl + 1 : NULL;
    }
    {
        const char *os = os_name(atoi(ks));
        if (os) snprintf(m->os, sizeof m->os, "%s", os);
        else scpy(m->os, ks, sizeof m->os);
    }
}

static void add_found(ULONG net, int oct, const char *token, int *added)
{
    char host[20], name[32];
    int i;
    snprintf(host, sizeof host, "%u.%u.%u.%d", (unsigned)((net >> 24) & 255),
             (unsigned)((net >> 16) & 255), (unsigned)((net >> 8) & 255), oct);
    for (i = 0; i < g_nmach; i++) {
        if (!strcmp(g_mach[i].host, host)) return;
        /* This very Amiga, already on the board as loopback. */
        if (((net & 0xFFFFFF00UL) | (ULONG)oct) == g_own_ip &&
            (!strcmp(g_mach[i].host, "127.0.0.1") || !strcasecmp(g_mach[i].host, "localhost")))
            return;
    }
    if (g_nmach >= MAX_MACH) return;
    snprintf(name, sizeof name, "Amiga .%d", oct);
    mach_init(&g_mach[g_nmach], name, host, AGENT_PORT, token);
    g_nmach++;
    (*added)++;
}

static void scr_reply(struct Job *j);
static void mach_busy(ULONG mid, int delta);
static void vnc_buttons(void);
static void vnc_signalled(void);

static void handle_reply(struct Job *j)
{
    struct Machine *m = j->mid ? mach_by_id(j->mid) : NULL;
    char b[200];

    g_outstanding--;
    if (j->status == JS_CANCEL) { job_free(j); return; }

    switch (j->type) {
    case JOB_POLL:
        if (!m) break;
        m->polling = 0;
        if (j->status == JS_OK) {
            int was_up = m->state == MS_UP;
            m->state = MS_UP;
            m->seen = 1;
            m->ms = j->ms;
            m->err[0] = 0;
            apply_info(m, j->out ? (char *)j->out : "");
            if (!was_up || !m->helloed) {
                struct Job *h = job_new(JOB_HELLO, m);
                if (h) { scpy(h->arg, HELLO_TEXT, sizeof h->arg); job_send(&w_poll, h); }
                m->helloed = 1;
            }
            m->fails = 0;
        } else if (j->status == JS_AUTH) {
            m->state = MS_LOCKED;
            m->helloed = 0;
            scpy(m->err, j->err, sizeof m->err);
        } else {
            /* One miss is often just the agent busy with somebody else's
             * request (it serves one connection at a time); two is down. */
            if (++m->fails >= 2 || m->state != MS_UP) {
                m->state = MS_DOWN;
                m->helloed = 0;
            }
            scpy(m->err, j->err, sizeof m->err);
        }
        list_redraw(m);
        update_status_line();
        if (m->id == g_detail_mid) detail_show_info();
        break;

    case JOB_EXEC:
        g_exec_busy = 0;
        g_exec_mid = 0;
        if (j->status == JS_ERR && strstr(j->err, "still running")) g_stuck_mid = j->mid;
        else if (j->status == JS_OK && j->mid == g_stuck_mid) g_stuck_mid = 0;
        if (m && m->id == g_detail_mid) {
            if (j->status == JS_OK) {
                shell_add_output(j->out ? (char *)j->out : "", j->outlen);
                snprintf(b, sizeof b, "rc %ld  -  %lu ms", (long)j->rc, (unsigned long)j->ms);
            } else if (g_stuck_mid == j->mid) {
                snprintf(b, sizeof b, "Still running after %d s - Break sends it Ctrl-C.", EXEC_DEADLINE);
            } else {
                char e[120];
                sanitize(e, sizeof e, j->err, strlen(j->err));
                snprintf(b, sizeof b, "\033bFailed:\033n %s", e);
            }
            set(txt_shell, MUIA_Text_Contents, (ULONG)b);
        }
        if (m) list_redraw(m);
        shell_state();
        break;

    case JOB_BREAK:
        if (j->status == JS_OK && j->mid == g_stuck_mid) g_stuck_mid = 0;
        if (m && m->id == g_detail_mid) {
            if (j->status == JS_OK) {
                char t[120];
                sanitize(t, sizeof t, j->out ? (char *)j->out : "", j->outlen);
                snprintf(b, sizeof b, "Break: %s", t);
            } else snprintf(b, sizeof b, "Break failed: %s", j->err);
            set(txt_shell, MUIA_Text_Contents, (ULONG)b);
        }
        shell_state();
        break;

    case JOB_GET:
    case JOB_PUT:
        xfer_reply(j);
        break;

    case JOB_LIST:
        if (j->tag) { cp_fill(j->tag - 1, j); break; }
        g_files_pending[0] = 0;
        if (m && m->id == g_detail_mid) files_fill(j);
        break;

    case JOB_COPY:
        cp_copy_reply(j);
        break;

    case JOB_FOP:
        if (j->tag == 100) drives_reply(j);
        else cp_fop_reply(j);
        break;

    case JOB_SCAN: {
        int i, agents = 0, locked = 0, open = 0, added = 0;
        g_scan_busy = 0;
        g_scan_job = NULL;
        for (i = 1; i < 255; i++) {
            if (j->scan_hit[i] == SCAN_AGENT) { agents++; add_found(j->scan_net, i, j->token, &added); }
            else if (j->scan_hit[i] == SCAN_LOCKED) { locked++; add_found(j->scan_net, i, "", &added); }
            else if (j->scan_hit[i] == SCAN_OPEN) open++;
        }
        snprintf(b, sizeof b, "Found %d agent%s (%d locked, %d other); %d added.",
                 agents + locked, agents + locked == 1 ? "" : "s", locked, open, added);
        set(txt_scan, MUIA_Text_Contents, (ULONG)b);
        set(bt_scan_go, MUIA_Disabled, FALSE);
        if (added) { prefs_save(); list_rebuild(g_nmach - added); poll_all(); }
        update_status_line();
        break; }

    case JOB_HASH:
    case JOB_SHOT:
        scr_reply(j);
        break;

    case JOB_INPUT:
        mach_busy(j->mid, -1);
        if (j->status != JS_OK && j->mid == g_scr_mid) {
            snprintf(b, sizeof b, "\033bInput failed:\033n %s", j->err);
            set(txt_scr, MUIA_Text_Contents, (ULONG)b);
        }
        break;

    case JOB_VNC:
        g_vnc_active = 0;
        vnc_signalled();
        vnc_buttons();
        break;

    case JOB_HOSTID: {
        ULONG a = j->scan_net;
        g_own_ip = a;
        if (a && (a >> 24) != 127) {
            snprintf(g_scan_net_default, sizeof g_scan_net_default, "%u.%u.%u",
                     (unsigned)((a >> 24) & 255), (unsigned)((a >> 16) & 255),
                     (unsigned)((a >> 8) & 255));
        }
        break; }

    default:
        break;
    }
    job_free(j);
}

static void drain_replies(void)
{
    struct Job *j;
    while ((j = (struct Job *)GetMsg(g_reply))) handle_reply(j);
}


/* ------------------------------------------------------------------ *
 * Screen window: the agent's own view of the machine
 *
 * Cheap HASH every second; a full SHOT only when the checksum moved. The
 * agent serves one connection at a time and a big RTG SHOT takes seconds,
 * so the machine is marked busy (no polling) while one is in flight.
 * ------------------------------------------------------------------ */

static void mach_busy(ULONG mid, int delta)
{
    struct Machine *m = mach_by_id(mid);
    if (!m) return;
    if (delta > 0) m->busy++;
    else if (m->busy) m->busy--;
}

static void scr_send(UBYTE type)
{
    struct Machine *m = mach_by_id(g_scr_mid);
    struct Job *j;
    if (!m || g_scr_pending) return;
    if (!(j = job_new(type, m))) return;
    g_scr_pending = 1;
    g_scr_force = 0;
    mach_busy(m->id, 1);
    job_send(&w_screen, j);
}

static void scr_input(const UBYTE *pl, UWORD n)
{
    struct Machine *m = mach_by_id(g_scr_mid);
    struct Job *j;
    if (!m || !(j = job_new(JOB_INPUT, m))) return;
    memcpy(j->arg, pl, n);
    j->arglen = n;
    mach_busy(m->id, 1);
    job_send(&w_screen, j);
    g_scr_force = 1;            /* look again soon: something probably moved */
}

static UBYTE *put16(UBYTE *p, UWORD v) { p[0] = (UBYTE)(v >> 8); p[1] = (UBYTE)v; return p + 2; }

/* Clicks become one CLICK (the protocol's point: press and release in one
 * request, or the Amiga sees a held button); a drag becomes a SCRIPT with
 * real Amiga-side timing. Keys: press and release together, as a SCRIPT. */
static ULONG scr_view_func(struct Hook *h, Object *o, struct ViewEvent *ev)
{
    UBYTE pl[40], *p = pl;
    (void)h; (void)o;
    if (!g_scr_fb) return 0;
    if (ev->type == VIEW_BUTTON) {
        if (ev->down) { g_scr_downx = ev->x; g_scr_downy = ev->y; return 0; }
        if (abs((int)ev->x - g_scr_downx) <= 2 && abs((int)ev->y - g_scr_downy) <= 2) {
            *p++ = 5;                               /* CLICK */
            p = put16(p, ev->x); p = put16(p, ev->y);
            *p++ = ev->button; *p++ = 1;
        } else {
            *p++ = 8; *p++ = 6;                     /* SCRIPT, 6 events */
            *p++ = 1; p = put16(p, g_scr_downx); p = put16(p, g_scr_downy);
            *p++ = 2; *p++ = ev->button; *p++ = 1;
            *p++ = 9; p = put16(p, 3);
            *p++ = 1; p = put16(p, ev->x); p = put16(p, ev->y);
            *p++ = 9; p = put16(p, 3);
            *p++ = 2; *p++ = ev->button; *p++ = 0;
        }
        scr_input(pl, (UWORD)(p - pl));
    } else if (ev->type == VIEW_KEY) {
        UWORD q = (UWORD)(ev->qualifier & 0xFF);    /* shift/caps/ctrl/alt/amiga */
        if (!ev->down || (ev->rawcode >= 0x60 && ev->rawcode <= 0x67)) return 0;
        *p++ = 8; *p++ = 2;
        *p++ = 3; *p++ = ev->rawcode; *p++ = 1; p = put16(p, q);
        *p++ = 3; *p++ = ev->rawcode; *p++ = 0; p = put16(p, q);
        scr_input(pl, (UWORD)(p - pl));
    }
    return 0;
}

static void scr_set_frame(ULONG *fb, UWORD w, UWORD h)
{
    ULONG *old = g_scr_fb;
    g_scr_fb = fb; g_scr_w = w; g_scr_h = h;
    view_set_frame(view_scr, fb, w, h);
    if (old) FreeVec(old);
}

static void scr_open(struct Machine *m)
{
    static char title[80];
    if (!m) return;
    if (m->id != g_scr_mid) {
        g_scr_mid = m->id;
        g_scr_hash = 0;
        scr_set_frame(NULL, 0, 0);
        set(txt_scr, MUIA_Text_Contents, (ULONG)"Fetching the screen...");
    }
    snprintf(title, sizeof title, "%s - Screen - amifleet68", m->name);
    set(win_scr, MUIA_Window_Title, (ULONG)title);
    set(win_scr, MUIA_Window_Open, TRUE);
    g_scr_force = 1;
}

static void scr_reply(struct Job *j)
{
    char b[160];
    g_scr_pending = 0;
    mach_busy(j->mid, -1);
    if (j->mid != g_scr_mid) return;
    if (j->status != JS_OK) {
        char e[100];
        sanitize(e, sizeof e, j->err, strlen(j->err));
        snprintf(b, sizeof b, "\033b%s\033n  (%s)", e, j->type == JOB_HASH ? "HASH" : "SHOT");
        set(txt_scr, MUIA_Text_Contents, (ULONG)b);
        return;
    }
    if (j->type == JOB_HASH) {
        if (j->hash != g_scr_hash || !g_scr_fb) {
            g_scr_hash = j->hash;
            set(txt_scr, MUIA_Text_Contents, (ULONG)"Screen changed - fetching...");
            scr_send(JOB_SHOT);
        }
        return;
    }
    /* SHOT: take ownership of the converted frame */
    scr_set_frame((ULONG *)j->out, j->fw, j->fh);
    j->out = NULL;
    snprintf(b, sizeof b, "%ux%u - fetched in %lu.%lu s. Click or type over the picture to control it.",
             (unsigned)j->fw, (unsigned)j->fh,
             (unsigned long)(j->fetch_ms / 1000), (unsigned long)(j->fetch_ms % 1000 / 100));
    set(txt_scr, MUIA_Text_Contents, (ULONG)b);
}

static void scr_tick(int second)
{
    ULONG open = 0, live = 0;
    if (!g_scr_mid || g_scr_pending) return;
    get(win_scr, MUIA_Window_Open, &open);
    if (!open) return;
    get(chk_live, MUIA_Selected, &live);
    if (g_scr_force || (live && second)) scr_send(g_scr_fb ? JOB_HASH : JOB_SHOT);
}

/* ------------------------------------------------------------------ *
 * VNC window
 * ------------------------------------------------------------------ */

/* Amiga rawkey -> X keysym, for the keys that are not characters. */
static ULONG special_keysym(UBYTE raw)
{
    static const struct { UBYTE raw; ULONG sym; } t[] = {
        {0x41,0xff08},{0x42,0xff09},{0x43,0xff8d},{0x44,0xff0d},{0x45,0xff1b},{0x46,0xffff},
        {0x4c,0xff52},{0x4d,0xff54},{0x4e,0xff53},{0x4f,0xff51},{0x5f,0xff6a},
        {0x60,0xffe1},{0x61,0xffe2},{0x62,0xffe5},{0x63,0xffe3},{0x64,0xffe9},{0x65,0xffea},
        {0x66,0xffe7},{0x67,0xffe8} };
    unsigned i;
    if (raw >= 0x50 && raw <= 0x59) return 0xffbe + (raw - 0x50);      /* F1-F10 */
    for (i = 0; i < sizeof t / sizeof t[0]; i++) if (t[i].raw == raw) return t[i].sym;
    return 0;
}

static ULONG keysym_for(struct ViewEvent *ev)
{
    struct InputEvent ie;
    UBYTE buf[4];
    ULONG sym = special_keysym(ev->rawcode);
    if (sym) return sym;
    /* Characters through the local keymap - with shift/caps only: ctrl,
     * alt and the Amiga keys travel as key events of their own. */
    memset(&ie, 0, sizeof ie);
    ie.ie_Class = IECLASS_RAWKEY;
    ie.ie_Code = ev->rawcode;
    ie.ie_Qualifier = ev->qualifier & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT | IEQUALIFIER_CAPSLOCK);
    if (MapRawKey(&ie, (STRPTR)buf, sizeof buf, NULL) == 1) return buf[0];
    return 0;
}

static void vnc_queue(UBYTE type, UBYTE arg, UWORD x, UWORD y, ULONG sym)
{
    struct VncSession *vs = g_vnc;
    UWORD next;
    if (!vs || !g_vnc_active) return;
    ObtainSemaphore(&vs->sem);
    /* Coalesce pointer motion: replace a queued move with the same buttons. */
    if (type == VE_POINTER && vs->qhead != vs->qtail) {
        UWORD last = (UWORD)((vs->qtail + VNC_QLEN - 1) % VNC_QLEN);
        if (vs->q[last].type == VE_POINTER && vs->q[last].arg == arg) {
            vs->q[last].x = x; vs->q[last].y = y;
            ReleaseSemaphore(&vs->sem);
            return;
        }
    }
    next = (UWORD)((vs->qtail + 1) % VNC_QLEN);
    if (next != vs->qhead) {
        struct VncInput *e = &vs->q[vs->qtail];
        e->type = type; e->arg = arg; e->x = x; e->y = y; e->keysym = sym;
        vs->qtail = next;
    }
    ReleaseSemaphore(&vs->sem);
    if (vs->task && vs->in_sig) Signal(vs->task, vs->in_sig);
}

static ULONG vnc_view_func(struct Hook *h, Object *o, struct ViewEvent *ev)
{
    (void)h; (void)o;
    switch (ev->type) {
    case VIEW_BUTTON:
    case VIEW_MOVE:
        vnc_queue(VE_POINTER, (UBYTE)ev->buttons, ev->x, ev->y, 0);
        break;
    case VIEW_KEY: {
        ULONG sym;
        if (ev->down) {
            sym = keysym_for(ev);
            g_vnc_keysym[ev->rawcode & 127] = sym;
        } else {
            sym = g_vnc_keysym[ev->rawcode & 127];
            g_vnc_keysym[ev->rawcode & 127] = 0;
        }
        if (sym) vnc_queue(VE_KEY, ev->down, 0, 0, sym);
        break; }
    }
    return 0;
}

static void vnc_buttons(void)
{
    set(bt_vnc_go, MUIA_Disabled, g_vnc_active);
    set(bt_vnc_stop, MUIA_Disabled, !g_vnc_active);
}

static void vnc_drop_frame(void)
{
    view_set_frame(view_vnc, NULL, 0, 0);
    if (g_vnc && g_vnc->fb) { FreeVec(g_vnc->fb); g_vnc->fb = NULL; }
}

static void vnc_disconnect(void)
{
    if (!g_vnc || !g_vnc_active) return;
    g_vnc->stop = 1;
    if (g_vnc->task) Signal(g_vnc->task, SIGBREAKF_CTRL_C);
    set(txt_vnc, MUIA_Text_Contents, (ULONG)"Disconnecting...");
}

static void vnc_connect(void)
{
    struct Machine *m = mach_by_id(g_vnc_mid);
    struct VncSession *vs = g_vnc;
    struct Job *j;
    char *pw = NULL;
    ULONG fast = 0;

    if (!m || !vs || g_vnc_active) return;
    vnc_drop_frame();
    memset(vs, 0, sizeof *vs);
    InitSemaphore(&vs->sem);
    vs->gui = FindTask(NULL);
    vs->gui_sig = 1UL << g_vnc_signal;
    scpy(vs->host, m->host, sizeof vs->host);
    get(str_vncpw, MUIA_String_Contents, &pw);
    scpy(vs->password, pw && pw[0] ? pw : "amiga", 8);   /* AmiVNC: 7 max */
    get(chk_fast, MUIA_Selected, &fast);
    vs->fast = fast ? 1 : 0;
    vs->rfb_port = (UWORD)(fast ? 5901 : 5900);
    vs->may_start = 1;
    memset(g_vnc_keysym, 0, sizeof g_vnc_keysym);

    if (!(j = job_new(JOB_VNC, m))) return;
    j->vnc = vs;
    g_vnc_active = 1;
    vnc_buttons();
    set(txt_vnc, MUIA_Text_Contents, (ULONG)"Connecting...");
    job_send(&w_vnc, j);
}

static void vnc_open(struct Machine *m)
{
    static char title[80];
    if (!m) return;
    if (m->id != g_vnc_mid) {
        vnc_disconnect();
        if (!g_vnc_active) vnc_drop_frame();
        g_vnc_mid = m->id;
        set(txt_vnc, MUIA_Text_Contents,
            (ULONG)"Connect starts AmiVNC through the agent if it is not running yet.");
    }
    snprintf(title, sizeof title, "%s - VNC - amifleet68", m->name);
    set(win_vnc, MUIA_Window_Title, (ULONG)title);
    set(win_vnc, MUIA_Window_Open, TRUE);
    vnc_buttons();
}

/* The session worker signalled: new frame, damage, or a status change. */
static void vnc_signalled(void)
{
    struct VncSession *vs = g_vnc;
    char st[160];
    int newframe, dirty;
    WORD x0, y0, x1, y1;
    ULONG *fb;
    UWORD w, h;
    UBYTE state;
    ULONG frames, kb;

    if (!vs) return;
    ObtainSemaphore(&vs->sem);
    newframe = vs->newframe; vs->newframe = 0;
    dirty = vs->dirty; vs->dirty = 0;
    x0 = vs->dx0; y0 = vs->dy0; x1 = vs->dx1; y1 = vs->dy1;
    fb = vs->fb; w = vs->w; h = vs->h;
    state = vs->state;
    frames = vs->frames; kb = vs->kbytes;
    scpy(st, vs->status, sizeof st);
    ReleaseSemaphore(&vs->sem);

    /* Draw under a shared lock so the worker cannot be mid-row. */
    ObtainSemaphoreShared(&vs->sem);
    if (newframe) view_set_frame(view_vnc, fb, w, h);
    else if (dirty) view_damage(view_vnc, x0, y0, x1, y1);
    ReleaseSemaphore(&vs->sem);
    if (state == VS_RUNNING) {
        char b[200];
        snprintf(b, sizeof b, "%s  -  %ux%u, %lu updates, %lu KB", st,
                 (unsigned)w, (unsigned)h, (unsigned long)frames, (unsigned long)kb);
        set(txt_vnc, MUIA_Text_Contents, (ULONG)b);
    } else {
        char clean[160];
        sanitize(clean, sizeof clean, st, strlen(st));
        set(txt_vnc, MUIA_Text_Contents, (ULONG)clean);
    }
}

/* ------------------------------------------------------------------ *
 * Actions
 * ------------------------------------------------------------------ */

static void edit_open(int idx)
{
    char port[8];
    struct Machine *m = idx >= 0 ? &g_mach[idx] : NULL;
    g_edit_idx = idx;
    snprintf(port, sizeof port, "%u", m ? (unsigned)m->port : AGENT_PORT);
    set(str_name, MUIA_String_Contents, (ULONG)(m ? m->name : ""));
    set(str_host, MUIA_String_Contents, (ULONG)(m ? m->host : ""));
    set(str_port, MUIA_String_Contents, (ULONG)port);
    set(str_token, MUIA_String_Contents, (ULONG)(m ? m->token : ""));
    set(win_edit, MUIA_Window_Title, (ULONG)(m ? "Edit machine" : "Add machine"));
    set(win_edit, MUIA_Window_Open, TRUE);
    set(win_edit, MUIA_Window_ActiveObject, (ULONG)(m ? str_host : str_name));
}

static void edit_commit(void)
{
    char *name = NULL, *host = NULL, *port = NULL, *token = NULL;
    struct Machine *m;
    int idx;

    get(str_name, MUIA_String_Contents, &name);
    get(str_host, MUIA_String_Contents, &host);
    get(str_port, MUIA_String_Contents, &port);
    get(str_token, MUIA_String_Contents, &token);
    if (!host || !host[0]) {
        MUI_Request(app, win_edit, 0, (char *)"amifleet68", (char *)"_OK",
                    (char *)"A machine needs a host name or IP address.");
        return;
    }
    if (g_edit_idx >= 0 && g_edit_idx < g_nmach) {
        idx = g_edit_idx;
        m = &g_mach[idx];
        scpy(m->name, name && name[0] ? name : host, sizeof m->name);
        scpy(m->host, host, sizeof m->host);
        scpy(m->token, token, sizeof m->token);
        m->port = (UWORD)(port && atoi(port) > 0 ? atoi(port) : AGENT_PORT);
        m->state = MS_UNKNOWN; m->helloed = 0; m->err[0] = 0;
    } else {
        if (g_nmach >= MAX_MACH) return;
        idx = g_nmach;
        m = &g_mach[g_nmach++];
        mach_init(m, name && name[0] ? name : host, host,
                  port ? atoi(port) : AGENT_PORT, token);
    }
    set(win_edit, MUIA_Window_Open, FALSE);
    prefs_save();
    list_rebuild(idx);
    poll_machine(m);
    update_status_line();
    if (m->id == g_detail_mid) detail_show_info();
}

static void remove_selected(void)
{
    struct Machine *m = mach_selected();
    char msg[120];
    int idx;
    if (!m) return;
    snprintf(msg, sizeof msg, "Remove \033b%s\033n (%s) from the fleet?", m->name, m->host);
    if (MUI_Request(app, win, 0, (char *)"amifleet68", (char *)"_Remove|_Cancel", msg) != 1)
        return;
    if (m->id == g_detail_mid) { set(win_det, MUIA_Window_Open, FALSE); g_detail_mid = 0; }
    idx = (int)(m - g_mach);
    memmove(&g_mach[idx], &g_mach[idx + 1], sizeof g_mach[0] * (size_t)(g_nmach - idx - 1));
    g_nmach--;
    prefs_save();
    list_rebuild(idx);
    update_status_line();
    update_buttons();
}

static void scan_open(void)
{
    char *cur = NULL;
    get(str_scan_net, MUIA_String_Contents, &cur);
    if (!cur || !cur[0]) {
        const char *net = g_scan_net_default;
        char guess[16] = "";
        if (!net[0] && g_nmach) {       /* fall back to the first machine's /24 */
            const char *h = g_mach[0].host;
            const char *d = strrchr(h, '.');
            if (d && (size_t)(d - h) < sizeof guess) { memcpy(guess, h, (size_t)(d - h)); guess[d - h] = 0; }
            net = guess;
        }
        set(str_scan_net, MUIA_String_Contents, (ULONG)net);
    }
    get(str_scan_token, MUIA_String_Contents, &cur);
    if ((!cur || !cur[0]) && g_nmach)
        set(str_scan_token, MUIA_String_Contents, (ULONG)g_mach[0].token);
    set(win_scan, MUIA_Window_Open, TRUE);
}

static void scan_start(void)
{
    char *net = NULL, *token = NULL;
    unsigned a, b, c;
    struct Job *j;

    if (g_scan_busy) return;
    get(str_scan_net, MUIA_String_Contents, &net);
    get(str_scan_token, MUIA_String_Contents, &token);
    if (!net || sscanf(net, "%u.%u.%u", &a, &b, &c) != 3 || a > 255 || b > 255 || c > 255) {
        set(txt_scan, MUIA_Text_Contents, (ULONG)"Enter the first three parts of the network, e.g. 192.168.1");
        return;
    }
    if (!(j = job_new(JOB_SCAN, NULL))) return;
    j->scan_net = ((ULONG)a << 24) | ((ULONG)b << 16) | ((ULONG)c << 8);
    j->port = AGENT_PORT;
    scpy(j->token, token, sizeof j->token);
    g_scan_busy = 1;
    g_scan_job = j;
    set(bt_scan_go, MUIA_Disabled, TRUE);
    set(txt_scan, MUIA_Text_Contents, (ULONG)"Scanning...");
    job_send(&w_misc, j);
}

static void shell_run(void)
{
    struct Machine *m = mach_by_id(g_detail_mid);
    char *cmd = NULL, echo[300];
    struct Job *j;
    if (!m || g_exec_busy) return;
    get(str_cmd, MUIA_String_Contents, &cmd);
    if (!cmd || !cmd[0]) return;
    if (!(j = job_new(JOB_EXEC, m))) return;
    scpy(j->arg, cmd, sizeof j->arg);
    j->deadline = EXEC_DEADLINE;
    {
        char clean[260];
        sanitize(clean, sizeof clean, cmd, strlen(cmd));
        snprintf(echo, sizeof echo, "\033b%s>\033n %s", m->name, clean);
        shell_add(echo);
        DoMethod(lst_shell, MUIM_List_Jump, MUIV_List_Jump_Bottom);
    }
    set(str_cmd, MUIA_String_Contents, (ULONG)"");
    set(txt_shell, MUIA_Text_Contents, (ULONG)"Running...");
    g_exec_busy = 1;
    g_exec_mid = m->id;
    list_redraw(m);
    shell_state();
    job_send(&w_shell, j);
    set(win_det, MUIA_Window_ActiveObject, (ULONG)str_cmd);
}

static void shell_break(void)
{
    struct Machine *m = mach_by_id(g_detail_mid);
    struct Job *j;
    if (!m || g_exec_busy || m->id != g_stuck_mid) return;
    if (!(j = job_new(JOB_BREAK, m))) return;
    set(bt_break, MUIA_Disabled, TRUE);
    set(txt_shell, MUIA_Text_Contents, (ULONG)"Sending Ctrl-C...");
    job_send(&w_poll, j);
}

/* The AmigaGuide manual lives next to the program. MultiView runs as its own
 * process, so PROGDIR: would be ITS drawer - hand it an absolute path. */
static const char *guide_path(void)
{
    static char path[300];
    char dir[256];
    BPTR pd = GetProgramDir();
    if (!path[0]) {
        if (pd && NameFromLock(pd, (STRPTR)dir, sizeof dir) && dir[0]) {
            scpy(path, dir, sizeof path);
            AddPart((STRPTR)path, (STRPTR)"amifleet68.guide", sizeof path);
        } else scpy(path, "PROGDIR:amifleet68.guide", sizeof path);
    }
    return path;
}

static void open_manual(void)
{
    char cmd[360];
    BPTR l = Lock((STRPTR)guide_path(), ACCESS_READ);
    if (!l) {
        MUI_Request(app, win, 0, (char *)"amifleet68", (char *)"_OK",
                    (char *)"The manual (amifleet68.guide) is not next to the program.");
        return;
    }
    UnLock(l);
    snprintf(cmd, sizeof cmd, "Run >NIL: SYS:Utilities/MultiView \"%s\"", guide_path());
    SystemTags((STRPTR)cmd, TAG_DONE);
}

static void about(void)
{
    MUI_Request(app, win, 0, (char *)"About amifleet68", (char *)"_OK",
        (char *)"\033c\033bamifleet68 " AMIFLEET_VERSION "\033n (" AMIFLEET_VERDATE ")\n\n"
        "The amiagent fleet console, native on the Amiga.\n\n"
        "Watch every Amiga running amiagent, run AmigaDOS\n"
        "commands on them, move files, see and drive their\n"
        "screens, and connect to AmiVNC - all over TCP.\n\n"
        "\033i(c) 2026 Thomas Luebker - Apache License 2.0\033n\n"
        "https://github.com/thomas-luebker/amifleet68");
}


/* ------------------------------------------------------------------ *
 * ARexx: port AMIFLEET.1, on top of MUI's built-ins (QUIT, SHOW, HIDE...)
 *
 *   MACHINES                  one line per machine: name state ms agent cpu os
 *   STATE NAME/A              online | offline | token | busy | unknown
 *   POLL                      poll every machine now
 *   ADD NAME/A,HOST/A,PORT/N,TOKEN/K
 *   DETAILS NAME/A            open the details window on that machine
 *   SCREEN NAME/A             open its live screen
 *   VNC NAME/A                open the VNC window (CONNECT to start)
 *   CONNECT / DISCONNECT      the VNC window's session
 *   COPYFILES                 open the file copy window
 * ------------------------------------------------------------------ */

enum { RX_MACHINES = 1, RX_STATE, RX_POLL, RX_ADD, RX_DETAILS, RX_SCREEN, RX_VNC,
       RX_CONNECT, RX_DISCONNECT, RX_COPYFILES };

static char g_rx_result[1600];

static struct Machine *mach_by_name(const char *n)
{
    int i;
    for (i = 0; n && i < g_nmach; i++)
        if (!strcasecmp(g_mach[i].name, n) || !strcasecmp(g_mach[i].host, n)) return &g_mach[i];
    return NULL;
}

static const char *state_word(struct Machine *m)
{
    if (g_exec_busy && m->id == g_exec_mid) return "busy";
    switch (m->state) {
    case MS_UP: return "online";
    case MS_DOWN: return "offline";
    case MS_LOCKED: return "token";
    default: return "unknown";
    }
}

static void rx_select(struct Machine *m)
{
    set(lst_mach, MUIA_List_Active, (LONG)(m - g_mach));
}

static LONG rx_func(struct Hook *h, Object *o, ULONG *args)
{
    struct Machine *m = NULL;
    ULONG cmd = (ULONG)h->h_Data;
    (void)o;

    if (cmd == RX_STATE || cmd == RX_DETAILS || cmd == RX_SCREEN || cmd == RX_VNC) {
        m = mach_by_name((char *)args[0]);
        if (!m) return 10;                 /* no such machine */
    }
    switch (cmd) {
    case RX_MACHINES: {
        size_t o2 = 0;
        int i;
        g_rx_result[0] = 0;
        for (i = 0; i < g_nmach && o2 + 100 < sizeof g_rx_result; i++) {
            struct Machine *x = &g_mach[i];
            o2 += (size_t)snprintf(g_rx_result + o2, sizeof g_rx_result - o2, "%s%s %s %lu %s %s %s",
                                   i ? "\n" : "", x->name, state_word(x), (unsigned long)x->ms,
                                   x->seen ? x->agent : "-", x->seen ? x->cpu : "-", x->seen ? x->os : "-");
        }
        set(app, MUIA_Application_RexxString, (ULONG)g_rx_result);
        return 0; }
    case RX_STATE:
        scpy(g_rx_result, state_word(m), sizeof g_rx_result);
        set(app, MUIA_Application_RexxString, (ULONG)g_rx_result);
        return 0;
    case RX_POLL:
        poll_all();
        return 0;
    case RX_ADD: {
        const char *name = (char *)args[0], *host = (char *)args[1];
        LONG *port = (LONG *)args[2];
        const char *token = args[3] ? (char *)args[3] : "";
        if (g_nmach >= MAX_MACH || mach_by_name(name)) return 10;
        mach_init(&g_mach[g_nmach], name, host, port ? (int)*port : AGENT_PORT, token);
        g_nmach++;
        prefs_save();
        list_rebuild(g_nmach - 1);
        poll_machine(&g_mach[g_nmach - 1]);
        update_status_line();
        return 0; }
    case RX_DETAILS: rx_select(m); detail_open(m); return 0;
    case RX_SCREEN:  rx_select(m); scr_open(m); return 0;
    case RX_VNC:     rx_select(m); vnc_open(m); return 0;
    case RX_CONNECT:
        if (!g_vnc_mid) return 10;
        vnc_connect();
        return 0;
    case RX_DISCONNECT:
        vnc_disconnect();
        return 0;
    case RX_COPYFILES:
        cp_open();
        return 0;
    }
    return 10;
}

static struct Hook rx_hooks[10];
static struct MUI_Command rx_commands[] = {
    { (char *)"MACHINES",   NULL,                               0, &rx_hooks[0], {0} },
    { (char *)"STATE",      (char *)"NAME/A",                   1, &rx_hooks[1], {0} },
    { (char *)"POLL",       NULL,                               0, &rx_hooks[2], {0} },
    { (char *)"ADD",        (char *)"NAME/A,HOST/A,PORT/N,TOKEN/K", 4, &rx_hooks[3], {0} },
    { (char *)"DETAILS",    (char *)"NAME/A",                   1, &rx_hooks[4], {0} },
    { (char *)"SCREEN",     (char *)"NAME/A",                   1, &rx_hooks[5], {0} },
    { (char *)"VNC",        (char *)"NAME/A",                   1, &rx_hooks[6], {0} },
    { (char *)"CONNECT",    NULL,                               0, &rx_hooks[7], {0} },
    { (char *)"DISCONNECT", NULL,                               0, &rx_hooks[8], {0} },
    { (char *)"COPYFILES",  NULL,                               0, &rx_hooks[9], {0} },
    { NULL, NULL, 0, NULL, {0} }
};

static void rx_init(void)
{
    int i;
    for (i = 0; i < 10; i++) {
        rx_hooks[i].h_Entry = (APTR)HookEntry;
        rx_hooks[i].h_SubEntry = (APTR)rx_func;
        rx_hooks[i].h_Data = (APTR)(ULONG)(i + 1);
    }
}

/* ------------------------------------------------------------------ *
 * The application tree
 * ------------------------------------------------------------------ */

static const char *g_pages[] = { "Info", "Shell", "Files", NULL };

#define StatusText(init) TextObject, TextFrame, MUIA_Background, MUII_TextBack, \
    MUIA_Text_Contents, (ULONG)(init), End

static Object *menuitem(const char *title, const char *key, ULONG id)
{
    return MenuitemObject,
        MUIA_Menuitem_Title, (ULONG)title,
        key ? MUIA_Menuitem_Shortcut : TAG_IGNORE, (ULONG)key,
        MUIA_UserData, id,
    End;
}

static Object *menubar(void)
{
    return MenuitemObject, MUIA_Menuitem_Title, (ULONG)NM_BARLABEL, End;
}

static int build_app(void)
{
    rx_init();
    mach_disp_hook.h_Entry = (APTR)HookEntry;
    mach_disp_hook.h_SubEntry = (APTR)mach_disp_func;
    file_disp_hook.h_Entry = (APTR)HookEntry;
    file_disp_hook.h_SubEntry = (APTR)file_disp_func;
    scr_view_hook.h_Entry = (APTR)HookEntry;
    scr_view_hook.h_SubEntry = (APTR)scr_view_func;
    vnc_view_hook.h_Entry = (APTR)HookEntry;
    vnc_view_hook.h_SubEntry = (APTR)vnc_view_func;

    app = ApplicationObject,
        MUIA_Application_Title,       (ULONG)"amifleet68",
        MUIA_Application_Version,     (ULONG)&verstag[1],
        MUIA_Application_Author,      (ULONG)"Thomas Luebker",
        MUIA_Application_Copyright,   (ULONG)"(c) 2026 Thomas Luebker",
        MUIA_Application_Description, (ULONG)"amiagent fleet console",
        /* PALTEST gets its own base: MUI remembers window geometry per
         * base, and a test run must not shrink the real layout. */
        MUIA_Application_Base,        (ULONG)(g_paltest ? "AMIFLEETPAL" : "AMIFLEET"),
        MUIA_Application_Commands,    (ULONG)rx_commands,
        MUIA_Application_HelpFile,    (ULONG)guide_path(),     /* the Help key */

        MUIA_Application_Menustrip, (ULONG)(MenustripObject,
            /* Amiga UI Style Guide order: Project first, Quit last in it,
             * Settings last. No RAmiga-C/V/X: Intuition would take them
             * away from string gadgets (clipboard copy/paste/clear). */
            MUIA_Family_Child, MenuObjectT("Project"),
                MUIA_Family_Child, menuitem("Manual...", NULL, ID_MANUAL),
                MUIA_Family_Child, menuitem("About...", "?", ID_ABOUT),
                MUIA_Family_Child, menuitem("About MUI...", NULL, ID_ABOUTMUI),
                MUIA_Family_Child, menubar(),
                MUIA_Family_Child, menuitem("Quit", "Q", MUIV_Application_ReturnID_Quit),
            End,
            MUIA_Family_Child, MenuObjectT("Fleet"),
                MUIA_Family_Child, menuitem("Add machine...", "A", ID_ADD),
                MUIA_Family_Child, menuitem("Edit machine...", "E", ID_EDIT),
                MUIA_Family_Child, menuitem("Remove machine...", NULL, ID_REMOVE),
                MUIA_Family_Child, menubar(),
                MUIA_Family_Child, menuitem("Poll now", "P", ID_POLL),
                MUIA_Family_Child, menuitem("Scan network...", "S", ID_SCAN),
            End,
            MUIA_Family_Child, MenuObjectT("Windows"),
                MUIA_Family_Child, menuitem("Details...", "D", ID_DETAILS),
                MUIA_Family_Child, menuitem("Screen...", "W", ID_SCREEN),
                MUIA_Family_Child, menuitem("VNC...", "N", ID_VNC),
                MUIA_Family_Child, menuitem("Copy files...", "F", ID_COPYWIN),
            End,
            MUIA_Family_Child, MenuObjectT("Settings"),
                MUIA_Family_Child, menuitem("MUI...", NULL, ID_MUIPREFS),
            End,
        End),

        /* ---- the fleet board ---- */
        SubWindow, win = WindowObject,
            MUIA_Window_Title, (ULONG)"amifleet68 " AMIFLEET_VERSION " - Amiga fleet",
            MUIA_Window_ID,    MAKE_ID('A','F','L','T'),
            MUIA_Window_Width, MUIV_Window_Width_Visible(40),
            WindowContents, VGroup,
                /* Two rows: one row of nine buttons is ~720 px in topaz/8, and
                 * the MUI style guide wants every window to fit 640x256. */
                Child, HGroup,
                    Child, bt_add     = SimpleButton("_Add..."),
                    Child, bt_edit    = SimpleButton("_Edit..."),
                    Child, bt_remove  = SimpleButton("_Remove..."),
                    Child, bt_poll    = SimpleButton("_Poll now"),
                    Child, bt_scan    = SimpleButton("_Scan..."),
                End,
                Child, HGroup,
                    Child, bt_details = SimpleButton("_Details..."),
                    Child, bt_screen  = SimpleButton("Scree_n..."),
                    Child, bt_vnc     = SimpleButton("_VNC..."),
                    Child, bt_copy    = SimpleButton("_Copy..."),
                End,
                Child, lv_mach = ListviewObject,
                    MUIA_CycleChain, 1,
                    /* WEIGHTs only - MIW/MAW are percent clamps that made
                     * amipkg-mui's window refuse to open on big-font RTG. */
                    MUIA_Listview_List, lst_mach = ListObject, InputListFrame,
                        MUIA_List_Format, (ULONG)"BAR WEIGHT=100,BAR WEIGHT=40,BAR P=\033r WEIGHT=30,"
                                                 "BAR WEIGHT=40,BAR WEIGHT=30,BAR WEIGHT=25,"
                                                 "BAR P=\033r WEIGHT=35,P=\033r WEIGHT=35",
                        MUIA_List_Title, TRUE,
                        MUIA_List_DisplayHook, (ULONG)&mach_disp_hook,
                    End,
                End,
                Child, txt_status = StatusText("Starting..."),
            End,
        End,

        /* ---- add / edit ---- */
        SubWindow, win_edit = WindowObject,
            MUIA_Window_Title, (ULONG)"Add machine",
            MUIA_Window_ID,    MAKE_ID('A','F','E','D'),
            WindowContents, VGroup,
                Child, ColGroup(2),
                    Child, KeyLabel2("Name:", 'n'),
                    Child, str_name = StringObject, StringFrame, MUIA_ControlChar, 'n',
                        MUIA_String_MaxLen, 31, MUIA_CycleChain, 1, End,
                    Child, KeyLabel2("Host:", 'h'),
                    Child, str_host = StringObject, StringFrame, MUIA_ControlChar, 'h',
                        MUIA_String_MaxLen, 63, MUIA_CycleChain, 1, End,
                    Child, KeyLabel2("Port:", 'p'),
                    Child, str_port = StringObject, StringFrame, MUIA_ControlChar, 'p',
                        MUIA_String_MaxLen, 6, MUIA_String_Accept, (ULONG)"0123456789",
                        MUIA_CycleChain, 1, End,
                    Child, KeyLabel2("Token:", 't'),
                    Child, str_token = StringObject, StringFrame, MUIA_ControlChar, 't',
                        MUIA_String_MaxLen, 63, MUIA_String_Secret, TRUE,
                        MUIA_CycleChain, 1, End,
                End,
                Child, TextObject, MUIA_Font, MUIV_Font_Tiny,
                    MUIA_Text_Contents, (ULONG)"The token is the TOKEN= the agent was started with.",
                End,
                Child, HGroup,
                    Child, bt_edit_ok = SimpleButton("_OK"),
                    Child, HSpace(0),
                    Child, bt_edit_cancel = SimpleButton("_Cancel"),
                End,
            End,
        End,

        /* ---- scan ---- */
        SubWindow, win_scan = WindowObject,
            MUIA_Window_Title, (ULONG)"Scan network",
            MUIA_Window_ID,    MAKE_ID('A','F','S','C'),
            WindowContents, VGroup,
                Child, TextObject,
                    MUIA_Text_Contents, (ULONG)"Sweeps x.y.z.1-254 for amiagent (port 7846)\n"
                                               "and adds every agent it finds.",
                End,
                Child, ColGroup(2),
                    Child, KeyLabel2("Network:", 'n'),
                    Child, str_scan_net = StringObject, StringFrame, MUIA_ControlChar, 'n',
                        MUIA_String_MaxLen, 15, MUIA_String_Accept, (ULONG)"0123456789.",
                        MUIA_CycleChain, 1, End,
                    Child, KeyLabel2("Token:", 't'),
                    Child, str_scan_token = StringObject, StringFrame, MUIA_ControlChar, 't',
                        MUIA_String_MaxLen, 63, MUIA_String_Secret, TRUE,
                        MUIA_CycleChain, 1, End,
                End,
                Child, txt_scan = StatusText("Enter the first three parts of your network, e.g. 192.168.1"),
                Child, HGroup,
                    Child, bt_scan_go = SimpleButton("_Scan"),
                    Child, HSpace(0),
                    Child, bt_scan_close = SimpleButton("_Close"),
                End,
            End,
        End,

        /* ---- screen (agent HASH/SHOT) ---- */
        SubWindow, win_scr = WindowObject,
            MUIA_Window_Title,   (ULONG)"Screen - amifleet68",
            MUIA_Window_ID,      MAKE_ID('A','F','S','V'),
            MUIA_Window_NoMenus, TRUE,         /* right button goes to the picture */
            MUIA_Window_Width,   MUIV_Window_Width_Visible(50),
            MUIA_Window_Height,  MUIV_Window_Height_Visible(50),
            WindowContents, VGroup,
                Child, view_scr = view_new(&scr_view_hook),
                Child, HGroup,
                    Child, txt_scr = StatusText(""),
                    Child, chk_live = MUI_MakeObject(MUIO_Checkmark, (ULONG)"_Live"),
                    Child, Label1("_Live"),
                    Child, bt_scr_refresh = SimpleButton("_Refresh"),
                End,
            End,
        End,

        /* ---- VNC ---- */
        SubWindow, win_vnc = WindowObject,
            MUIA_Window_Title,   (ULONG)"VNC - amifleet68",
            MUIA_Window_ID,      MAKE_ID('A','F','V','N'),
            MUIA_Window_NoMenus, TRUE,
            MUIA_Window_Width,   MUIV_Window_Width_Visible(50),
            MUIA_Window_Height,  MUIV_Window_Height_Visible(50),
            WindowContents, VGroup,
                Child, view_vnc = view_new(&vnc_view_hook),
                Child, txt_vnc = StatusText(""),
                Child, HGroup,
                    Child, KeyLabel2("Password:", 'p'),
                    Child, str_vncpw = StringObject, StringFrame, MUIA_ControlChar, 'p',
                        MUIA_String_MaxLen, 8, MUIA_String_Secret, TRUE,
                        MUIA_String_Contents, (ULONG)"amiga",
                        MUIA_CycleChain, 1, End,
                    Child, chk_fast = MUI_MakeObject(MUIO_Checkmark, (ULONG)"_Fast"),
                    Child, Label1("_Fast (8-bit)"),
                    Child, bt_vnc_go   = SimpleButton("_Connect"),
                    Child, bt_vnc_stop = SimpleButton("_Disconnect"),
                End,
            End,
        End,

        /* ---- file copy ---- */
        SubWindow, win_copy = WindowObject,
            MUIA_Window_Title, (ULONG)"Copy files - amifleet68",
            MUIA_Window_ID,    MAKE_ID('A','F','C','P'),
            MUIA_Window_Width,  MUIV_Window_Width_Visible(55),
            MUIA_Window_Height, MUIV_Window_Height_Visible(55),
            WindowContents, VGroup,
                Child, HGroup,
                    Child, cp_pane(0),
                    Child, BalanceObject, End,
                    Child, cp_pane(1),
                End,
                Child, HGroup,
                    Child, bt_cp_right = SimpleButton("Copy _>>"),
                    Child, bt_cp_left  = SimpleButton("_<< Copy"),
                    Child, MUI_MakeObject(MUIO_VBar, 4),
                    Child, bt_cp_del   = SimpleButton("De_lete..."),
                    Child, bt_cp_ren   = SimpleButton("Re_name..."),
                    Child, bt_cp_mkd   = SimpleButton("_MakeDir..."),
                    Child, bt_cp_ref   = SimpleButton("Re_fresh"),
                End,
                Child, HGroup,
                    Child, txt_cp = TextObject, TextFrame, MUIA_Background, MUII_TextBack,
                        MUIA_HorizWeight, 400,
                        MUIA_Text_Contents, (ULONG)"Mark entries (shift-click for several), then Copy >> or << Copy.", End,
                    Child, bt_cp_cancel = SimpleButton("_Cancel"),
                End,
            End,
        End,

        /* ---- a one-line question (Rename, MakeDir) ---- */
        SubWindow, win_ask = WindowObject,
            MUIA_Window_Title, (ULONG)"amifleet68",
            MUIA_Window_ID,    MAKE_ID('A','F','A','K'),
            WindowContents, VGroup,
                Child, txt_ask = TextObject, MUIA_Text_Contents, (ULONG)"", End,
                Child, str_ask = StringObject, StringFrame, MUIA_String_MaxLen, 100,
                    MUIA_CycleChain, 1, End,
                Child, HGroup,
                    Child, bt_ask_ok = SimpleButton("_OK"),
                    Child, HSpace(0),
                    Child, bt_ask_cancel = SimpleButton("_Cancel"),
                End,
            End,
        End,

        /* ---- details ---- */
        SubWindow, win_det = WindowObject,
            MUIA_Window_Title, (ULONG)"amifleet68",
            MUIA_Window_ID,    MAKE_ID('A','F','D','T'),
            WindowContents, VGroup,
                Child, txt_det_head = StatusText(""),
                Child, RegisterGroup(g_pages),
                    MUIA_CycleChain, 1,

                    /* Info */
                    Child, VGroup,
                        Child, ListviewObject,
                            MUIA_Listview_Input, FALSE,
                            MUIA_Listview_List, ft_info = FloattextObject, ReadListFrame,
                                MUIA_Floattext_Text, (ULONG)"",
                            End,
                        End,
                        Child, HGroup,
                            Child, HSpace(0),
                            Child, bt_info_refresh = SimpleButton("Re_fresh"),
                        End,
                    End,

                    /* Shell */
                    Child, VGroup,
                        Child, lv_shell = ListviewObject,
                            MUIA_Listview_Input, FALSE,
                            MUIA_Listview_List, lst_shell = ListObject, ReadListFrame,
                                MUIA_List_ConstructHook, MUIV_List_ConstructHook_String,
                                MUIA_List_DestructHook,  MUIV_List_DestructHook_String,
                            End,
                        End,
                        Child, HGroup,
                            Child, KeyLabel2("Command:", 'm'),
                            Child, str_cmd = StringObject, StringFrame, MUIA_ControlChar, 'm',
                                MUIA_String_MaxLen, 500, MUIA_CycleChain, 1, End,
                        End,
                        Child, HGroup,
                            Child, txt_shell = StatusText(""),
                            Child, bt_run   = SimpleButton("_Run"),
                            Child, bt_break = SimpleButton("_Break"),
                            Child, bt_clear = SimpleButton("C_lear"),
                        End,
                    End,

                    /* Files */
                    Child, VGroup,
                        Child, HGroup,
                            Child, Label2("Path:"),
                            Child, drive_field(2, &g_detail_mid, ID_FILES_GO, "SYS:"),
                            Child, bt_parent = SimpleButton("_Parent"),
                        End,
                        Child, lv_files = ListviewObject,
                            MUIA_CycleChain, 1,
                            MUIA_Listview_List, lst_files = ListObject, InputListFrame,
                                MUIA_List_Format, (ULONG)"BAR WEIGHT=100,BAR P=\033r WEIGHT=20,BAR WEIGHT=20,WEIGHT=35",
                                MUIA_List_Title, TRUE,
                                MUIA_List_DisplayHook, (ULONG)&file_disp_hook,
                            End,
                        End,
                        Child, HGroup,
                            Child, txt_files = TextObject, TextFrame, MUIA_Background, MUII_TextBack,
                                MUIA_HorizWeight, 300, MUIA_Text_Contents, (ULONG)"", End,
                            Child, bt_down = SimpleButton("_Download..."),
                            Child, bt_up   = SimpleButton("_Upload..."),
                        End,
                    End,
                End,
            End,
        End,
    End;
    if (!app) return 0;

    /* Notifications: every one becomes a ReturnID handled in the main loop. */
#define RET(obj, attr, val, id) \
    DoMethod((obj), MUIM_Notify, (attr), (val), (ULONG)app, 2, MUIM_Application_ReturnID, (id))

    DoMethod(app, MUIM_Notify, MUIA_Application_MenuAction, MUIV_EveryTime,
             (ULONG)app, 2, MUIM_Application_ReturnID, MUIV_TriggerValue);
    RET(win, MUIA_Window_CloseRequest, TRUE, MUIV_Application_ReturnID_Quit);
    RET(bt_add, MUIA_Pressed, FALSE, ID_ADD);
    RET(bt_edit, MUIA_Pressed, FALSE, ID_EDIT);
    RET(bt_remove, MUIA_Pressed, FALSE, ID_REMOVE);
    RET(bt_details, MUIA_Pressed, FALSE, ID_DETAILS);
    RET(bt_poll, MUIA_Pressed, FALSE, ID_POLL);
    RET(bt_scan, MUIA_Pressed, FALSE, ID_SCAN);
    RET(lv_mach, MUIA_Listview_DoubleClick, TRUE, ID_LIST_DCLICK);
    RET(bt_screen, MUIA_Pressed, FALSE, ID_SCREEN);
    RET(bt_vnc, MUIA_Pressed, FALSE, ID_VNC);
    RET(bt_copy, MUIA_Pressed, FALSE, ID_COPYWIN);

    {   /* Style guide: every window shows the program on the screen title
         * bar, Help on any window lands on its own manual page, keyboard
         * users reach every button with Tab. */
        static const char stitle[] = "amifleet68 " AMIFLEET_VERSION;
        struct { Object **w; const char *node; } wins[] = {
            { &win, "Board" }, { &win_edit, "Start" }, { &win_scan, "Start" },
            { &win_scr, "Screen" }, { &win_vnc, "VNC" }, { &win_copy, "Copy" },
            { &win_ask, "Copy" }, { &win_det, "Details" } };
        Object *buttons[] = { bt_add, bt_edit, bt_remove, bt_poll, bt_scan, bt_details,
            bt_screen, bt_vnc, bt_copy, bt_edit_ok, bt_edit_cancel, bt_scan_go, bt_scan_close,
            bt_scr_refresh, bt_vnc_go, bt_vnc_stop, bt_cp_right, bt_cp_left, bt_cp_del,
            bt_cp_ren, bt_cp_mkd, bt_cp_ref, bt_cp_cancel, bt_ask_ok, bt_ask_cancel,
            bt_info_refresh, bt_run, bt_break, bt_clear, bt_parent, bt_down, bt_up,
            g_pane[0].bt_parent, g_pane[1].bt_parent, chk_live, chk_fast };
        unsigned k;
        for (k = 0; k < sizeof wins / sizeof wins[0]; k++) {
            set(*wins[k].w, MUIA_Window_ScreenTitle, (ULONG)stitle);
            set(*wins[k].w, MUIA_HelpNode, (ULONG)wins[k].node);
        }
        for (k = 0; k < sizeof buttons / sizeof buttons[0]; k++)
            if (buttons[k]) set(buttons[k], MUIA_CycleChain, 1);
        set(win, MUIA_Window_DefaultObject, (ULONG)lv_mach);       /* cursor keys move the selection */
        set(win_copy, MUIA_Window_DefaultObject, (ULONG)g_pane[0].lv);
    }
    cp_notify();
    drive_notify();
    str_path = g_dpop[2].str;
    RET(win_scr, MUIA_Window_CloseRequest, TRUE, ID_SCR_CLOSE);
    RET(bt_scr_refresh, MUIA_Pressed, FALSE, ID_SCR_REFRESH);
    RET(win_vnc, MUIA_Window_CloseRequest, TRUE, ID_VNC_CLOSE);
    RET(bt_vnc_go, MUIA_Pressed, FALSE, ID_VNC_CONNECT);
    RET(bt_vnc_stop, MUIA_Pressed, FALSE, ID_VNC_DISCONNECT);
    DoMethod(lst_mach, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime,
             (ULONG)app, 2, MUIM_Application_ReturnID, ID_SELECT);

    RET(bt_edit_ok, MUIA_Pressed, FALSE, ID_EDIT_OK);
    RET(bt_edit_cancel, MUIA_Pressed, FALSE, ID_EDIT_CANCEL);
    RET(win_edit, MUIA_Window_CloseRequest, TRUE, ID_EDIT_CANCEL);
    RET(str_token, MUIA_String_Acknowledge, MUIV_EveryTime, ID_EDIT_OK);

    RET(bt_scan_go, MUIA_Pressed, FALSE, ID_SCAN_GO);
    RET(bt_scan_close, MUIA_Pressed, FALSE, ID_SCAN_CLOSE);
    RET(win_scan, MUIA_Window_CloseRequest, TRUE, ID_SCAN_CLOSE);

    RET(win_det, MUIA_Window_CloseRequest, TRUE, ID_DETAIL_CLOSE);
    RET(bt_info_refresh, MUIA_Pressed, FALSE, ID_INFO_REFRESH);
    RET(bt_run, MUIA_Pressed, FALSE, ID_SHELL_RUN);
    RET(str_cmd, MUIA_String_Acknowledge, MUIV_EveryTime, ID_SHELL_RUN);
    RET(bt_break, MUIA_Pressed, FALSE, ID_SHELL_BREAK);
    RET(bt_clear, MUIA_Pressed, FALSE, ID_SHELL_CLEAR);
    RET(str_path, MUIA_String_Acknowledge, MUIV_EveryTime, ID_FILES_GO);
    RET(bt_parent, MUIA_Pressed, FALSE, ID_FILES_PARENT);
    RET(lv_files, MUIA_Listview_DoubleClick, TRUE, ID_FILES_DCLICK);
    RET(bt_down, MUIA_Pressed, FALSE, ID_FILES_DOWNLOAD);

    /* Defaults: live screen on; VNC in AmiVNC's 1-byte-per-pixel mode, the
     * sensible choice for a 68k viewer. */
    set(chk_live, MUIA_Selected, TRUE);
    set(chk_fast, MUIA_Selected, TRUE);

#define HELP(o, t) set((o), MUIA_ShortHelp, (ULONG)(t))
    HELP(bt_add,     "Add a machine running amiagent by host name or address.");
    HELP(bt_edit,    "Change the selected machine's name, host, port or token.");
    HELP(bt_remove,  "Take the selected machine off the board.");
    HELP(bt_details, "Full report, an AmigaDOS shell and a file browser\nfor the selected machine. (Or double-click it.)");
    HELP(bt_screen,  "The selected machine's screen, grabbed through the agent.\nClicks and keys over the picture go to that machine.");
    HELP(bt_vnc,     "A VNC session to AmiVNC on the selected machine.\nStarts AmiVNC through the agent if it is not running.");
    HELP(bt_poll,    "Ask every machine for a fresh report now.");
    HELP(bt_copy,    "Copy files and drawers between any two machines\n(or within one), plus Delete, Rename and MakeDir.");
    HELP(bt_cp_right,"Copy the marked entries from the left pane\ninto the right pane's drawer.");
    HELP(bt_cp_left, "Copy the marked entries from the right pane\ninto the left pane's drawer.");
    HELP(bt_cp_del,  "Delete the marked entries in the pane you last used.");
    HELP(bt_scan,    "Sweep the local network for machines running amiagent.");
    HELP(bt_run,     "Run the command through the agent's EXEC (20 s).");
    HELP(bt_break,   "Send Ctrl-C to a command the agent left running.");
    HELP(bt_down,    "Save the selected file on this Amiga.");
    HELP(bt_up,      "Send a file from this Amiga into the drawer shown.");
    HELP(chk_live,   "Keep the picture current (checks once a second,\nfetches only when the screen changed).");
    HELP(chk_fast,   "AmiVNC's 256-colour mode: a quarter of the data.");
    HELP(str_vncpw,  "AmiVNC password, 7 characters at most.");
#undef HELP
    RET(bt_up, MUIA_Pressed, FALSE, ID_FILES_UPLOAD);

    return 1;
}

/* ------------------------------------------------------------------ *
 * Main loop
 * ------------------------------------------------------------------ */

static struct MsgPort *g_tport;
static struct timerequest *g_treq;
static int g_timer_ok;

static void timer_start(void)
{
    g_treq->tr_node.io_Command = TR_ADDREQUEST;
    g_treq->tr_time.tv_secs = 0;
    g_treq->tr_time.tv_micro = 500000;      /* half-second ticks */
    SendIO((struct IORequest *)g_treq);
}

static void handle_id(LONG id, int *done)
{
    struct Machine *m;
    if (cp_handle(id)) return;
    switch (id) {
    case MUIV_Application_ReturnID_Quit: *done = 1; break;
    case ID_ABOUT: about(); break;
    case ID_MANUAL: open_manual(); break;
    case ID_ABOUTMUI: DoMethod(app, MUIM_Application_AboutMUI, (ULONG)win); break;
    case ID_MUIPREFS: DoMethod(app, MUIM_Application_OpenConfigWindow, 0); break;
    case ID_ADD: edit_open(-1); break;
    case ID_EDIT:
        if ((m = mach_selected())) edit_open((int)(m - g_mach));
        break;
    case ID_REMOVE: remove_selected(); break;
    case ID_POLL: poll_all(); set_status("Polling..."); break;
    case ID_SCAN: scan_open(); break;
    case ID_DETAILS:
    case ID_LIST_DCLICK:
        detail_open(mach_selected());
        break;
    case ID_SCREEN: scr_open(mach_selected()); break;
    case ID_SCR_CLOSE: set(win_scr, MUIA_Window_Open, FALSE); break;
    case ID_SCR_REFRESH: g_scr_hash = 0; if (!g_scr_pending) scr_send(JOB_SHOT); break;
    case ID_VNC: vnc_open(mach_selected()); break;
    case ID_VNC_CONNECT: vnc_connect(); break;
    case ID_VNC_DISCONNECT: vnc_disconnect(); break;
    case ID_VNC_CLOSE: vnc_disconnect(); set(win_vnc, MUIA_Window_Open, FALSE); break;
    case ID_EDIT_OK: edit_commit(); break;
    case ID_EDIT_CANCEL: set(win_edit, MUIA_Window_Open, FALSE); break;
    case ID_SCAN_GO: scan_start(); break;
    case ID_SCAN_CLOSE: set(win_scan, MUIA_Window_Open, FALSE); break;
    case ID_DETAIL_CLOSE: set(win_det, MUIA_Window_Open, FALSE); break;
    case ID_INFO_REFRESH:
        if ((m = mach_by_id(g_detail_mid))) poll_machine(m);
        break;
    case ID_SHELL_RUN: shell_run(); break;
    case ID_SHELL_BREAK: shell_break(); break;
    case ID_SHELL_CLEAR: DoMethod(lst_shell, MUIM_List_Clear); break;
    case ID_FILES_GO: {
        char *p = NULL;
        get(str_path, MUIA_String_Contents, &p);
        if (p && p[0]) files_request(p);
        break; }
    case ID_FILES_PARENT: files_parent(); break;
    case ID_FILES_DOWNLOAD: files_download(); break;
    case ID_FILES_UPLOAD: files_upload(); break;
    case ID_FILES_DCLICK: {
        struct FileEnt *e = NULL;
        DoMethod(lst_files, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, (ULONG)&e);
        if (e && e->is_dir) {
            char p[256];
            files_join(p, sizeof p, g_files_path, e->name);
            files_request(p);
        }
        break; }
    default: break;
    }
    if (id == ID_SELECT || id == ID_REMOVE || id == ID_EDIT_OK) update_buttons();
}

static int start_workers(void)
{
    if (!worker_start(&w_poll, "amifleet68 poll", 16384)) return 0;
    if (!worker_start(&w_shell, "amifleet68 shell", 16384)) return 0;
    if (!worker_start(&w_misc, "amifleet68 misc", 16384)) return 0;
    if (!worker_start(&w_screen, "amifleet68 screen", 16384)) return 0;
    if (!worker_start(&w_vnc, "amifleet68 vnc", 16384)) return 0;
    if (!worker_start(&w_copy, "amifleet68 copy", 65536)) return 0;   /* recursion */
    return 1;
}

static void stop_workers(void)
{
    static struct Job quit[6];
    struct Worker *ws[6];
    int i;

    ws[0] = &w_poll; ws[1] = &w_shell; ws[2] = &w_misc; ws[3] = &w_screen; ws[4] = &w_vnc;
    ws[5] = &w_copy;
    g_quitting = 1;
    if (g_vnc) g_vnc->stop = 1;
    if (g_copy_spec) g_copy_spec->cancel = 1;
    for (i = 0; i < 6; i++) if (ws[i]->proc) Signal((struct Task *)ws[i]->proc, SIGBREAKF_CTRL_C);
    for (i = 0; i < 6; i++) {
        if (!ws[i]->proc) continue;
        memset(&quit[i], 0, sizeof quit[i]);
        quit[i].msg.mn_ReplyPort = g_reply;
        quit[i].msg.mn_Length = sizeof quit[i];
        quit[i].type = JOB_QUIT;
        PutMsg(ws[i]->port, &quit[i].msg);
        g_outstanding++;
    }
    while (g_outstanding > 0) {
        struct Job *j;
        WaitPort(g_reply);
        while ((j = (struct Job *)GetMsg(g_reply))) {
            g_outstanding--;
            if (j->type == JOB_COPY && j->cs) FreeVec(j->cs);
            if (j->type != JOB_QUIT) job_free(j);
        }
    }
}

/* PALTEST: every window on a 640x256 PAL hires screen, topaz/8 - the size
 * the MUI style guide says everything must fit ("amifleet68 PALTEST"). */
static struct Screen *g_testscr = NULL;
static struct DiskObject *g_dobj = NULL;

static void on_test_screen(void)
{
    Object *ws[] = { win, win_edit, win_scan, win_scr, win_vnc, win_copy, win_ask, win_det };
    unsigned k;
    if (!g_testscr) return;
    for (k = 0; k < sizeof ws / sizeof ws[0]; k++) set(ws[k], MUIA_Window_Screen, (ULONG)g_testscr);
}

static int gui_run(void)
{
    int rc = RETURN_FAIL;
    ULONG opened = 0, tsig = 0, rsig, vsig = 0;

    UtilityBase   = OpenLibrary((STRPTR)"utility.library", 37);
    MUIMasterBase = OpenLibrary((STRPTR)MUIMASTER_NAME, 19);
    if (!MUIMasterBase || !UtilityBase) {
        printf("amifleet68: MUI 3.8+ is required (muimaster.library v19).\n");
        goto out;
    }
    CyberGfxBase = OpenLibrary((STRPTR)"cybergraphics.library", 40);   /* optional */
    AslBase = OpenLibrary((STRPTR)"asl.library", 38);                   /* optional */
    if (!view_init()) { printf("amifleet68: could not create the view class.\n"); goto out; }
    if (!(g_reply = CreateMsgPort())) goto out;
    g_vnc = (struct VncSession *)AllocVec(sizeof *g_vnc, MEMF_ANY | MEMF_CLEAR);
    g_vnc_signal = AllocSignal(-1);
    if (!g_vnc || g_vnc_signal < 0) goto out;
    vsig = 1UL << g_vnc_signal;
    if (!start_workers()) {
        printf("amifleet68: could not open bsdsocket.library - is the TCP/IP stack running?\n");
        goto out;
    }

    g_tport = CreateMsgPort();
    if (g_tport && (g_treq = (struct timerequest *)CreateIORequest(g_tport, sizeof *g_treq)) &&
        OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)g_treq, 0) == 0)
        g_timer_ok = 1;

    prefs_load();
    if (!g_nmach)   /* first run: the agent on this very machine */
        mach_init(&g_mach[g_nmach++], "This Amiga", "127.0.0.1", AGENT_PORT, "");

    if (g_paltest) {
        static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, 0, 0 };
        static UWORD pens[] = { (UWORD)~0 };
        g_testscr = OpenScreenTags(NULL,
            SA_Width, 640, SA_Height, 256, SA_Depth, 3,
            SA_DisplayID, PAL_MONITOR_ID | HIRES_KEY,
            SA_Font, (ULONG)&topaz8, SA_Pens, (ULONG)pens,
            SA_Title, (ULONG)"amifleet68 PALTEST - 640x256 topaz/8",
            TAG_DONE);
        if (!g_testscr) {       /* no PAL monitor driver: any 640x256 mode */
            ULONG id = BestModeID(BIDTAG_NominalWidth, 640, BIDTAG_NominalHeight, 256,
                                  BIDTAG_Depth, 3, TAG_DONE);
            if (id != (ULONG)INVALID_ID)
                g_testscr = OpenScreenTags(NULL,
                    SA_Width, 640, SA_Height, 256, SA_Depth, 3, SA_DisplayID, id,
                    SA_Font, (ULONG)&topaz8, SA_Pens, (ULONG)pens,
                    SA_Title, (ULONG)"amifleet68 PALTEST - 640x256 topaz/8", TAG_DONE);
        }
        if (!g_testscr) { printf("amifleet68: PALTEST screen did not open.\n"); fflush(stdout); }
    }
    /* The program's own icon: MUI shows it when the app is iconified. */
    g_dobj = GetDiskObject((STRPTR)"PROGDIR:amifleet68");
    if (!g_dobj) g_dobj = GetDiskObject((STRPTR)"PROGDIR:amifleet68.020");

    if (!build_app()) {
        printf("amifleet68: could not create the application.\n");
        goto out;
    }
    if (g_dobj) set(app, MUIA_Application_DiskObject, (ULONG)g_dobj);
    on_test_screen();
    list_rebuild(0);
    update_buttons();
    update_status_line();

    set(win, MUIA_Window_Open, TRUE);
    get(win, MUIA_Window_Open, &opened);
    if (!opened) {
        printf("amifleet68: the window did not open (screen too small,\n"
               "broken MUI prefs, or an incomplete MUI install?).\n");
        goto out;
    }

    {   /* our own address, to pre-fill the scan window */
        struct Job *j = job_new(JOB_HOSTID, NULL);
        if (j) job_send(&w_misc, j);
    }
    poll_all();
    if (g_timer_ok) { timer_start(); tsig = 1UL << g_tport->mp_SigBit; }
    rsig = 1UL << g_reply->mp_SigBit;

    {
        int done = 0;
        long zeros = 0;
        ULONG tick = 0;
        while (!done) {
            ULONG sigs = 0;
            LONG id = DoMethod(app, MUIM_Application_NewInput, (ULONG)&sigs);
            if (id) handle_id(id, &done);
            if (done) break;
            if (sigs) {
                zeros = 0;
                sigs = Wait(sigs | tsig | rsig | vsig | SIGBREAKF_CTRL_C);
                if (sigs & SIGBREAKF_CTRL_C) done = 1;
                if (sigs & rsig) drain_replies();
                if (sigs & vsig) vnc_signalled();
                if (sigs & tsig) {
                    while (GetMsg(g_tport)) ;
                    ++tick;
                    if (tick % (POLL_SECS * 2) == 0) poll_all();
                    scr_tick(tick % 2 == 0);
                    xfer_progress();
                    cp_progress();
                    if (g_scan_busy && g_scan_job) {
                        char b[80];
                        ULONG d = g_scan_job->scan_done;
                        if (d >= 254) snprintf(b, sizeof b, "Asking the agents found...");
                        else snprintf(b, sizeof b, "Scanning... %lu of 254 addresses", (unsigned long)d);
                        set(txt_scan, MUIA_Text_Contents, (ULONG)b);
                    }
                    if (!done) timer_start();
                }
            } else if (++zeros > 100) {
                Delay(1);
            }
        }
    }
    rc = RETURN_OK;

out:
    if (app) {
        set(win, MUIA_Window_Open, FALSE);
    }
    if (g_reply) stop_workers();
    if (g_timer_ok) {
        if (!CheckIO((struct IORequest *)g_treq)) AbortIO((struct IORequest *)g_treq);
        WaitIO((struct IORequest *)g_treq);
        CloseDevice((struct IORequest *)g_treq);
    }
    if (g_treq) DeleteIORequest((struct IORequest *)g_treq);
    if (g_tport) DeleteMsgPort(g_tport);
    if (g_reply) DeleteMsgPort(g_reply);
    if (g_files) FreeVec(g_files);
    {   int i; for (i = 0; i < MAX_MACH; i++) if (g_mach[i].drives) FreeVec(g_mach[i].drives); }
    if (app) MUI_DisposeObject(app);
    if (g_scr_fb) FreeVec(g_scr_fb);
    if (g_vnc) { if (g_vnc->fb) FreeVec(g_vnc->fb); FreeVec(g_vnc); }
    if (g_vnc_signal >= 0) FreeSignal(g_vnc_signal);
    view_exit();
    if (g_testscr) CloseScreen(g_testscr);
    if (g_dobj) FreeDiskObject(g_dobj);
    if (CyberGfxBase) CloseLibrary(CyberGfxBase);
    if (AslBase) CloseLibrary(AslBase);
    if (MUIMasterBase) CloseLibrary(MUIMasterBase);
    if (UtilityBase) CloseLibrary(UtilityBase);
    return rc;
}

/* StackSwap wrapper (same idiom as amipkg-mui and amimon-mui). */
static struct StackSwapStruct g_sss;
static char *g_stk;
static int   g_rc;

int main(void)
{
    /* From a Shell: "amifleet68 PALTEST". Read the raw argument string -
     * the startup's argc/argv did not carry it through (measured). */
    {
        STRPTR args = GetArgStr();
        if (args && (strstr((char *)args, "PALTEST") || strstr((char *)args, "paltest"))) g_paltest = 1;
    }
    g_stk = (char *)AllocMem(STACK_BYTES, MEMF_ANY);
    if (!g_stk) return gui_run();
    g_sss.stk_Lower   = (APTR)g_stk;
    g_sss.stk_Upper   = (ULONG)g_stk + STACK_BYTES;
    g_sss.stk_Pointer = (APTR)((ULONG)g_stk + STACK_BYTES);
    StackSwap(&g_sss);
    g_rc = gui_run();
    StackSwap(&g_sss);
    FreeMem(g_stk, STACK_BYTES);
    return g_rc;
}
