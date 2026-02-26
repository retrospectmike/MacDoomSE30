/*
 * i_video_mac.c — Video interface for Doom SE/30
 *
 * Doom renders to a 320x200 8-bit buffer (screens[0]).
 * We convert that to 1-bit and blit to the 512x342 framebuffer.
 *
 * Phase 1: simple threshold (no dithering yet)
 * Phase 2: ordered dithering + proper scaling
 */

#include <QuickDraw.h>
#include <Memory.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "doomdef.h"
#include "doomstat.h"
#include "d_main.h"
#include "i_system.h"
#include "i_video.h"
#include "v_video.h"

/* Mac screen info */
static Ptr   fb_base;       /* framebuffer base address */
static int   fb_rowbytes;   /* bytes per row */
static int   fb_width;      /* pixels */
static int   fb_height;     /* pixels */

/* Grayscale palette: maps Doom's 256-color palette to 0-255 grayscale */
static byte  grayscale_pal[256];

/* 4x4 Bayer ordered dither threshold matrix (0-15 scaled to 0-255) */
static const byte bayer4x4[4][4] = {
    {  0, 136,  34, 170 },
    { 204,  68, 238, 102 },
    {  51, 187,  17, 153 },
    { 255, 119, 221,  85 }
};

void I_InitGraphics(void)
{
    BitMap *screen = &qd.screenBits;
    fb_base     = screen->baseAddr;
    fb_rowbytes = screen->rowBytes;
    fb_width    = screen->bounds.right  - screen->bounds.left;
    fb_height   = screen->bounds.bottom - screen->bounds.top;

    printf("I_InitGraphics: %dx%d, rowBytes=%d\n",
           fb_width, fb_height, fb_rowbytes);

    /* Allocate Doom's screen buffer: 320x200 8-bit */
    screens[0] = (byte *)malloc(SCREENWIDTH * SCREENHEIGHT);
    if (!screens[0])
        I_Error("I_InitGraphics: failed to allocate screen buffer");
}

void I_ShutdownGraphics(void)
{
    /* Nothing to clean up */
}

/*
 * I_SetPalette — Doom calls this when the palette changes.
 * We convert the RGB palette to grayscale values for our 1-bit output.
 * palette is 768 bytes: 256 entries of (R, G, B).
 */
/* Gamma applied to grayscale before dithering.
 * Doom's palette is designed for CRT gamma ~2.2; raw luminance values
 * are perceptually dark, causing midtone surfaces to dither to solid black
 * on a 1-bit display.  A gamma < 1.0 brightens midtones while leaving
 * true black (0) and true white (255) unchanged.
 * Tune DITHER_GAMMA: lower = brighter.  0.60 is a good starting point. */
#define DITHER_GAMMA 0.60f

void I_SetPalette(byte *palette)
{
    int i;
    for (i = 0; i < 256; i++) {
        int r = palette[i * 3 + 0];
        int g = palette[i * 3 + 1];
        int b = palette[i * 3 + 2];
        /* Standard luminance (BT.601) */
        int gray = (r * 77 + g * 150 + b * 29) >> 8;
        /* Gamma correction: brightens midtones for 1-bit dithered output.
         * Computed once per palette change, not per pixel. */
        if (gray > 0 && gray < 255)
            gray = (int)(255.0f * powf(gray / 255.0f, DITHER_GAMMA) + 0.5f);
        grayscale_pal[i] = (byte)(gray > 255 ? 255 : gray);
    }
}

/*
 * I_FinishUpdate — blit Doom's 320x200 8-bit buffer to the 512x342 1-bit screen.
 *
 * Strategy: render Doom's 320x200 into the center of the 512x342 screen.
 * For now: 1:1 mapping (no scaling), centered, with ordered dithering.
 * Phase 2 will add scaling.
 */
void I_FinishUpdate(void)
{
    byte *src = screens[0];
    int  x_offset = (fb_width  - SCREENWIDTH)  / 2;   /* (512-320)/2 = 96 */
    int  y_offset = (fb_height - SCREENHEIGHT) / 2;    /* (342-200)/2 = 71 */
    int  y, x;

    for (y = 0; y < SCREENHEIGHT; y++) {
        byte *src_row = src + y * SCREENWIDTH;
        unsigned char *dst_row = (unsigned char *)(fb_base + (y + y_offset) * fb_rowbytes);

        for (x = 0; x < SCREENWIDTH; x++) {
            int sx = x + x_offset;      /* screen x coordinate */
            int byte_idx = sx >> 3;
            int bit = 7 - (sx & 7);

            /* Convert palette index to grayscale */
            byte gray = grayscale_pal[src_row[x]];

            /* Ordered dither: compare against Bayer threshold */
            byte threshold = bayer4x4[y & 3][x & 3];

            if (gray < threshold) {
                /* Black pixel (dark = black on Mac, 1-bit = 1) */
                dst_row[byte_idx] |= (1 << bit);
            } else {
                /* White pixel */
                dst_row[byte_idx] &= ~(1 << bit);
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
