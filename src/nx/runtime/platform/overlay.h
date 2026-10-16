/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's menu: Android's back button, a gamepad's Home, the on-screen Home or F2 opens it over the game, in the
   menu's look (src/gui, the YoRHa theme), drawn by the renderer as quads (ui.h). The same on every platform: the
   on-screen controls' options (vpad.h), the touch screen's, and Exit */
#ifndef SNX_OVERLAY_H
#define SNX_OVERLAY_H
#include "nx/runtime/platform/ui.h"

#define OVERLAY_COLS 40 /* the panel, in cells of the 8x16 font */
#define OVERLAY_ROWS 16 /* the tallest page's: the others are as tall as their items */
enum { OVERLAY_NONE, OVERLAY_CLOSE, OVERLAY_EXIT, OVERLAY_EDIT };

/* whether it is open, for the renderer's thread too */
int overlay_shown(void);
void overlay_toggle(void);
/* the display's own scale (SDL's: dpi / 160 on a phone), which the panel's letters follow; 1 until told */
void overlay_set_display_scale(float scale);
/* where the panel goes on a picture of sw x sh pixels: its corner and the whole number it is scaled by */
void overlay_place(int sw, int sh, int* x, int* y, int* scale);
int overlay_select(void);      /* the focused item's action: a page, a setting changed, or an OVERLAY_ for the caller */
void overlay_move(int rows);   /* the focus up (-1) or down (1), round the page */
void overlay_scroll(int rows); /* the mouse's wheel: the same, stopping at the ends */
void overlay_side(int step);   /* the focused setting down (-1) or up (1) */
int overlay_back(void);        /* the page above; OVERLAY_CLOSE from the first */
/* a button pressed, as the game reads it (one HidNpadButton bit): the D-pad and the left stick move, A selects, B goes
   back, the rest do nothing. What it did, as overlay_select */
int overlay_pad(u64 button);
/* the pads each poll, the on-screen controls and the connected ones: their buttons and sticks at ms. With the menu
   shown, each button pressed since the last call acts as overlay_pad, the left stick past half way as the D-pad; a
   direction held goes on after SNX_OVERLAY_REPEAT_DELAY ms, every SNX_OVERLAY_REPEAT_MS; a press held as the menu opens
   does not act. What the last one did, or the first that leaves the menu */
#define SNX_OVERLAY_REPEAT_DELAY 400
#define SNX_OVERLAY_REPEAT_MS    120
int overlay_controls(u64 buttons, const i32 sticks[4], u64 ms);
/* the button map page waits for a control pressed on a pad (pad_map.h's): it takes it, and that press is no menu
   key */
int overlay_listening(void);
void overlay_press(int control);
/* a tap at x, y on a picture of sw x sh: what it did, OVERLAY_CLOSE outside the panel */
int overlay_tap(float x, float y, int sw, int sh);
void overlay_draw(SnxUiList* l, int sw, int sh);
/* a fatal error's panel (backend/sdl3.c), the menu's own: the head, the game's name, the error, what it means and what to
   do, cut to its width, and Close in focus */
typedef struct {
    float x, y, w, h;
} SnxOverlayRect;
/* log and link may be NULL. A visible log is underlined and its bounds returned
   in link. A hidden or absent log returns an empty rectangle. */
void overlay_error_draw(SnxUiList* l, int sw, int sh, const char* name, const char* msg, const char* how,
                        const char* log, SnxOverlayRect* link);
int overlay_error_link(const SnxOverlayRect* link, float x, float y);

#endif
