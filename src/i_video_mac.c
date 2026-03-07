/*
 * i_video_mac.c — Video interface for Doom SE/30
 *
 * Doom renders to a 320x200 8-bit buffer (screens[0]).
 * During gameplay (GS_LEVEL), wall/floor/ceiling rendering goes directly to
 * the 1-bit framebuffer via R_DrawColumn_Mono / R_DrawSpan_Mono.
 * I_FinishUpdate blits only the non-view area (status bar, border) and any
 * HUD/menu overlay pixels (non-zero in screens[0]) over the direct render.
 *
 * During menus, wipes, intermission: full screens[0] blit as before.
 */

#include <QuickDraw.h>
#include <Memory.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "doomdef.h"
#include "doomstat.h"
#include "d_main.h"
#include "i_system.h"
#include "i_video.h"
#include "v_video.h"

/* ---- Framebuffer info — non-static so r_draw.c can access them ---- */
byte           *fb_mono_base     = NULL;  /* 1-bit framebuffer base address */
int             fb_mono_rowbytes = 0;     /* bytes per row in framebuffer */
int             fb_mono_xoff     = 0;     /* (fb_width - SCREENWIDTH) / 2 */
int             fb_mono_yoff     = 0;     /* (fb_height - SCREENHEIGHT) / 2 */

/* Grayscale palette: maps Doom's 256-color palette to 0-255 grayscale.
 * Non-static so r_draw.c can use it for direct 1-bit rendering.
 * Values are gamma-corrected (dither_gamma applied). */
byte            grayscale_pal[256];

/* Merged colormap → grayscale lookup.
 * mono_colormaps[level*256 + pal_idx] = grayscale_pal[colormaps[level*256 + pal_idx]]
 * 34 levels × 256 entries = 8704 bytes.
 * Collapses the two-level indirection (colormap then grayscale_pal) into one
 * table lookup in R_DrawColumn_Mono / R_DrawSpan_Mono inner loops.
 * Rebuilt whenever grayscale_pal changes (I_SetPalette / I_RebuildDitherPalette). */
byte            mono_colormaps[34 * 256];

/* Raw grayscale palette: BT.601 luminance WITHOUT gamma correction.
 * Used as the input to the status bar contrast stretch. */
static byte     raw_gray[256];

/* Status bar contrast-stretched palette.
 * The Doom status bar was designed for color displays: the STBAR background,
 * STT digit glyphs, and STYSNUM weapon numbers all have raw luminance
 * crammed into a narrow band (~70-160).  A hard threshold at 128 on raw
 * values makes nearly everything black; Bayer dithering on raw values
 * produces near-identical dot densities for background and digits.
 *
 * sbar_gray[] linearly stretches the [SBAR_BLACK..SBAR_WHITE] raw-gray band
 * to [0..255], then we threshold at 128:
 *   raw < SBAR_BLACK  → 0   → BLACK  (dark background and shadows)
 *   raw > SBAR_WHITE  → 255 → WHITE  (baked labels, weapon numbers)
 *   in between        → linearly mapped: digit highlights land > 128 → WHITE,
 *                       background and digit shadows land < 128 → BLACK.
 *
 * Tuning: lower SBAR_BLACK → more pixels become white (brighter overall).
 *         raise SBAR_WHITE  → fewer pixels become white (more contrast). */
/* Status bar contrast uses the RED channel instead of BT.601 luminance.
 * The Doom damage palette (red flash) makes the status bar look great because
 * it shifts all colors toward red — which happens to create good contrast
 * between the brownish/olive STBAR background (moderate red) and the
 * yellow/orange STT digit glyphs (very high red).  Using the red channel
 * directly replicates this permanently without waiting for damage.
 *
 * Tune: SBAR_BLACK = red value below which pixel → black (try 90–110)
 *       SBAR_WHITE = red value above which pixel → white (try 140–180) */
#define SBAR_BLACK  58   /* background brown R ≈ 60-95 → black  */
#define SBAR_WHITE 193   /* digit yellow/orange R ≈ 180-240 → white */
static byte     sbar_gray[256];

/* 4x4 Bayer ordered dither threshold matrix (0-15 scaled to 0-255).
 * Non-static so r_draw.c can use the same matrix. */
const byte bayer4x4[4][4] = {
    {  0, 136,  34, 170 },
    { 204,  68, 238, 102 },
    {  51, 187,  17, 153 },
    { 255, 119, 221,  85 }
};

/* Dedicated menu overlay buffer — M_Drawer renders here (not into screens[1],
 * which Doom uses as its border tile cache). Non-static so d_main.c can use it
 * to redirect M_Drawer output. */
byte *menu_overlay_buf = NULL;

/* Set to true by d_main.c during the screen-wipe loop to force full blit.
 * During a wipe, wipe_ScreenWipe owns every pixel of screens[0]; we must not
 * skip the view area or the melt will show corrupted content. */
boolean wipe_in_progress = false;

/* hu_overlay_active — set by HU_Drawer when a message or chat widget is
 * visible in the view area.  Cleared by D_Display before HU_Drawer runs.
 * When false, I_FinishUpdate skips the screens[0] view-area overlay scan
 * entirely (saves ~3136 blit8_black reads per frame — ~40ms).            */
boolean hu_overlay_active = false;

/* border_needs_blit — set by d_main.c when R_DrawViewBorder/R_FillBackScreen
 * write new content into the screens[0] border area.  I_FinishUpdate caches
 * the 1-bit framebuffer border and only re-blits when this flag is set or
 * after a non-direct frame (wipe/menu) that may have overwritten the fb.  */
int border_needs_blit = 1;

/* Double-buffering: all rendering writes to fb_offscreen_buf (via fb_mono_base).
 * I_FinishUpdate copies the completed frame to real_fb_base (the actual screen)
 * in one memcpy, preventing the mid-frame floor/ceiling fill from being visible. */
static void          *real_fb_base     = NULL; /* actual screen memory (qd.screenBits.baseAddr) */
static unsigned char *fb_offscreen_buf = NULL; /* off-screen rendering target */

/* View geometry — from r_draw.c/r_main.c, used for selective blit */
extern int viewwindowx;
extern int viewwindowy;
extern int viewheight;
extern int scaledviewwidth;

/* ---- Runtime-tunable dither parameters ---------------------------------- */
/* All are non-static so I_AdjustDither (called from i_input_mac.c) can
 * modify them, and so they can be saved/loaded via doom_dither.cfg.       */

float dither_gamma  = 0.52f; /* gamma exponent: <1 brightens midtones       */
int   dither_r_wt   = 121;   /* red   luminance weight (sum ~250)            */
int   dither_g_wt   = 104;   /* green luminance weight                       */
int   dither_b_wt   = 25;    /* blue  luminance weight                       */
int   dither_gblack = 55;    /* game contrast: input black-point (0-255)     */
int   dither_gwhite = 160;   /* game contrast: input white-point (0-255)     */

/* no_lighting: when non-zero, all rendering uses the fullbright colormap.
 * Non-static so r_draw.c can read it each frame.                           */
int   no_lighting   = 0;

/* fog_scale: distance threshold (fixed_t units; 0=off) — defined in d_main.c,
 * referenced here for save/load and runtime adjustment.                     */
extern int fog_scale;

/* Saved copy of the last palette Doom passed to I_SetPalette.
 * Needed to rebuild grayscale_pal when dither params change at runtime.   */
static byte saved_palette[768];
static int  palette_valid = 0;   /* set to 1 after first I_SetPalette call  */
static int  mono_dirty    = 0;   /* set when grayscale_pal changes; cleared by
                                  * I_FinishUpdate. Bounds mono_colormaps rebuild
                                  * to once per rendered frame even if I_SetPalette
                                  * fires many times per second (damage, pickups). */

/* Precomputed gamma curve: gray_in (0-255) -> gray_out (0-255).
 * Rebuilt whenever dither_gamma changes (in I_RebuildDitherPalette).      */
static byte gamma_curve[256];

/* colormaps is defined in r_data.c; lighttable_t = byte.
 * We reference it here to build mono_colormaps.           */
extern byte *colormaps;

/* ---- Dither config helpers ---------------------------------------------- */

/* Integer square root (Newton-Raphson).  No libm needed. */
static unsigned int isqrt(unsigned int n)
{
    unsigned int x, x1;
    if (n == 0) return 0;
    x = n;
    do {
        x1 = (x + n / x) >> 1;
        if (x1 >= x) break;
        x = x1;
    } while (1);
    return x;
}

/* Rebuild mono_colormaps from current grayscale_pal + colormaps.
 * Called after grayscale_pal is updated (I_SetPalette / I_RebuildDitherPalette).
 * colormaps must be valid (set by R_InitColormaps before I_InitGraphics).    */
static void I_BuildMonoColormaps(void)
{
    int i;
    if (!colormaps) return;
    for (i = 0; i < 34 * 256; i++)
        mono_colormaps[i] = grayscale_pal[(byte)colormaps[i]];
    /* Note: doom_log removed here — fflush on every rebuild was measurable overhead. */
}

/* Rebuild gamma_curve from current dither_gamma.  Called at startup and
 * whenever dither_gamma changes at runtime.
 *
 * Pure integer implementation — NO exp/log/pow, NO math.h.
 * Using those functions links the SANE transcendental library on 68k Mac,
 * which inflates the A5-relative data segment past the 32 KB addressing
 * limit and silently corrupts globals on real SE/30 hardware.
 *
 * Instead we compute exact values at five gamma anchors using integer sqrt:
 *   gamma=0.25: v025 = isqrt(isqrt(g*255)*255)   [= 255*(g/255)^0.25]
 *   gamma=0.5:  v05  = isqrt(g*255)               [= 255*(g/255)^0.5 ]
 *   gamma=1.0:  v10  = g                           [identity           ]
 *   gamma=2.0:  v20  = g*g/255                     [= 255*(g/255)^2.0 ]
 *   gamma=4.0:  v40  = v20*v20/255                 [= 255*(g/255)^4.0 ]
 * then linearly interpolate between the two nearest anchors.              */
void I_BuildGammaCurve(void)
{
    int g;
    /* Convert dither_gamma to integer × 100 (basic float multiply — no SANE) */
    int gam100 = (int)(dither_gamma * 100.0f + 0.5f);

    gamma_curve[0]   = 0;
    gamma_curve[255] = 255;

    for (g = 1; g < 255; g++) {
        unsigned int v025, v05, v10, v20, v40;
        unsigned int out;
        unsigned int frac;

        /* Anchor values */
        v05  = isqrt((unsigned int)g * 255u);
        v025 = isqrt(v05              * 255u);
        v10  = (unsigned int)g;
        v20  = (unsigned int)g * (unsigned int)g / 255u;
        v40  = v20 * v20 / 255u;

        /* Interpolate.  All anchors decrease with rising gamma, so
         * differences are always non-negative in unsigned arithmetic.      */
        if (gam100 <= 25) {
            /* gamma ≤ 0.25: clamp to v025 (brightest anchor) */
            out = v025;
        } else if (gam100 <= 50) {
            /* [0.25, 0.50]: v025 → v05, frac in [0, 25] */
            frac = (unsigned int)(gam100 - 25);
            out  = v025 - (v025 - v05) * frac / 25u;
        } else if (gam100 <= 100) {
            /* [0.50, 1.00]: v05 → v10, frac in [0, 50] */
            frac = (unsigned int)(gam100 - 50);
            out  = v05  - (v05  - v10) * frac / 50u;
        } else if (gam100 <= 200) {
            /* [1.00, 2.00]: v10 → v20, frac in [0, 100] */
            frac = (unsigned int)(gam100 - 100);
            out  = v10  - (v10  - v20) * frac / 100u;
        } else {
            /* [2.00, 4.00]: v20 → v40, frac in [0, 200]; gam max = 3.0 */
            frac = (unsigned int)(gam100 - 200);
            if (frac > 200u) frac = 200u;
            out  = v20  - (v20  - v40) * frac / 200u;
        }

        gamma_curve[g] = (byte)(out > 255u ? 255u : out);
    }

    doom_log("I_BuildGammaCurve: dg=%.2f gc[64]=%d gc[128]=%d gc[192]=%d\r",
             dither_gamma, gamma_curve[64], gamma_curve[128], gamma_curve[192]);
}

/* Rebuild grayscale_pal from current params + saved palette.
 * Called after any dither param changes at runtime.                        */
void I_RebuildDitherPalette(void)
{
    int i;
    int wt_sum;

    if (!palette_valid) return;

    I_BuildGammaCurve();

    wt_sum = dither_r_wt + dither_g_wt + dither_b_wt;
    if (wt_sum < 1) wt_sum = 1;

    for (i = 0; i < 256; i++) {
        int r = saved_palette[i * 3 + 0];
        int g = saved_palette[i * 3 + 1];
        int b = saved_palette[i * 3 + 2];
        int gray = (r * dither_r_wt + g * dither_g_wt + b * dither_b_wt) / wt_sum;
        int lo = dither_gblack, hi = dither_gwhite;
        if (hi > lo)
            gray = gray < lo ? 0 : gray > hi ? 255 : (gray - lo) * 255 / (hi - lo);
        else
            gray = gray >= lo ? 255 : 0;  /* degenerate: hard threshold */
        grayscale_pal[i] = gamma_curve[gray];
    }
    doom_log("I_RebuildDitherPalette: gp[64]=%d gp[128]=%d gp[192]=%d\r",
             grayscale_pal[64], grayscale_pal[128], grayscale_pal[192]);
    I_BuildMonoColormaps();
}

/* Load doom_dither.cfg — key/value pairs, one per line.
 * Unknown keys are silently ignored for forward compatibility.             */
void I_LoadDitherConfig(void)
{
    FILE *f = fopen("doom_dither.cfg", "r");
    char key[32];
    float fval;
    int   ival;

    if (!f) {
        doom_log("I_LoadDitherConfig: no doom_dither.cfg, using defaults\r");
        return;
    }
    while (fscanf(f, "%31s", key) == 1) {
        if (strcmp(key, "gamma")       == 0 && fscanf(f, "%f",  &fval) == 1)
            dither_gamma  = (fval >= 0.05f && fval <= 3.0f) ? fval : dither_gamma;
        else if (strcmp(key, "r_weight")  == 0 && fscanf(f, "%d", &ival) == 1)
            dither_r_wt   = (ival >= 0 && ival <= 255) ? ival : dither_r_wt;
        else if (strcmp(key, "g_weight")  == 0 && fscanf(f, "%d", &ival) == 1)
            dither_g_wt   = (ival >= 0 && ival <= 255) ? ival : dither_g_wt;
        else if (strcmp(key, "b_weight")  == 0 && fscanf(f, "%d", &ival) == 1)
            dither_b_wt   = (ival >= 0 && ival <= 255) ? ival : dither_b_wt;
        else if (strcmp(key, "game_black") == 0 && fscanf(f, "%d", &ival) == 1)
            dither_gblack = (ival >= 0 && ival <= 255) ? ival : dither_gblack;
        else if (strcmp(key, "game_white") == 0 && fscanf(f, "%d", &ival) == 1)
            dither_gwhite = (ival >= 0 && ival <= 255) ? ival : dither_gwhite;
        else if (strcmp(key, "no_lighting") == 0 && fscanf(f, "%d", &ival) == 1)
            no_lighting   = ival ? 1 : 0;
        else if (strcmp(key, "fog_scale") == 0 && fscanf(f, "%d", &ival) == 1)
            fog_scale     = (ival >= 0 && ival <= 65536) ? ival : fog_scale;
        else {
            /* unknown key — skip the rest of the line */
            int c; while ((c = fgetc(f)) != '\n' && c != '\r' && c != EOF) {}
        }
    }
    fclose(f);
    /* Ensure minimum gap after loading — prevents div-by-zero */
    if (dither_gwhite < dither_gblack + 10) dither_gwhite = dither_gblack + 10;
    if (dither_gwhite > 255)                dither_gwhite = 255;
    doom_log("I_LoadDitherConfig: gamma=%.2f rw=%d gw=%d bw=%d gb=%d gw2=%d nl=%d\r",
             dither_gamma, dither_r_wt, dither_g_wt, dither_b_wt,
             dither_gblack, dither_gwhite, no_lighting);
}

/* Save current dither params to doom_dither.cfg.
 * Called by the 'K' hotkey at runtime.                                    */
void I_SaveDitherConfig(void)
{
    FILE *f = fopen("doom_dither.cfg", "w");
    if (!f) { doom_log("I_SaveDitherConfig: failed to open doom_dither.cfg\r"); return; }
    fprintf(f, "gamma %0.2f\r", dither_gamma);
    fprintf(f, "r_weight %d\r", dither_r_wt);
    fprintf(f, "g_weight %d\r", dither_g_wt);
    fprintf(f, "b_weight %d\r", dither_b_wt);
    fprintf(f, "game_black %d\r", dither_gblack);
    fprintf(f, "game_white %d\r", dither_gwhite);
    fprintf(f, "no_lighting %d\r", no_lighting);
    fprintf(f, "fog_scale %d\r", fog_scale);
    fclose(f);
    doom_log("I_SaveDitherConfig: saved (fog_scale=%d)\r", fog_scale);
}

/* Adjust a dither parameter at runtime (called from i_input_mac.c).
 * param: 0=gamma, 1=game_black, 2=game_white, 3=toggle no_lighting, 4=save
 * delta: +1 or -1 (ignored for toggle/save)                               */
void I_AdjustDither(int param, int delta)
{
    switch (param) {
        case 0: /* gamma: step 0.05 */
            dither_gamma += delta * 0.05f;
            if (dither_gamma < 0.05f) dither_gamma = 0.05f;
            if (dither_gamma > 3.0f)  dither_gamma = 3.0f;
            doom_log("dither: gamma=%.2f\r", dither_gamma);
            break;
        case 1: /* game_black: step 5, must stay at least 10 below game_white */
            dither_gblack += delta * 5;
            if (dither_gblack < 0)                    dither_gblack = 0;
            if (dither_gblack > dither_gwhite - 10)   dither_gblack = dither_gwhite - 10;
            doom_log("dither: game_black=%d\r", dither_gblack);
            break;
        case 2: /* game_white: step 5, must stay at least 10 above game_black */
            dither_gwhite += delta * 5;
            if (dither_gwhite > 255)                  dither_gwhite = 255;
            if (dither_gwhite < dither_gblack + 10)   dither_gwhite = dither_gblack + 10;
            doom_log("dither: game_white=%d\r", dither_gwhite);
            break;
        case 3: /* toggle no_lighting */
            no_lighting = !no_lighting;
            doom_log("dither: no_lighting=%d\r", no_lighting);
            return;  /* no palette rebuild needed */
        case 4: /* save config */
            I_SaveDitherConfig();
            return;
        case 5: /* fog_scale: step 2048, range 0-65536 (0=off) */
            fog_scale += delta * 2048;
            if (fog_scale < 0)      fog_scale = 0;
            if (fog_scale > 65536)  fog_scale = 65536;
            doom_log("fog: fog_scale=%d\r", fog_scale);
            return;  /* no palette rebuild needed */
        case 6: /* solidfloor_gray: cycle 0→1→2→3→4→0 */
            {
                extern int solidfloor_gray;
                solidfloor_gray = (solidfloor_gray + 1) % 5;
                doom_log("floor: solidfloor_gray=%d\r", solidfloor_gray);
            }
            return;  /* no palette rebuild needed */
    }
    I_RebuildDitherPalette();
}

void I_InitGraphics(void)
{
    BitMap *screen = &qd.screenBits;
    fb_mono_base     = (byte *)screen->baseAddr;
    fb_mono_rowbytes = screen->rowBytes;
    fb_mono_xoff     = (screen->bounds.right  - screen->bounds.left  - SCREENWIDTH)  / 2;
    fb_mono_yoff     = (screen->bounds.bottom - screen->bounds.top   - SCREENHEIGHT) / 2;

    {
        int fb_height = screen->bounds.bottom - screen->bounds.top;
        doom_log("I_InitGraphics: %dx%d, rowBytes=%d, xoff=%d, yoff=%d\r",
                 screen->bounds.right - screen->bounds.left,
                 fb_height, fb_mono_rowbytes, fb_mono_xoff, fb_mono_yoff);

        /* Allocate off-screen buffer for double-buffering.
         * All rendering (R_DrawColumn_Mono / R_DrawSpan_Mono / I_FinishUpdate blit)
         * writes here via fb_mono_base.  At the end of each I_FinishUpdate the
         * completed frame is memcpy'd to the real screen in one shot, so the user
         * never sees a partially-rendered frame. */
        fb_offscreen_buf = (unsigned char *)malloc((size_t)fb_mono_rowbytes * fb_height);
        if (!fb_offscreen_buf)
            I_Error("I_InitGraphics: failed to allocate off-screen buffer");
        memset(fb_offscreen_buf, 0, (size_t)fb_mono_rowbytes * fb_height);
        real_fb_base = fb_mono_base;     /* save real screen pointer */
        fb_mono_base = fb_offscreen_buf; /* redirect all rendering to off-screen */
        doom_log("I_InitGraphics: off-screen buffer %d bytes, real_fb=%p off_fb=%p\r",
                 fb_mono_rowbytes * fb_height, real_fb_base, (void *)fb_mono_base);
    }

    /* Load doom_dither.cfg if present, then build gamma curve.
     * I_SetPalette may have been called earlier in startup (before this
     * function) with gamma_curve still all-zero.  I_RebuildDitherPalette()
     * re-bakes grayscale_pal with the correct curve if palette_valid is set. */
    I_LoadDitherConfig();
    I_BuildGammaCurve();
    I_RebuildDitherPalette();

    /* Hide the Mac OS software cursor.
     *
     * System 7's cursor VBL task runs at 60 Hz: it saves the pixels under
     * the cursor, draws the cursor bitmap, and on the next tick restores the
     * saved pixels.  Because we write directly to the framebuffer (bypassing
     * QuickDraw), the "saved pixels" can be stale — when the VBL task
     * restores them it overwrites our freshly-rendered content for one scan
     * cycle, causing the aperiodic "candle flicker" on sprites and overlays.
     * HideCursor() increments the OS hide-level so the VBL task stops
     * touching the framebuffer entirely.  ShowCursor() in I_ShutdownGraphics
     * decrements it back when we exit. */
    HideCursor();

    /* Allocate Doom's screen buffer: 320x200 8-bit */
    screens[0] = (byte *)malloc(SCREENWIDTH * SCREENHEIGHT);
    if (!screens[0])
        I_Error("I_InitGraphics: failed to allocate screen buffer");
    memset(screens[0], 0, SCREENWIDTH * SCREENHEIGHT);

    /* Allocate a dedicated menu overlay buffer.
     * NOTE: screens[1] is Doom's border tile cache (R_FillBackScreen writes
     * the border pattern there; R_DrawViewBorder copies from it to screens[0]).
     * We must NOT touch screens[1].  Use a separate allocation instead. */
    menu_overlay_buf = (byte *)malloc(SCREENWIDTH * SCREENHEIGHT);
    if (!menu_overlay_buf)
        I_Error("I_InitGraphics: failed to allocate overlay buffer");
    memset(menu_overlay_buf, 0, SCREENWIDTH * SCREENHEIGHT);
}

void I_ShutdownGraphics(void)
{
    ShowCursor();
}

/* Count I_SetPalette calls skipped because palette was unchanged.
 * Reset every 35 game tics by d_main.c.  Reported in FPS log line. */
long prof_palette_skips = 0;

/*
 * I_SetPalette — Doom calls this when the palette changes.
 * We convert the RGB palette to grayscale values for our 1-bit output.
 * palette is 768 bytes: 256 entries of (R, G, B).
 *
 * ST_doPaletteStuff calls this every game tic even when palette index 0
 * (default palette) is active.  The fast-path memcmp avoids rebuilding
 * grayscale_pal and setting mono_dirty when nothing has changed, which
 * was burning ~2 Mac ticks/window in the profiling blit bucket.
 */
void I_SetPalette(byte *palette)
{
    int i;
    int wt_sum;

    /* Fast path: identical palette → skip all computation, don't mark dirty. */
    if (palette_valid && memcmp(palette, saved_palette, 768) == 0)
    {
        prof_palette_skips++;
        return;
    }

    wt_sum = dither_r_wt + dither_g_wt + dither_b_wt;
    if (wt_sum < 1) wt_sum = 1;

    /* Save palette for runtime rebuilds (hotkey param changes). */
    for (i = 0; i < 768; i++) saved_palette[i] = palette[i];
    palette_valid = 1;

    for (i = 0; i < 256; i++) {
        int r = palette[i * 3 + 0];
        int g = palette[i * 3 + 1];
        int b = palette[i * 3 + 2];
        int gray = (r * dither_r_wt + g * dither_g_wt + b * dither_b_wt) / wt_sum;
        /* Game contrast stretch */
        if (dither_gwhite > dither_gblack)
            gray = gray < dither_gblack ? 0
                 : gray > dither_gwhite ? 255
                 : (gray - dither_gblack) * 255 / (dither_gwhite - dither_gblack);
        else
            gray = gray >= dither_gblack ? 255 : 0;
        grayscale_pal[i] = gamma_curve[gray];
        if (i == 128)
            doom_log("I_SetPalette: gc[gray=%d]=%d -> gp[128]=%d (dg=%.2f)\r",
                     gray, gamma_curve[gray], grayscale_pal[128], dither_gamma);
        raw_gray[i] = (byte)gray;
        {
            int sb = r < SBAR_BLACK ? 0
                   : r > SBAR_WHITE ? 255
                   : (r - SBAR_BLACK) * 255 / (SBAR_WHITE - SBAR_BLACK);
            sbar_gray[i] = (byte)sb;
        }
    }
    /* Mark mono_colormaps stale — rebuilt once per frame in I_FinishUpdate,
     * not here, so that rapid palette flashes (damage, pickups) only pay
     * the 8KB rebuild cost once regardless of how many I_SetPalette calls
     * arrive in a single frame. */
    mono_dirty = 1;
}

/*
 * Blit 8 pixels from src (palette indices) to one byte at dst (1-bit).
 * bayer_r is the Bayer row for this screen y (4-element, indexed by x&3).
 * x is the screen-x of the leftmost of the 8 pixels (for Bayer column index).
 */
static inline unsigned char blit8(const byte *src, const byte *bayer_r, int x)
{
    unsigned char out = 0;
    if (grayscale_pal[src[0]] < bayer_r[(x+0) & 3]) out |= 0x80;
    if (grayscale_pal[src[1]] < bayer_r[(x+1) & 3]) out |= 0x40;
    if (grayscale_pal[src[2]] < bayer_r[(x+2) & 3]) out |= 0x20;
    if (grayscale_pal[src[3]] < bayer_r[(x+3) & 3]) out |= 0x10;
    if (grayscale_pal[src[4]] < bayer_r[(x+4) & 3]) out |= 0x08;
    if (grayscale_pal[src[5]] < bayer_r[(x+5) & 3]) out |= 0x04;
    if (grayscale_pal[src[6]] < bayer_r[(x+6) & 3]) out |= 0x02;
    if (grayscale_pal[src[7]] < bayer_r[(x+7) & 3]) out |= 0x01;
    return out;
}

/*
 * blit8_sbar_thresh — hard-threshold blit for the status bar using
 * contrast-stretched grayscale (sbar_gray[]).
 *
 * sbar_gray[] maps palette indices through raw BT.601 luminance and then
 * linearly stretches the [SBAR_BLACK..SBAR_WHITE] band to [0..255].
 * A threshold at 128 then cleanly separates:
 *   - dark background + digit shadows  (raw ~70-105) → sbar < 128 → BLACK
 *   - digit highlights + weapon nums   (raw ~120-160) → sbar > 128 → WHITE
 *   - baked STBAR label text           (raw ~170+)    → sbar = 255  → WHITE
 *
 * Tune SBAR_BLACK / SBAR_WHITE if contrast is off on real hardware.
 */
#define SBARHEIGHT 32  /* matches r_draw.c */
static inline unsigned char blit8_sbar_thresh(const byte *src)
{
    unsigned char out = 0;
    if (sbar_gray[src[0]] < 128) out |= 0x80;
    if (sbar_gray[src[1]] < 128) out |= 0x40;
    if (sbar_gray[src[2]] < 128) out |= 0x20;
    if (sbar_gray[src[3]] < 128) out |= 0x10;
    if (sbar_gray[src[4]] < 128) out |= 0x08;
    if (sbar_gray[src[5]] < 128) out |= 0x04;
    if (sbar_gray[src[6]] < 128) out |= 0x02;
    if (sbar_gray[src[7]] < 128) out |= 0x01;
    return out;
}

/*
 * blit8_black — for HUD/menu overlay over direct-rendered view area.
 * Any non-zero pixel in screens[0] maps to black (set bit) in the 1-bit fb.
 * This gives maximum contrast: overlay text/elements always appear solid black
 * regardless of what the 1-bit renderer put underneath.
 * Returns 0 for an all-zero chunk (common case), so OR-ing into fb is safe.
 */
static inline unsigned char blit8_black(const byte *src)
{
    unsigned char out = 0;
    if (src[0]) out |= 0x80;
    if (src[1]) out |= 0x40;
    if (src[2]) out |= 0x20;
    if (src[3]) out |= 0x10;
    if (src[4]) out |= 0x08;
    if (src[5]) out |= 0x04;
    if (src[6]) out |= 0x02;
    if (src[7]) out |= 0x01;
    return out;
}

/*
 * I_FinishUpdate — blit Doom's 320x200 8-bit buffer to the 512x342 1-bit screen.
 *
 * During gameplay (GS_LEVEL, not automap):
 *   - The view area was rendered directly to 1-bit by R_DrawColumn_Mono /
 *     R_DrawSpan_Mono.  screens[0]'s view area was pre-cleared to 0 by
 *     R_RenderPlayerView so we can detect HUD/menu overlays by non-zero check.
 *   - We blit the non-view area (status bar, border) from screens[0].
 *   - We overlay non-zero view pixels (HUD text, active menu) from screens[0].
 *
 * During menus, wipes, intermission:
 *   - Full blit of screens[0] as before.
 *
 * fb_mono_xoff (96) and viewwindowx (48) are multiples of 8, so all
 * boundaries fall on byte boundaries in the framebuffer.
 */
void I_FinishUpdate(void)
{
    /* Rebuild mono_colormaps at most once per frame. */
    if (mono_dirty) { I_BuildMonoColormaps(); mono_dirty = 0; }

    byte *src = screens[0];
    int   xoff = fb_mono_xoff;
    int   yoff = fb_mono_yoff;
    int   y, x;

    /* Direct mode: GS_LEVEL gameplay with no menu/wipe/automap.
     * Walls/floors/ceilings already rendered to 1-bit fb by R_DrawColumn_Mono
     * and R_DrawSpan_Mono.  We only need to blit the status bar and border,
     * and optionally overlay HU messages over the view area. */
    boolean is_direct = (gamestate == GS_LEVEL && !automapactive && gametic > 0
                         && fb_mono_base != NULL && !wipe_in_progress && !menuactive);

    /* Track direct/non-direct transitions for border cache logic. */
    static boolean last_direct = false;

    /* Instrument: log every is_direct state change */
    if (is_direct != last_direct) {
        doom_log("IFU: is_direct %d->%d gs=%d ma=%d wipe=%d fb=%p\r",
                 (int)last_direct, (int)is_direct,
                 (int)gamestate, (int)menuactive,
                 (int)wipe_in_progress, fb_mono_base);
    }

    if (is_direct) {
        /* Cached view geometry (constant while screenblocks unchanged). */
        const int vx0   = viewwindowx;              /* 48  — 8-aligned */
        const int vx1   = viewwindowx + scaledviewwidth; /* 272 — 8-aligned */
        const int vy0   = viewwindowy;              /* 28  */
        const int vy1   = viewwindowy + viewheight; /* 140 */
        const int sbar0 = SCREENHEIGHT - SBARHEIGHT;/* 168 */

        /* Border cache: skip border re-blit unless screens[0] border content
         * has changed (R_DrawViewBorder ran) or we're transitioning from a
         * non-direct frame (wipe/menu may have written over the fb border). */
        boolean do_border = (border_needs_blit || !last_direct);
        if (do_border) border_needs_blit = 0;

        /* --- Region 1: rows above the view window (top border) --- */
        if (do_border) {
            for (y = 0; y < vy0; y++) {
                const byte    *sr  = src + y * SCREENWIDTH;
                unsigned char *dst = (unsigned char *)(fb_mono_base
                                     + (y + yoff) * fb_mono_rowbytes) + (xoff >> 3);
                const byte    *br  = bayer4x4[y & 3];
                for (x = 0; x < SCREENWIDTH; x += 8) *dst++ = blit8(sr + x, br, x);
            }
        }

        /* --- Region 2: view rows (border strips + view area) --- */
        for (y = vy0; y < vy1; y++) {
            const byte    *sr  = src + y * SCREENWIDTH;
            unsigned char *dst = (unsigned char *)(fb_mono_base
                                 + (y + yoff) * fb_mono_rowbytes) + (xoff >> 3);
            const byte    *br  = bayer4x4[y & 3];

            /* Left border strip */
            if (do_border) {
                for (x = 0; x < vx0; x += 8) *dst++ = blit8(sr + x, br, x);
            } else {
                dst += vx0 >> 3;
            }

            /* View area: only scan for HU overlay when a message is active.
             * screens[0] view area is pre-cleared to 0 by R_RenderPlayerView;
             * non-zero bytes mean HU text was drawn there.  When inactive,
             * the direct-rendered 1-bit content stands as-is. */
            if (hu_overlay_active) {
                for (x = vx0; x < vx1; x += 8) {
                    *dst &= ~blit8_black(sr + x);
                    dst++;
                }
            } else {
                dst += (vx1 - vx0) >> 3;
            }

            /* Right border strip */
            if (do_border) {
                for (x = vx1; x < SCREENWIDTH; x += 8) *dst++ = blit8(sr + x, br, x);
            } else {
                dst += (SCREENWIDTH - vx1) >> 3;
            }
        }

        /* --- Region 3: rows between view and status bar (bottom border) --- */
        if (do_border && vy1 < sbar0) {
            for (y = vy1; y < sbar0; y++) {
                const byte    *sr  = src + y * SCREENWIDTH;
                unsigned char *dst = (unsigned char *)(fb_mono_base
                                     + (y + yoff) * fb_mono_rowbytes) + (xoff >> 3);
                const byte    *br  = bayer4x4[y & 3];
                for (x = 0; x < SCREENWIDTH; x += 8) *dst++ = blit8(sr + x, br, x);
            }
        }

        /* --- Region 4: status bar rows (always blitted — content changes) --- */
        for (y = sbar0; y < SCREENHEIGHT; y++) {
            const byte    *sr  = src + y * SCREENWIDTH;
            unsigned char *dst = (unsigned char *)(fb_mono_base
                                 + (y + yoff) * fb_mono_rowbytes) + (xoff >> 3);
            for (x = 0; x < SCREENWIDTH; x += 8) *dst++ = blit8_sbar_thresh(sr + x);
        }

    } else {
        /* Non-direct (menu, wipe, intermission): full blit.
         * last_direct=false ensures next direct frame triggers do_border=true,
         * re-blitting the border after wipe/menu may have written over it. */
        const int sbar0 = SCREENHEIGHT - SBARHEIGHT;
        for (y = 0; y < SCREENHEIGHT; y++) {
            const byte    *sr  = src + y * SCREENWIDTH;
            unsigned char *dst = (unsigned char *)(fb_mono_base
                                 + (y + yoff) * fb_mono_rowbytes) + (xoff >> 3);
            if (y >= sbar0) {
                for (x = 0; x < SCREENWIDTH; x += 8)
                    *dst++ = blit8_sbar_thresh(sr + x);
            } else {
                const byte *br = bayer4x4[y & 3];
                for (x = 0; x < SCREENWIDTH; x += 8)
                    *dst++ = blit8(sr + x, br, x);
            }
        }
    }

    last_direct = is_direct;

    /* Menu overlay: solid-white mask applied over the full screen.
     * Only runs when the menu is open (menuactive implies !is_direct). */
    {   static boolean prev_ma_overlay = false;
        if (menuactive != prev_ma_overlay)
            doom_log("IFU: menu_overlay path %s (is_direct=%d)\r",
                     menuactive ? "ENTER" : "EXIT", (int)is_direct);
        prev_ma_overlay = menuactive; }
    if (menuactive && menu_overlay_buf) {
        byte *overlay = menu_overlay_buf;
        for (y = 0; y < SCREENHEIGHT; y++) {
            const byte    *ovr = overlay + y * SCREENWIDTH;
            unsigned char *dst = (unsigned char *)(fb_mono_base
                                 + (y + yoff) * fb_mono_rowbytes) + (xoff >> 3);
            for (x = 0; x < SCREENWIDTH; x += 8) { *dst &= ~blit8_black(ovr + x); dst++; }
        }
    }

    /* PROBE B: read back same bytes as PROBE A, after I_FinishUpdate processing.
     * If different from PROBE A, I_FinishUpdate is clearing the view area. */
    {
        extern int detailshift;
        static int probe_b_logged = 0;
        /* Guard: only fire during actual direct-mode gameplay frame.
         * Read same positions as PROBE_A to compare before/after IFU processing. */
        if (!probe_b_logged && detailshift == 2 && fb_mono_base && is_direct) {
            int cx0   = (viewwindowx + xoff) >> 3;
            int cx14  = ((14<<2) + viewwindowx + xoff) >> 3;
            int wall_y  = viewwindowy + viewheight/2 + yoff;
            int floor_y = viewwindowy + viewheight - 2 + yoff;
            unsigned char b0w  = ((unsigned char*)fb_mono_base)[wall_y  * fb_mono_rowbytes + cx0];
            unsigned char b14w = ((unsigned char*)fb_mono_base)[wall_y  * fb_mono_rowbytes + cx14];
            unsigned char b0f  = ((unsigned char*)fb_mono_base)[floor_y * fb_mono_rowbytes + cx0];
            doom_log("PROBE_B: cx0_wall=0x%02X cx14_wall=0x%02X cx0_floor=0x%02X (wy=%d fy=%d cx0=%d cx14=%d)\r",
                     b0w, b14w, b0f, wall_y, floor_y, cx0, cx14);
            probe_b_logged = 1;
        }
    }

    /* Double-buffer flip: copy the completed off-screen frame to the real screen.
     * Only copies the SCREENHEIGHT rows that Doom uses (rows yoff..yoff+199).
     * 200 × 64 = 12,800 bytes — ~1.6ms at 16MHz, fully worth the flicker-free result.
     * The user never sees a partially-rendered frame. */
    if (real_fb_base) {
        memcpy((unsigned char *)real_fb_base + yoff * fb_mono_rowbytes,
               (unsigned char *)fb_mono_base + yoff * fb_mono_rowbytes,
               (size_t)SCREENHEIGHT * fb_mono_rowbytes);
    }
}

void I_UpdateNoBlit(void)
{
    /* Empty — Doom calls this but we do all blitting in I_FinishUpdate */
}

void I_WaitVBL(int count)
{
    /* Wait for count/70 seconds (original was VGA vblank at 70Hz) */
    /* Use Mac TickCount (60Hz). Approximate. */
    long target = TickCount() + (count * 60 / 70);
    while (TickCount() < target)
        ;
}

void I_ReadScreen(byte *scr)
{
    memcpy(scr, screens[0], SCREENWIDTH * SCREENHEIGHT);
}

void I_BeginRead(void)
{
    /* Disk icon — not needed */
}

void I_EndRead(void)
{
    /* Disk icon — not needed */
}
