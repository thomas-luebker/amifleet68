/*
 * view.c - FrameView, a MUI Area subclass that shows a remote screen.
 *
 * The frame is a 0RGB ULONG array owned by someone else (the Screen window's
 * last SHOT, or a VNC session). The view scales it to fit its box, keeping
 * the aspect ratio and never enlarging, and redraws either everything
 * (MADF_DRAWOBJECT) or just the damaged part (MADF_DRAWUPDATE, from
 * view_damage()).
 *
 * Drawing:
 *   - RTG screen deeper than 8 bits: cybergraphics WritePixelArray, ARGB.
 *   - anything else: a 6x6x6 colour cube of pens from ObtainBestPen, chunky
 *     pixels through graphics.library WriteChunkyPixels (v40, OS 3.1).
 *
 * Input: clicks, pointer motion and keys over the picture are handed to the
 * owner's hook as a ViewEvent in REMOTE coordinates. Keys go to the remote
 * machine while the pointer is over the picture, and are eaten there.
 */

#ifndef __amigaos__
#error "amifleet68 targets AmigaOS - build with m68k-amigaos-gcc (see Makefile)"
#endif

#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/classes.h>
#include <graphics/gfx.h>
#include <graphics/view.h>
#include <devices/inputevent.h>
#include <libraries/mui.h>
#include <cybergraphx/cybergraphics.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/utility.h>
#include <proto/muimaster.h>
#include <inline/cybergraphics.h>
#include <clib/alib_protos.h>

#include "view.h"

extern struct Library *CyberGfxBase;     /* opened (or not) by main.c */

#define BLOCK_ROWS 16

struct ViewData {
    ULONG *fb;
    UWORD  fw, fh;
    struct Hook *hook;
    struct MUI_EventHandlerNode ehn;
    UBYTE  shown, rtg, havepens, dirty, update;
    WORD   dx0, dy0, dx1, dy1;          /* damaged source rect */
    WORD   ox, oy, dw, dh;              /* where the picture sits, last draw */
    UBYTE  pens[216];
    struct ColorMap *cm;
    UBYTE *blk;                         /* BLOCK_ROWS rows of output pixels */
    ULONG  blkcap;
    WORD  *xmap;                        /* dest column -> source column */
    ULONG  xmapcap;
    WORD   xmap_dw, xmap_fw;
    UWORD  btnmask;
};

static struct MUI_CustomClass *g_mcc;

/* ------------------------------------------------------------------ */

static void place(Object *obj, struct ViewData *d)
{
    LONG aw = _mwidth(obj), ah = _mheight(obj);
    LONG dw = d->fw, dh = d->fh;
    if (!d->fb || !d->fw || !d->fh || aw <= 0 || ah <= 0) { d->dw = d->dh = 0; return; }
    if (dw > aw || dh > ah) {
        if ((LONG)d->fw * ah > (LONG)d->fh * aw) { dw = aw; dh = (LONG)d->fh * aw / d->fw; }
        else { dh = ah; dw = (LONG)d->fw * ah / d->fh; }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    }
    d->dw = (WORD)dw; d->dh = (WORD)dh;
    d->ox = (WORD)(_mleft(obj) + (aw - dw) / 2);
    d->oy = (WORD)(_mtop(obj) + (ah - dh) / 2);
}

static int ensure(struct ViewData *d)
{
    ULONG need = (ULONG)d->dw * BLOCK_ROWS * 4;
    LONG x;
    if (need > d->blkcap) {
        if (d->blk) FreeVec(d->blk);
        d->blk = (UBYTE *)AllocVec(need, MEMF_ANY);
        d->blkcap = d->blk ? need : 0;
        if (!d->blk) return 0;
    }
    if ((ULONG)d->dw > d->xmapcap) {
        if (d->xmap) FreeVec(d->xmap);
        d->xmap = (WORD *)AllocVec((ULONG)d->dw * sizeof(WORD), MEMF_ANY);
        d->xmapcap = d->xmap ? (ULONG)d->dw : 0;
        if (!d->xmap) return 0;
        d->xmap_dw = 0;
    }
    if (d->xmap_dw != d->dw || d->xmap_fw != (WORD)d->fw) {
        for (x = 0; x < d->dw; x++) d->xmap[x] = (WORD)((LONG)x * d->fw / d->dw);
        d->xmap_dw = d->dw; d->xmap_fw = (WORD)d->fw;
    }
    return 1;
}

/* Render source rect [sx0,sx1) x [sy0,sy1) through the scaling. */
static void render(Object *obj, struct ViewData *d, LONG sx0, LONG sy0, LONG sx1, LONG sy1)
{
    struct RastPort *rp = _rp(obj);
    LONG rx0, rx1, ry0, ry1, ry, w;

    if (!d->dw || !d->dh || !ensure(d)) return;
    if (sx0 < 0) sx0 = 0;
    if (sy0 < 0) sy0 = 0;
    if (sx1 > d->fw) sx1 = d->fw;
    if (sy1 > d->fh) sy1 = d->fh;
    if (sx1 <= sx0 || sy1 <= sy0) return;

    rx0 = sx0 * d->dw / d->fw;
    rx1 = (sx1 * d->dw + d->fw - 1) / d->fw;
    ry0 = sy0 * d->dh / d->fh;
    ry1 = (sy1 * d->dh + d->fh - 1) / d->fh;
    if (rx1 > d->dw) rx1 = d->dw;
    if (ry1 > d->dh) ry1 = d->dh;
    w = rx1 - rx0;
    if (w <= 0) return;

    for (ry = ry0; ry < ry1; ry += BLOCK_ROWS) {
        LONG rows = ry1 - ry < BLOCK_ROWS ? ry1 - ry : BLOCK_ROWS, r, x;
        for (r = 0; r < rows; r++) {
            const ULONG *src = d->fb + (ULONG)((ry + r) * d->fh / d->dh) * d->fw;
            if (d->rtg) {
                ULONG *o = (ULONG *)d->blk + r * w;
                for (x = 0; x < w; x++) o[x] = src[d->xmap[rx0 + x]];
            } else {
                UBYTE *o = d->blk + r * w;
                for (x = 0; x < w; x++) {
                    ULONG c = src[d->xmap[rx0 + x]];
                    o[x] = d->pens[((c >> 16 & 255) * 6 >> 8) * 36 +
                                   ((c >> 8 & 255) * 6 >> 8) * 6 + ((c & 255) * 6 >> 8)];
                }
            }
        }
        if (d->rtg)
            WritePixelArray(d->blk, 0, 0, (UWORD)(w * 4), rp,
                            (UWORD)(d->ox + rx0), (UWORD)(d->oy + ry), (UWORD)w, (UWORD)rows,
                            RECTFMT_ARGB);
        else if (GfxBase->LibNode.lib_Version >= 40)
            WriteChunkyPixels(rp, d->ox + rx0, d->oy + ry, d->ox + rx0 + w - 1,
                              d->oy + ry + rows - 1, d->blk, w);
    }
}

static void fill_margins(Object *obj, struct ViewData *d)
{
    struct RastPort *rp = _rp(obj);
    LONG l = _mleft(obj), t = _mtop(obj), r = _mright(obj), b = _mbottom(obj);
    SetAPen(rp, _pens(obj)[MPEN_SHADOW]);
    if (!d->dw || !d->dh) { RectFill(rp, l, t, r, b); return; }
    if (d->oy > t) RectFill(rp, l, t, r, d->oy - 1);
    if (d->oy + d->dh <= b) RectFill(rp, l, d->oy + d->dh, r, b);
    if (d->ox > l) RectFill(rp, l, d->oy, d->ox - 1, d->oy + d->dh - 1);
    if (d->ox + d->dw <= r) RectFill(rp, d->ox + d->dw, d->oy, r, d->oy + d->dh - 1);
}

/* Window pixel -> remote pixel; 0 when outside the picture. */
static int to_remote(struct ViewData *d, WORD mx, WORD my, UWORD *x, UWORD *y)
{
    if (!d->dw || !d->dh || mx < d->ox || my < d->oy ||
        mx >= d->ox + d->dw || my >= d->oy + d->dh) return 0;
    *x = (UWORD)((LONG)(mx - d->ox) * d->fw / d->dw);
    *y = (UWORD)((LONG)(my - d->oy) * d->fh / d->dh);
    return 1;
}

/* ------------------------------------------------------------------ */

static void get_pens(Object *obj, struct ViewData *d)
{
    struct Screen *scr = _screen(obj);
    struct BitMap *bm = scr->RastPort.BitMap;
    int r, g, b, i = 0;

    d->rtg = CyberGfxBase && GetCyberMapAttr(bm, CYBRMATTR_ISCYBERGFX) &&
             GetCyberMapAttr(bm, CYBRMATTR_DEPTH) > 8;
    if (d->rtg) return;
    d->cm = scr->ViewPort.ColorMap;
    for (r = 0; r < 6; r++)
        for (g = 0; g < 6; g++)
            for (b = 0; b < 6; b++) {
                LONG p = ObtainBestPen(d->cm, (ULONG)(r * 51) * 0x01010101UL,
                                       (ULONG)(g * 51) * 0x01010101UL,
                                       (ULONG)(b * 51) * 0x01010101UL,
                                       OBP_Precision, PRECISION_IMAGE, TAG_DONE);
                d->pens[i++] = (UBYTE)(p < 0 ? 1 : p);
            }
    d->havepens = 1;
}

static void free_pens(struct ViewData *d)
{
    int i;
    if (!d->havepens) return;
    for (i = 0; i < 216; i++) ReleasePen(d->cm, d->pens[i]);
    d->havepens = 0;
}

static ULONG handle_event(struct IClass *cl, Object *obj, struct MUIP_HandleEvent *msg)
{
    struct ViewData *d = INST_DATA(cl, obj);
    struct IntuiMessage *im = msg->imsg;
    struct ViewEvent ev;
    UWORD x, y;
    int inside;

    if (!im || !d->hook || !d->fb) return 0;
    inside = to_remote(d, im->MouseX, im->MouseY, &x, &y);
    ev.x = x; ev.y = y;
    ev.qualifier = im->Qualifier;
    ev.seconds = im->Seconds; ev.micros = im->Micros;
    ev.imsg = im;

    switch (im->Class) {
    case IDCMP_MOUSEBUTTONS: {
        UWORD bit = 0;
        switch (im->Code & ~IECODE_UP_PREFIX) {
        case IECODE_LBUTTON: bit = 1; ev.button = 0; break;
        case IECODE_RBUTTON: bit = 4; ev.button = 1; break;
        case IECODE_MBUTTON: bit = 2; ev.button = 2; break;
        default: return 0;
        }
        ev.down = !(im->Code & IECODE_UP_PREFIX);
        /* Presses must start on the picture; releases always follow. */
        if (ev.down && !inside) return 0;
        if (!ev.down && !(d->btnmask & bit)) return 0;
        if (ev.down) d->btnmask |= bit; else d->btnmask &= (UWORD)~bit;
        if (!inside) {      /* released outside: clamp to the edge */
            WORD mx = im->MouseX, my = im->MouseY;
            if (mx < d->ox) mx = d->ox;
            if (my < d->oy) my = d->oy;
            if (mx >= d->ox + d->dw) mx = (WORD)(d->ox + d->dw - 1);
            if (my >= d->oy + d->dh) my = (WORD)(d->oy + d->dh - 1);
            to_remote(d, mx, my, &x, &y);
            ev.x = x; ev.y = y;
        }
        ev.type = VIEW_BUTTON;
        ev.buttons = d->btnmask;
        CallHookPkt(d->hook, obj, &ev);
        return MUI_EventHandlerRC_Eat;
    }
    case IDCMP_MOUSEMOVE:
        if (!inside) return 0;
        ev.type = VIEW_MOVE;
        ev.buttons = d->btnmask;
        CallHookPkt(d->hook, obj, &ev);
        return 0;
    case IDCMP_RAWKEY:
        if (!inside) return 0;
        if ((im->Code & ~IECODE_UP_PREFIX) >= 0x68) return 0;   /* not a key */
        ev.type = VIEW_KEY;
        ev.rawcode = (UBYTE)(im->Code & ~IECODE_UP_PREFIX);
        ev.down = !(im->Code & IECODE_UP_PREFIX);
        CallHookPkt(d->hook, obj, &ev);
        return MUI_EventHandlerRC_Eat;
    }
    return 0;
}

static ULONG view_dispatch(register struct IClass *cl __asm("a0"),
                           register Object *obj __asm("a2"),
                           register Msg msg __asm("a1"))
{
    struct ViewData *d;

    switch (msg->MethodID) {
    case OM_NEW: {
        Object *o = (Object *)DoSuperMethodA(cl, obj, msg);
        if (o) {
            struct TagItem *t = FindTagItem(VIEW_Hook, ((struct opSet *)msg)->ops_AttrList);
            d = INST_DATA(cl, o);
            d->hook = t ? (struct Hook *)t->ti_Data : NULL;
        }
        return (ULONG)o;
    }
    case OM_DISPOSE:
        d = INST_DATA(cl, obj);
        if (d->blk) FreeVec(d->blk);
        if (d->xmap) FreeVec(d->xmap);
        break;

    case MUIM_AskMinMax: {
        struct MUI_MinMax *mm;
        DoSuperMethodA(cl, obj, msg);
        mm = ((struct MUIP_AskMinMax *)msg)->MinMaxInfo;
        mm->MinWidth += 160;  mm->MinHeight += 100;
        mm->DefWidth += 640;  mm->DefHeight += 400;
        mm->MaxWidth += MUI_MAXMAX; mm->MaxHeight += MUI_MAXMAX;
        return 0;
    }

    case MUIM_Setup:
        if (!DoSuperMethodA(cl, obj, msg)) return FALSE;
        d = INST_DATA(cl, obj);
        get_pens(obj, d);
        d->ehn.ehn_Priority = 0;
        d->ehn.ehn_Flags = 0;
        d->ehn.ehn_Object = obj;
        d->ehn.ehn_Class = cl;
        d->ehn.ehn_Events = IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_RAWKEY;
        DoMethod(_win(obj), MUIM_Window_AddEventHandler, (ULONG)&d->ehn);
        return TRUE;

    case MUIM_Cleanup:
        d = INST_DATA(cl, obj);
        DoMethod(_win(obj), MUIM_Window_RemEventHandler, (ULONG)&d->ehn);
        free_pens(d);
        break;

    case MUIM_Show:
        d = INST_DATA(cl, obj);
        d->shown = 1;
        break;
    case MUIM_Hide:
        d = INST_DATA(cl, obj);
        d->shown = 0;
        d->btnmask = 0;
        break;

    case MUIM_Draw: {
        ULONG flags = ((struct MUIP_Draw *)msg)->flags;
        DoSuperMethodA(cl, obj, msg);
        d = INST_DATA(cl, obj);
        /* Do not trust msg->flags: MUI 3.8 (19.35, A4000) delivers 0 here
         * for MUI_Redraw(MADF_DRAWOBJECT) and MADF_DRAWUPDATE alike. Our own
         * flag says whether view_damage() asked for a partial update;
         * anything else (expose, resize, new frame) is a full redraw. */
        (void)flags;
        if (d->update && d->dirty && d->fb && d->dw) {
            render(obj, d, d->dx0, d->dy0, d->dx1, d->dy1);
        } else {
            place(obj, d);
            fill_margins(obj, d);
            if (d->fb) render(obj, d, 0, 0, d->fw, d->fh);
        }
        d->update = 0;
        d->dirty = 0;
        return 0;
    }

    case MUIM_HandleEvent:
        return handle_event(cl, obj, (struct MUIP_HandleEvent *)msg);
    }
    return DoSuperMethodA(cl, obj, msg);
}

/* ------------------------------------------------------------------ */

int view_init(void)
{
    g_mcc = MUI_CreateCustomClass(NULL, (char *)MUIC_Area, NULL,
                                  sizeof(struct ViewData), (APTR)view_dispatch);
    return g_mcc != NULL;
}

void view_exit(void)
{
    if (g_mcc) MUI_DeleteCustomClass(g_mcc);
    g_mcc = NULL;
}

Object *view_new(struct Hook *hook)
{
    return (Object *)NewObject(g_mcc->mcc_Class, NULL,
        MUIA_Frame, MUIV_Frame_Virtual,
        MUIA_FillArea, FALSE,
        VIEW_Hook, (ULONG)hook,
        TAG_DONE);
}

void view_set_frame(Object *obj, ULONG *fb, int w, int h)
{
    struct ViewData *d = INST_DATA(g_mcc->mcc_Class, obj);
    d->fb = fb;
    d->fw = (UWORD)(fb ? w : 0);
    d->fh = (UWORD)(fb ? h : 0);
    d->dirty = 0;
    d->update = 0;
    if (d->shown) MUI_Redraw(obj, MADF_DRAWOBJECT);
}

void view_damage(Object *obj, int x0, int y0, int x1, int y1)
{
    struct ViewData *d = INST_DATA(g_mcc->mcc_Class, obj);
    if (!d->fb) return;
    if (!d->dirty) { d->dx0 = (WORD)x0; d->dy0 = (WORD)y0; d->dx1 = (WORD)x1; d->dy1 = (WORD)y1; }
    else {
        if (x0 < d->dx0) d->dx0 = (WORD)x0;
        if (y0 < d->dy0) d->dy0 = (WORD)y0;
        if (x1 > d->dx1) d->dx1 = (WORD)x1;
        if (y1 > d->dy1) d->dy1 = (WORD)y1;
    }
    d->dirty = 1;
    d->update = 1;
    if (d->shown) MUI_Redraw(obj, MADF_DRAWUPDATE);
}
