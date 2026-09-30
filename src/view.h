/* view.h - FrameView, the MUI class that shows a remote screen (see view.c). */
#ifndef AMIFLEET_VIEW_H
#define AMIFLEET_VIEW_H

#include <exec/types.h>
#include <intuition/classusr.h>
#include <utility/hooks.h>

#define VIEW_Hook (TAG_USER | 0x0AF10001)   /* struct Hook * for ViewEvents */

enum { VIEW_BUTTON = 1, VIEW_MOVE, VIEW_KEY };

/* Passed as the hook message; coordinates are REMOTE pixels. */
struct ViewEvent {
    UBYTE type;
    UBYTE button;       /* VIEW_BUTTON: 0 left, 1 right, 2 middle */
    UBYTE down;         /* VIEW_BUTTON / VIEW_KEY */
    UBYTE rawcode;      /* VIEW_KEY */
    UWORD buttons;      /* held buttons, RFB bits: 1 left, 2 middle, 4 right */
    UWORD x, y;
    UWORD qualifier;
    ULONG seconds, micros;
    struct IntuiMessage *imsg;
};

int     view_init(void);
void    view_exit(void);
Object *view_new(struct Hook *hook);
void    view_set_frame(Object *obj, ULONG *fb, int w, int h);   /* fb not owned */
void    view_damage(Object *obj, int x0, int y0, int x1, int y1);

#endif
