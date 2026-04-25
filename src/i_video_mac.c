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
#include <Gestalt.h>

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

/* Color display support — depth detected in i_main_mac.c before window creation.
 * g_color_depth = 1: 1-bit mono (SE/30, Classic, Plus).
 * g_color_depth = 8: 8-bit color or grayscale (IIci, Quadra, LC 520, etc.).  */
int  g_color_depth     = 1;
static byte *fb_color_base     = NULL;
static int   fb_color_rowbytes = 0;
static int   fb_color_xoff     = 0;
static int   fb_color_yoff     = 0;

/* CopyBits path for NuBus framebuffers (non-SE/30).
 * On IIci/IIcx/Quadra the FB is at 0xf9xxxxxx (NuBus).  In 24-bit addressing
 * mode the 68030 strips bits 24-31, so direct writes land in system heap → crash.
 * CopyBits routes through the ROM slot manager → safe in any addressing mode.
 * SE/30 FB is in main RAM (low address) → direct writes always safe. */
static int    s_use_copybits = 0;
static PixMap s_src_pm;        /* color: PixMap wrapping screens[0] for CopyBits */
static BitMap s_offscreen_bm;  /* mono:  BitMap wrapping fb_offscreen_buf for CopyBits */
static int    s_color_screen_w = 0;  /* physical color screen dimensions, set at init */
static int    s_color_screen_h = 0;

extern WindowPtr bg_window;    /* fullscreen black window, created in i_main_mac.c */

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

/* 4x4 Bayer ordered dither threshold matrix — high-contrast variant.
 * Threshold range compressed to [48..208]: gray < 48 → always black,
 * gray > 208 → always white.  Pushes more area to solid black/white,
 * giving crisper contrast on the SE/30's 1-bit display.
 * Non-static so r_draw.c can use the same matrix. */
const byte bayer4x4[4][4] = {
    { 48, 133,  69, 154 },
    { 176,  90, 197, 112 },
    {  80, 165,  58, 144 },
    { 208, 122, 186, 101 }
};

/* Dedicated menu overlay buffer — M_Drawer renders here (not into screens[1],
 * which Doom uses as its border tile cache). Non-static so d_main.c can use it
 * to redirect M_Drawer output. */
byte *menu_overlay_buf = NULL;

/* Menu background cache: 1-bit snapshot of the framebuffer saved on the first
 * menu frame.  Subsequent menu frames restore this via memcpy instead of
 * re-running 8000 blit8_sbar_thresh calls against screens[0]. */
static unsigned char menu_bg_1bit[SCREENHEIGHT * (SCREENWIDTH >> 3)]; /* 200×40 = 8000 bytes */
static boolean       menu_bg_valid = false;

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

/* ---- 2× pixel-scale mode ------------------------------------------------
 * When opt_scale2x is set, the game view is rendered at half resolution into
 * fb_source_buf (compact: scaledviewwidth/8 bytes/row), then expand2x_blit
 * expands it 2× horizontally and vertically into fb_offscreen_buf.
 *
 * fb_source_buf max size: blocks=8 QUAD, viewheight=128, 32 bytes/row → 4096 B.
 * expand2x_lut[b]: uint16_t where each bit of b is doubled (big-endian order).
 *   e.g. 0b10000000 → 0b1100000000000000 = 0xC000.
 *
 * Physical screen constants (set once in I_InitGraphics, constant thereafter):
 *   s_phys_rbytes    = 64   (512 px / 8)
 *   s_phys_height    = 342
 *   s_phys_xoff_byte = 12   (= (512-320)/2/8, byte column of game area left edge)
 *   s_phys_yoff      = 71   (= (342-200)/2,   row of game area top edge)
 * -------------------------------------------------------------------------*/
extern int opt_scale2x;  /* defined in d_main.c */
extern int opt_directfb; /* defined in d_main.c */

/* Output framebuffer for non-direct paths (menu/wipe/title).
 * In directfb mode there's no fb_offscreen_buf — write to fb_mono_base (the screen). */
#define FB_OUT (fb_offscreen_buf ? fb_offscreen_buf : (unsigned char *)fb_mono_base)

static unsigned char  fb_source_buf[128 * 32]; /* 4096 B: compact view render target */
static unsigned short expand2x_lut[256];        /* bit-doubling LUT, big-endian       */

static int s_phys_rbytes    = 0; /* physical screen rowbytes           */
static int s_phys_height    = 0; /* physical screen height in rows     */
static int s_phys_xoff_byte = 0; /* byte col of 320px game area start  */
static int s_phys_yoff      = 0; /* row of 200px game area start       */

/* Build expand2x_lut: each byte value → uint16_t with every bit doubled.
 * Big-endian: bit 7 (leftmost pixel) → bits 15,14 of result; bit 0 → bits 1,0. */
static void I_BuildExpand2xLUT(void)
{
    int b, i;
    for (b = 0; b < 256; b++) {
        unsigned short v = 0;
        for (i = 0; i < 8; i++)
            if ((b >> (7 - i)) & 1)
                v |= (unsigned short)3 << (14 - i * 2);
        expand2x_lut[b] = v;
    }
}

/* Expand 1-bit source (src_rows × src_rbytes) to 2× on dest.
 * Each source bit → 2 dest bits. Each source row → 2 consecutive dest rows.
 * dest_x0: byte column in dest where the expanded output starts. */
static void expand2x_blit(const unsigned char *src, unsigned char *dest,
                           int src_rows, int src_rbytes,
                           int dest_x0, int dest_y0, int dest_rbytes)
{
    int r, c;
    for (r = 0; r < src_rows; r++) {
        const unsigned char *sp  = src  + r * src_rbytes;
        unsigned char       *dp0 = dest + (dest_y0 + r * 2)     * dest_rbytes + dest_x0;
        unsigned char       *dp1 = dest + (dest_y0 + r * 2 + 1) * dest_rbytes + dest_x0;
        for (c = 0; c < src_rbytes; c++) {
            unsigned short ex = expand2x_lut[sp[c]];
            unsigned char  hi = (unsigned char)(ex >> 8);
            unsigned char  lo = (unsigned char)ex;
            dp0[c * 2]     = hi;  dp0[c * 2 + 1] = lo;
            dp1[c * 2]     = hi;  dp1[c * 2 + 1] = lo;
        }
    }
}

/* View geometry — from r_draw.c/r_main.c, used for selective blit */
extern int viewwindowx;
extern int viewwindowy;
extern int viewheight;
extern int scaledviewwidth;

/* ---- Runtime-tunable dither parameters ---------------------------------- */
/* All are non-static so I_AdjustDither (called from i_input_mac.c) can
 * modify them, and so they can be saved/loaded via doom_dither.cfg.       */

int   dither_gamma_x100 = 52; /* gamma exponent ×100: 52 = 0.52, range 5-300  */
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
int menu_thresh = 88;   /* luminance threshold for menu overlay (0-255) */

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
    int gam100 = dither_gamma_x100;

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

    doom_log("I_BuildGammaCurve: dg_x100=%d gc[64]=%d gc[128]=%d gc[192]=%d\r",
             dither_gamma_x100, gamma_curve[64], gamma_curve[128], gamma_curve[192]);
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

/* dither params are now loaded/saved via doom.cfg (M_LoadDefaults/M_SaveDefaults).
 * These stubs are kept so any lingering call sites link cleanly.           */
void I_LoadDitherConfig(void) {}
void I_SaveDitherConfig(void) {}

/* Adjust a dither parameter at runtime (called from i_input_mac.c).
 * param: 0=gamma, 1=game_black, 2=game_white, 3=toggle no_lighting, 4=save
 * delta: +1 or -1 (ignored for toggle/save)                               */
void I_AdjustDither(int param, int delta)
{
    switch (param) {
        case 0: /* gamma: step 5 (= 0.05 in ×100 units), range 5-300 */
            dither_gamma_x100 += delta * 5;
            if (dither_gamma_x100 < 5)   dither_gamma_x100 = 5;
            if (dither_gamma_x100 > 300) dither_gamma_x100 = 300;
            doom_log("dither: gamma_x100=%d\r", dither_gamma_x100);
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
        case 4: /* save config — now consolidated into doom.cfg */
            { extern void M_SaveDefaults(void); M_SaveDefaults(); }
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

/* Log 68030 CACR (Cache Control Register) state at startup.
 * MOVEC is privileged but Classic Mac OS runs in supervisor mode. */
static void I_LogCacheState(void)
{
    unsigned long cacr_val = 0;
    __asm__ __volatile__("movec %%cacr,%0" : "=d"(cacr_val));
    doom_log("CACR: 0x%08lX icache=%s dcache=%s dburst=%s walloc=%s\r",
             cacr_val,
             (cacr_val & 0x0001) ? "ON" : "OFF",   /* bit 0:  I-cache enable */
             (cacr_val & 0x0100) ? "ON" : "OFF",   /* bit 8:  D-cache enable */
             (cacr_val & 0x1000) ? "ON" : "OFF",   /* bit 12: D-cache burst  */
             (cacr_val & 0x2000) ? "ON" : "OFF");  /* bit 13: write allocate */
}

void I_InitGraphics(void)
{
    I_LogCacheState();

    if (g_color_depth >= 8) {
        /* Color/grayscale display path.
         * Get the hardware framebuffer from GetMainDevice() → gdPMap instead
         * of qd.screenBits, which is always a 1-bit bitmap.  SetEntries will
         * program this device's CLUT; I_FinishUpdate blits screens[0] here. */
        GDHandle     gd  = GetMainDevice();
        PixMapHandle pm  = (*gd)->gdPMap;
        int screen_w = (*pm)->bounds.right  - (*pm)->bounds.left;
        int screen_h = (*pm)->bounds.bottom - (*pm)->bounds.top;
        fb_color_base      = (byte *)(*pm)->baseAddr;
        fb_color_rowbytes  = (*pm)->rowBytes & 0x3FFF; /* high bits are flags */
        fb_color_xoff      = (screen_w - SCREENWIDTH)  / 2;
        fb_color_yoff      = (screen_h - SCREENHEIGHT) / 2;
        s_color_screen_w   = screen_w;
        s_color_screen_h   = screen_h;
        doom_log("I_InitGraphics: color %dx%d rb=%d xoff=%d yoff=%d depth=%d\r",
                 screen_w, screen_h, fb_color_rowbytes,
                 fb_color_xoff, fb_color_yoff, g_color_depth);
        /* Heap boundary check: is fb_color_base inside the app heap?
         * ApplZone() = heap start, GetApplLimit() = heap end.
         * If fb_color_base falls in [ApplZone, GetApplLimit), we're writing
         * into heap memory every frame — root cause of corruption on exit. */
        /* Low-memory globals: ApplZone=0x02AA (heap start), ApplLimit=0x0130 (heap end) */
        doom_log("I_InitGraphics: fb_color_base=%p ApplZone=%p ApplLimit=%p\r",
                 (void *)fb_color_base,
                 (void *)(*(long *)0x02AA),
                 (void *)(*(long *)0x0130));
        HideCursor();
        screens[0] = (byte *)malloc(SCREENWIDTH * SCREENHEIGHT);
        if (!screens[0])
            I_Error("I_InitGraphics: failed to allocate screen buffer");
        memset(screens[0], 0, SCREENWIDTH * SCREENHEIGHT);
        menu_overlay_buf = (byte *)malloc(SCREENWIDTH * SCREENHEIGHT);
        if (!menu_overlay_buf)
            I_Error("I_InitGraphics: failed to allocate overlay buffer");
        memset(menu_overlay_buf, 0, SCREENWIDTH * SCREENHEIGHT);

        /* Detect NuBus FB: non-SE/30 → use CopyBits instead of direct writes. */
        {
            long machine_type = 0;
            Gestalt(gestaltMachineType, &machine_type);
            s_use_copybits = (machine_type != kMacModelSE30);
            doom_log("I_InitGraphics: machine=%ld s_use_copybits=%d\r",
                     machine_type, s_use_copybits);
        }
        /* Source PixMap wrapping screens[0].  pmTable points to the device's
         * own CLUT so ctSeeds always match after SetEntries → CopyBits does a
         * direct index copy with no color-matching overhead. */
        s_src_pm.baseAddr   = (Ptr)screens[0];
        s_src_pm.rowBytes   = SCREENWIDTH | 0x8000; /* high bit = PixMap flag */
        s_src_pm.bounds.top    = 0;
        s_src_pm.bounds.left   = 0;
        s_src_pm.bounds.bottom = SCREENHEIGHT;
        s_src_pm.bounds.right  = SCREENWIDTH;
        s_src_pm.pmVersion  = 0;
        s_src_pm.packType   = 0;
        s_src_pm.packSize   = 0;
        s_src_pm.hRes       = 0x00480000; /* 72 dpi */
        s_src_pm.vRes       = 0x00480000;
        s_src_pm.pixelType  = 0; /* chunky */
        s_src_pm.pixelSize  = 8;
        s_src_pm.cmpCount   = 1;
        s_src_pm.cmpSize    = 8;
        s_src_pm.planeBytes = 0;
        s_src_pm.pmReserved = 0;
        s_src_pm.pmTable    = (*(*GetMainDevice())->gdPMap)->pmTable;

        /* No off-screen 1-bit buffer, no mono_colormaps, no Bayer dither.
         * fb_mono_base stays NULL → is_direct stays false → renderers always
         * write to screens[0] (via 8-bit colfunc), which we then blit out. */
        /* Bring bg_window to front so CopyBits visRgn isn't clipped by console */
        if (s_use_copybits && bg_window)
            SelectWindow(bg_window);
        return;
    }

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

        /* Save physical screen layout constants — used by I_FinishUpdate for
         * both normal and 2x mode (non-direct blit, flip, menu overlay). */
        s_phys_rbytes    = fb_mono_rowbytes;                              /* 64 */
        s_phys_height    = fb_height;                                     /* 342 */
        s_phys_xoff_byte = fb_mono_xoff / 8;                             /* 12 */
        s_phys_yoff      = fb_mono_yoff;                                  /* 71 */

        if (opt_directfb && !opt_scale2x) {
            /* directfb: render straight to screen, no double-buffer.
             * fb_mono_base stays pointing at real screen memory.
             * real_fb_base stays NULL so the flip in I_FinishUpdate is skipped. */
            real_fb_base     = NULL;
            fb_offscreen_buf = NULL;
            doom_log("I_InitGraphics: directfb mode, no double-buffer\r");
        } else {
            /* Allocate off-screen buffer for double-buffering.
             * All rendering writes here via fb_mono_base.  I_FinishUpdate
             * copies the completed frame to real_fb_base in one shot. */
            fb_offscreen_buf = (unsigned char *)malloc((size_t)fb_mono_rowbytes * fb_height);
            if (!fb_offscreen_buf)
                I_Error("I_InitGraphics: failed to allocate off-screen buffer");
            memset(fb_offscreen_buf, 0, (size_t)fb_mono_rowbytes * fb_height);
            real_fb_base = fb_mono_base;     /* save real screen pointer */
            fb_mono_base = fb_offscreen_buf; /* redirect all rendering to off-screen */
            doom_log("I_InitGraphics: off-screen buffer %d bytes, real_fb=%p off_fb=%p\r",
                     fb_mono_rowbytes * fb_height, real_fb_base, (void *)fb_mono_base);

            /* Detect NuBus FB for CopyBits flip. */
            {
                long machine_type = 0;
                Gestalt(gestaltMachineType, &machine_type);
                s_use_copybits = (machine_type != kMacModelSE30);
                doom_log("I_InitGraphics: machine=%ld s_use_copybits=%d\r",
                         machine_type, s_use_copybits);
            }
            /* BitMap wrapping full off-screen buffer for CopyBits flip. */
            s_offscreen_bm.baseAddr = (Ptr)fb_offscreen_buf;
            s_offscreen_bm.rowBytes = s_phys_rbytes;
            s_offscreen_bm.bounds.top    = 0;
            s_offscreen_bm.bounds.left   = 0;
            s_offscreen_bm.bounds.bottom = fb_height;
            s_offscreen_bm.bounds.right  = s_phys_rbytes * 8;
        }

        /* 2x pixel-scale mode: further redirect fb_mono_base to the compact
         * source buffer.  R_ExecuteSetViewSize later sets fb_mono_rowbytes to
         * scaledviewwidth/8 and forces viewwindowx=viewwindowy=0. */
        if (opt_scale2x) {
            I_BuildExpand2xLUT();
            memset(fb_source_buf, 0, sizeof(fb_source_buf));
            fb_mono_base     = fb_source_buf; /* renderers → compact source buf */
            fb_mono_xoff     = 0;
            fb_mono_yoff     = 0;
            fb_mono_rowbytes = 32;  /* default for blocks=8; updated in R_ExecuteSetViewSize */
            doom_log("I_InitGraphics: 2x mode, src buf=%d B, expand2x_lut built\r",
                     (int)sizeof(fb_source_buf));
        }
    }

    /* Build gamma curve from dither_gamma_x100 (loaded by M_LoadDefaults earlier).
     * I_SetPalette may have been called before this with gamma_curve all-zero;
     * I_RebuildDitherPalette() re-bakes grayscale_pal if palette_valid is set. */
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

    /* Bring bg_window to front so CopyBits visRgn isn't clipped by Retro68 console */
    if (s_use_copybits && bg_window)
        SelectWindow(bg_window);

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

    /* Cleanup window is owned by main() — nothing to do here */
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

    if (g_color_depth >= 8) {
        /* Color path: program hardware CLUT so palette index N displays as
         * Doom's actual RGB color N.  SetEntries writes directly to the video
         * card — no per-frame cost once the CLUT is loaded.
         * RGBColor channels are 16-bit (0-65535); multiply 8-bit values by 257
         * (= 0x101) to map 0→0 and 255→65535 exactly. */
        static ColorSpec cs[256];
        for (i = 0; i < 256; i++) {
            cs[i].value        = (INTEGER)i;
            cs[i].rgb.red      = (unsigned short)palette[i*3+0] * 257u;
            cs[i].rgb.green    = (unsigned short)palette[i*3+1] * 257u;
            cs[i].rgb.blue     = (unsigned short)palette[i*3+2] * 257u;
        }
        SetEntries(0, 255, cs);
        return;
    }

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
            doom_log("I_SetPalette: gc[gray=%d]=%d -> gp[128]=%d (dg_x100=%d)\r",
                     gray, gamma_curve[gray], grayscale_pal[128], dither_gamma_x100);
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

/* blit8_menu — luminance-threshold menu overlay.
 * Pixel → black only when index != 0 AND grayscale_pal[index] > menu_thresh.
 * Transparent (index 0) and dark background pixels below threshold → white.
 * menu_thresh runtime-tunable via I_AdjustDither(7, ±1). */
static inline unsigned char blit8_menu(const byte *src)
{
    unsigned char out = 0;
#define MK(n,bit) if (src[n] && grayscale_pal[src[n]] > menu_thresh) out |= (bit)
    MK(0, 0x80); MK(1, 0x40); MK(2, 0x20); MK(3, 0x10);
    MK(4, 0x08); MK(5, 0x04); MK(6, 0x02); MK(7, 0x01);
#undef MK
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
    if (g_color_depth >= 8) {
        /* Color/grayscale fast path: the 8-bit renderers already wrote correct
         * palette indices into screens[0].  SetEntries mapped those indices to
         * real RGB in the CLUT.
         * s_use_copybits=1 (NuBus IIci/Quadra): CopyBits through ROM slot manager,
         * safe in 24-bit mode.  Direct memcpy to 0xf9xxxxxx truncates to 24-bit
         * and corrupts the system heap.
         * s_use_copybits=0 (SE/30): FB is in main RAM, direct memcpy is safe. */
        if (opt_scale2x) {
            /* 2× pixel-scale color path.
             * Always blit the full screens[0] (320×200) at 2× centred.
             * 320×2=640, 200×2=400; on a 640×480 screen: 40px top/bottom margin. */
            int vy0 = (s_color_screen_h - SCREENHEIGHT * 2) / 2;
            int vx0 = (s_color_screen_w - SCREENWIDTH  * 2) / 2;

            static int last_c2x_vy0 = -1;
            if (vy0 != last_c2x_vy0) {
                SetPort(bg_window);
                FillRect(&bg_window->portRect, &qd.black);
                last_c2x_vy0 = vy0;
            }

            if (s_use_copybits) {
                Rect srcRect, dstRect;
                s_src_pm.baseAddr = (Ptr)screens[0];
                SetPort(bg_window);
                srcRect.top = 0;              srcRect.left  = 0;
                srcRect.bottom = SCREENHEIGHT; srcRect.right  = SCREENWIDTH;
                dstRect.top    = vy0;                    dstRect.left  = vx0;
                dstRect.bottom = vy0 + SCREENHEIGHT * 2; dstRect.right  = vx0 + SCREENWIDTH * 2;
                CopyBits((BitMap *)&s_src_pm,
                         (BitMap *)*((CGrafPtr)bg_window)->portPixMap,
                         &srcRect, &dstRect, srcCopy, NULL);
            } else {
                byte *src = screens[0];
                int fy;
                for (fy = 0; fy < SCREENHEIGHT; fy++) {
                    byte *sr = src + fy * SCREENWIDTH;
                    byte *d0 = fb_color_base + (vy0+fy*2)   * fb_color_rowbytes + vx0;
                    byte *d1 = fb_color_base + (vy0+fy*2+1) * fb_color_rowbytes + vx0;
                    int fx;
                    for (fx = 0; fx < SCREENWIDTH; fx++) {
                        byte v = sr[fx];
                        d0[fx*2] = d0[fx*2+1] = v;
                        d1[fx*2] = d1[fx*2+1] = v;
                    }
                }
            }
            return;
        }

        if (s_use_copybits) {
            Rect srcRect, dstRect;
            s_src_pm.baseAddr  = (Ptr)screens[0];
            srcRect.top = 0;             srcRect.left  = 0;
            srcRect.bottom = SCREENHEIGHT; srcRect.right = SCREENWIDTH;
            dstRect.top    = fb_color_yoff;
            dstRect.left   = fb_color_xoff;
            dstRect.bottom = fb_color_yoff + SCREENHEIGHT;
            dstRect.right  = fb_color_xoff + SCREENWIDTH;
            SetPort(bg_window);
            CopyBits((BitMap *)&s_src_pm,
                     (BitMap *)*((CGrafPtr)bg_window)->portPixMap,
                     &srcRect, &dstRect, srcCopy, NULL);
        } else {
            byte *src = screens[0];
            int y;
            for (y = 0; y < SCREENHEIGHT; y++) {
                memcpy(fb_color_base
                       + (y + fb_color_yoff) * fb_color_rowbytes + fb_color_xoff,
                       src + y * SCREENWIDTH, SCREENWIDTH);
            }
        }
        return;
    }

    /* Rebuild mono_colormaps at most once per frame. */
    if (mono_dirty) {
        I_BuildMonoColormaps(); mono_dirty = 0;
    }

    byte *src = screens[0];
    /* xoff/yoff: physical screen centering for the 320×200 game area.
     * Always the real screen constants — NOT fb_mono_{xoff,yoff}, which are 0
     * in 2x mode (they address the compact source buffer, not the screen). */
    int   xoff = s_phys_xoff_byte << 3;  /* pixels: 12*8 = 96 */
    int   yoff = s_phys_yoff;             /* rows: 71 */
    int   y, x;

    /* 2x pixel-scale layout (computed each frame; only used when opt_scale2x).
     * scaledviewwidth/8 = source buf bytes/row (32 for blocks=8).
     * The expanded view is centred on the physical 512px screen; status bar
     * (320px wide) is centred with 96px black bars on each side. */
    int scale2x_src_rbytes = scaledviewwidth >> 3;
    int scale2x_exp_bytes  = scale2x_src_rbytes * 2;
    int scale2x_dest_x0    = (s_phys_rbytes - scale2x_exp_bytes) / 2;
    int scale2x_dest_y0    = (s_phys_height - viewheight * 2 - SBARHEIGHT) / 2;
    int scale2x_sbar_y0    = scale2x_dest_y0 + viewheight * 2;

    /* Direct mode: GS_LEVEL gameplay with no menu/wipe/automap.
     * Walls/floors/ceilings already rendered to 1-bit fb by R_DrawColumn_Mono
     * and R_DrawSpan_Mono.  We only need to blit the status bar and border,
     * and optionally overlay HU messages over the view area. */
    boolean is_direct = (gamestate == GS_LEVEL && !automapactive && gametic > 0
                         && fb_mono_base != NULL && !wipe_in_progress && !menuactive);

    /* Track direct/non-direct transitions for border cache logic. */
    static boolean last_direct = false;

    /* 2x mode: if the view layout changed (e.g. screenblocks changed), clear
     * fb_offscreen_buf and the real screen to black so stale pixels are gone. */
    if (opt_scale2x) {
        static int last_2x_dest_y0 = -1;
        if (scale2x_dest_y0 != last_2x_dest_y0) {
            memset(FB_OUT, 0xFF, (size_t)s_phys_rbytes * s_phys_height);
            if (real_fb_base)
                memset(real_fb_base, 0xFF, (size_t)s_phys_rbytes * s_phys_height);
            last_2x_dest_y0 = scale2x_dest_y0;
        }
    }

    if (opt_scale2x && is_direct) {
        /* --- 2x pixel-scale direct path -----------------------------------
         * 1. Expand fb_source_buf → fb_offscreen_buf at 2× (view area).
         * 2. Blit status bar 1× centred → fb_offscreen_buf.
         * 3. HU overlay (if active): expand-and-AND mask over view rows.
         * Flip is handled below along with the normal flip. */
        boolean do_border = (border_needs_blit || !last_direct);
        if (do_border) border_needs_blit = 0;

        expand2x_blit(fb_source_buf, FB_OUT,
                      viewheight, scale2x_src_rbytes,
                      scale2x_dest_x0, scale2x_dest_y0, s_phys_rbytes);

        /* Status bar: always blit in 2x mode (view never fills full screen). */
        {
            const int sbar0 = SCREENHEIGHT - SBARHEIGHT;
            for (y = 0; y < SBARHEIGHT; y++) {
                const byte    *sr  = src + (sbar0 + y) * SCREENWIDTH;
                unsigned char *dst = FB_OUT
                                     + (scale2x_sbar_y0 + y) * s_phys_rbytes
                                     + s_phys_xoff_byte;
                for (x = 0; x < SCREENWIDTH; x += 8) *dst++ = blit8_sbar_thresh(sr + x);
            }
        }

        /* HU overlay: expand screens[0] view area (viewwindowx=0, viewwindowy=0). */
        if (hu_overlay_active) {
            int vy;
            for (vy = 0; vy < viewheight; vy++) {
                const byte    *sr  = src + vy * SCREENWIDTH;
                int            dr  = scale2x_dest_y0 + vy * 2;
                unsigned char *dp0 = FB_OUT + dr * s_phys_rbytes + scale2x_dest_x0;
                unsigned char *dp1 = dp0 + s_phys_rbytes;
                int sx;
                for (sx = 0; sx < scaledviewwidth; sx += 8) {
                    unsigned char mask = blit8_black(sr + sx);
                    if (mask) {
                        unsigned short ex = expand2x_lut[mask];
                        int c = (sx >> 2);   /* sx/8 * 2 dest bytes offset */
                        dp0[c] &= ~(unsigned char)(ex >> 8);
                        dp0[c+1] &= ~(unsigned char)ex;
                        dp1[c] &= ~(unsigned char)(ex >> 8);
                        dp1[c+1] &= ~(unsigned char)ex;
                    }
                }
            }
        }

    } else if (is_direct) {
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

        /* --- Region 4: status bar rows.  Always blit when view doesn't
         * cover sbar (vy1 <= sbar0) or border forced.  When view fills
         * the screen fully (vy1 > sbar0), only re-blit when ST_Drawer drew.
         * Note: vy1 == sbar0 at screenblocks=10 (view bottom flush with sbar
         * top) — must use <= not < or the sbar never blits at that size. --- */
        {
            extern int sbar_dirty;
            if (vy1 <= sbar0 && (do_border || sbar_dirty)) {
                sbar_dirty = 0;
                for (y = sbar0; y < SCREENHEIGHT; y++) {
                    const byte    *sr  = src + y * SCREENWIDTH;
                    unsigned char *dst = (unsigned char *)(fb_mono_base
                                         + (y + yoff) * fb_mono_rowbytes) + (xoff >> 3);
                    for (x = 0; x < SCREENWIDTH; x += 8) *dst++ = blit8_sbar_thresh(sr + x);
                }
            }
        }

    } else {
        /* Non-direct (menu, wipe, intermission, title): blit screens[0] to 1-bit fb.
         * Always targets fb_offscreen_buf at physical screen offsets — this works
         * for both normal and 2x mode (in normal mode fb_mono_base==fb_offscreen_buf).
         * Menu frames: fill entire offscreen buffer solid black (0xFF = all pixels on
         * in Mac 1-bit), covering the full area in both 2x and non-2x layouts.
         * The menu overlay then draws white text on top.  Wipes/title/intermission
         * use the normal screens[0] blit path.
         * last_direct=false ensures the next direct frame triggers do_border=true,
         * re-blitting the border after wipe/menu may have written over it. */
        int col_off   = s_phys_xoff_byte;        /* byte col of 320px game area: 12 */
        int col_bytes = SCREENWIDTH >> 3;         /* 40 bytes per row */

        if (menuactive) {
            unsigned char *fb = FB_OUT;
            if (!menu_bg_valid) {
                /* First menu frame: snapshot current framebuffer (game frame) */
                for (y = 0; y < SCREENHEIGHT; y++)
                    memcpy(menu_bg_1bit + y * col_bytes,
                           fb + (y + s_phys_yoff) * s_phys_rbytes + col_off,
                           col_bytes);
                menu_bg_valid = true;
            } else {
                /* Subsequent menu frames: restore frozen game frame */
                for (y = 0; y < SCREENHEIGHT; y++)
                    memcpy(fb + (y + s_phys_yoff) * s_phys_rbytes + col_off,
                           menu_bg_1bit + y * col_bytes,
                           col_bytes);
            }
        } else {
            menu_bg_valid = false;
            /* Full screens[0] blit for wipe/title/intermission (unchanged) */
            for (y = 0; y < SCREENHEIGHT; y++) {
                const byte    *sr  = src + y * SCREENWIDTH;
                unsigned char *dst = FB_OUT
                                     + (y + s_phys_yoff) * s_phys_rbytes + col_off;
                for (x = 0; x < SCREENWIDTH; x += 8)
                    *dst++ = blit8_sbar_thresh(sr + x);
            }
        }
    }

    last_direct = is_direct;
    /* Invalidate menu background snapshot on every direct frame so the next
     * menu open always captures the most recent game frame, not the first-ever
     * ESC press.  menu_bg_valid is never cleared by the non-direct path during
     * gameplay (menu close → is_direct immediately true → non-direct else never
     * runs).  One boolean write per frame; zero overhead. */
    if (is_direct) menu_bg_valid = false;

    /* Menu overlay: solid-white mask applied over the full screen.
     * Only runs when the menu is open (menuactive implies !is_direct). */
    if (menuactive && menu_overlay_buf) {
        /* Write menu overlay at physical offsets — FB_OUT is correct for both modes. */
        byte *overlay = menu_overlay_buf;
        unsigned char *fb = FB_OUT;
        for (y = 0; y < SCREENHEIGHT; y++) {
            const byte    *ovr = overlay + y * SCREENWIDTH;
            unsigned char *dst = fb
                                 + (y + s_phys_yoff) * s_phys_rbytes + s_phys_xoff_byte;
            for (x = 0; x < SCREENWIDTH; x += 8) { *dst &= ~blit8_menu(ovr + x); dst++; }
        }
    }

    /* Double-buffer flip: copy completed frame from fb_offscreen_buf → real screen.
     * Uses long* (MOVE.L) instead of memcpy to guarantee 32-bit bus transfers —
     * 4 bytes per bus cycle vs 1, ~4× faster on 68030's 32-bit data bus. */
    if (real_fb_base) {
        int flip_y;
        if (s_use_copybits) {
            /* NuBus machine: CopyBits routes through ROM slot manager.
             * Direct MOVE.L to real_fb_base (0xf9xxxxxx) truncates to 24-bit
             * in 24-bit addressing mode → corrupts system heap. */
            Rect blitRect;
            SetPort(bg_window);
            if (opt_scale2x && (is_direct || menuactive)) {
                blitRect.top    = scale2x_dest_y0;
                blitRect.left   = 0;
                blitRect.bottom = scale2x_sbar_y0 + SBARHEIGHT;
                blitRect.right  = s_phys_rbytes * 8;
            } else {
                blitRect.top    = s_phys_yoff;
                blitRect.left   = s_phys_xoff_byte * 8;
                blitRect.bottom = s_phys_yoff + SCREENHEIGHT;
                blitRect.right  = s_phys_xoff_byte * 8 + SCREENWIDTH;
            }
            CopyBits(&s_offscreen_bm, &bg_window->portBits,
                     &blitRect, &blitRect, srcCopy, NULL);
        } else if (opt_scale2x && (is_direct || menuactive)) {
            /* SE/30 2x direct/menu flip */
            int flip_end = scale2x_sbar_y0 + SBARHEIGHT;
            int longs_per_row = s_phys_rbytes >> 2;  /* 64/4 = 16 */
            for (flip_y = scale2x_dest_y0; flip_y < flip_end; flip_y++) {
                long *dst = (long *)((unsigned char *)real_fb_base + flip_y * s_phys_rbytes);
                const long *sr = (const long *)(fb_offscreen_buf + flip_y * s_phys_rbytes);
                int i;
                for (i = 0; i < longs_per_row; i++) dst[i] = sr[i];
            }
        } else {
            /* SE/30 normal flip: 40 bytes × 200 rows (320px game area, centred).
             * 40 bytes = 10 longs.  s_phys_xoff_byte (12) is 4-aligned.
             * Unrolled 10× MOVE.L to guarantee 32-bit bus transfers
             * (GCC converts loop-based copies back to memcpy). */
            for (flip_y = 0; flip_y < SCREENHEIGHT; flip_y++) {
                int row = flip_y + s_phys_yoff;
                long *d = (long *)((unsigned char *)real_fb_base + row * s_phys_rbytes + s_phys_xoff_byte);
                const long *s = (const long *)(fb_offscreen_buf + row * s_phys_rbytes + s_phys_xoff_byte);
                d[0]=s[0]; d[1]=s[1]; d[2]=s[2]; d[3]=s[3]; d[4]=s[4];
                d[5]=s[5]; d[6]=s[6]; d[7]=s[7]; d[8]=s[8]; d[9]=s[9];
            }
        }
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
