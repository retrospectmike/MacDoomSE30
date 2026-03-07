/*
 * i_system_mac.c — System interface for Doom SE/30
 *
 * Provides: timing, memory allocation, error handling, quit
 */

#include <MacTypes.h>
#include <Memory.h>
#include <Events.h>
#include <OSUtils.h>
#include <Processes.h>
#include <Sound.h>

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <Files.h>

/*
 * Debug log — output goes to the log FILE only.
 *
 * printf() to the Retro68 console is intentionally disabled:
 * when the console window accumulates enough text to scroll, Mac OS
 * ScrollRect copies a region of the screen bitmap upward.  That region
 * overlaps with our direct-framebuffer game rendering, dragging game
 * pixels into the console area and creating ghost images.
 * All diagnostic output is in doom_log.txt; open it after each run.
 */
static FILE *g_logfile = NULL;

void doom_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    const char *p;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (g_logfile) {
        /* Classic Mac OS text files use \r (0x0D) line endings.
         * SimpleText and TeachText will show \n (0x0A) as a square,
         * so translate here rather than changing every call site. */
        for (p = buf; *p; p++)
            fputc(*p == '\n' ? '\r' : *p, g_logfile);
        fflush(g_logfile);
    }
}

/* Force HFS to commit its sector cache to the underlying volume.
 * fflush() only flushes the stdio buffer into the HFS layer; if the
 * process crashes without a clean exit the HFS dirty cache is lost.
 * Call this after any log block whose loss would be unacceptable
 * (e.g., the 35-tic FPS window in D_DoomLoop). */
void doom_log_flush(void)
{
    if (g_logfile)
        fflush(g_logfile);
    FlushVol(NULL, 0);   /* flush default HFS volume → commits to ExtFS */
}

/* Play n short system beeps with a brief gap between them.
 * Used for detail-level feedback: HIGH=1, LOW=2, QUAD=3. */
void I_MacBeep(int n)
{
    int i;
    for (i = 0; i < n; i++) {
        SysBeep(6);          /* 6 ticks ≈ 0.1 s beep */
        if (i < n - 1)
            Delay(12, NULL); /* 12 ticks ≈ 0.2 s gap between beeps */
    }
}

void I_OpenLog(void)
{
    g_logfile = fopen("doom_log.txt", "w");
    if (g_logfile) {
        /* Mark the file as Mac TEXT so SimpleText can open it */
        {
            unsigned char pname[14];  /* Pascal string */
            FInfo fi;
            pname[0] = 12;  /* length of "doom_log.txt" */
            pname[1]='d'; pname[2]='o'; pname[3]='o'; pname[4]='m';
            pname[5]='_'; pname[6]='l'; pname[7]='o'; pname[8]='g';
            pname[9]='.'; pname[10]='t'; pname[11]='x'; pname[12]='t';
            if (GetFInfo(pname, 0, &fi) == noErr) {
                fi.fdType    = 'TEXT';
                fi.fdCreator = 'ttxt';  /* SimpleText */
                SetFInfo(pname, 0, &fi);
            }
        }
        doom_log("=== Doom SE/30 log opened ===\n");
    }
}

/*
 * mac_path() — strip Unix-style "./" prefix from paths.
 * Retro68's _open_r passes names verbatim to HOpenDF(0,0,name).
 * HFS does not understand "./" — it treats it as a literal filename.
 * Bare names like "doom1.wad" resolve correctly against the default
 * directory set by PBHSetVolSync in SetCWDToAppFolder().
 */
static const char *mac_path(const char *path)
{
    while (path[0] == '.' && path[1] == '/')
        path += 2;
    return path;
}

/* access(): check if a file exists — strip "./" before calling fopen. */
int access(const char *path, int mode)
{
    FILE *f;
    (void)mode;
    path = mac_path(path);
    f = fopen(path, "r");
    if (f) {
        fclose(f);
        return 0;
    }
    return -1;
}

/* mkdir(): stubbed — Doom only calls this for the CD-ROM path. */
int mkdir(const char *path, mode_t mode)
{
    (void)path; (void)mode;
    return 0;
}

#include "doomdef.h"
#include "doomstat.h"
#include "d_event.h"
#include "d_net.h"
#include "g_game.h"
#include "i_system.h"
#include "i_sound.h"
#include "i_video.h"
#include "i_net.h"
#include "m_misc.h"

/* Tic counter base — TickCount returns 1/60 sec ticks,
   Doom uses 35 tics/sec (TICRATE). */
static long basetime = 0;

int I_GetTime(void)
{
    long now = TickCount();
    if (basetime == 0)
        basetime = now;
    /* Convert from 60Hz Mac ticks to 35Hz Doom tics */
    return (int)(((now - basetime) * TICRATE) / 60);
}

long I_GetMacTick(void)
{
    return (long)TickCount();
}

void I_Init(void)
{
    I_InitSound();
    /* I_InitGraphics is called separately by D_DoomMain */
}

/*
 * I_ZoneBase — allocate the main memory zone for Doom
 *
 * With 64MB RAM on the SE/30, we can be generous.
 * Request a large block for the zone allocator.
 */
byte *I_ZoneBase(int *size)
{
    /* Try to get 48MB first, fall back to smaller sizes */
    int try_sizes[] = { 48*1024*1024, 32*1024*1024, 16*1024*1024,
                         8*1024*1024,  4*1024*1024,  2*1024*1024 };
    int i;
    byte *zone;

    doom_log("I_ZoneBase: starting allocation attempts\n");
    for (i = 0; i < 6; i++) {
        doom_log("I_ZoneBase: trying %d MB...\n", try_sizes[i] / (1024*1024));
        zone = (byte *)NewPtr(try_sizes[i]);
        if (zone != NULL) {
            *size = try_sizes[i];
            doom_log("I_ZoneBase: allocated %d MB zone at %p\n",
                     try_sizes[i] / (1024*1024), zone);
            return zone;
        }
        doom_log("I_ZoneBase: %d MB failed\n", try_sizes[i] / (1024*1024));
    }

    /* Last resort: use malloc */
    *size = 2 * 1024 * 1024;
    zone = (byte *)malloc(*size);
    if (zone == NULL)
        I_Error("I_ZoneBase: failed to allocate zone memory");

    doom_log("I_ZoneBase: malloc fallback, %d MB\n", *size / (1024*1024));
    return zone;
}

ticcmd_t emptycmd;

ticcmd_t *I_BaseTiccmd(void)
{
    return &emptycmd;
}

void I_Quit(void)
{
    doom_log("I_Quit: starting\n");
    doom_log_flush();
    D_QuitNetGame();
    doom_log("I_Quit: D_QuitNetGame done\n");
    doom_log_flush();
    I_ShutdownSound();
    doom_log("I_Quit: I_ShutdownSound done\n");
    doom_log_flush();
    I_ShutdownGraphics();
    doom_log("I_Quit: I_ShutdownGraphics done\n");
    doom_log_flush();
    M_SaveDefaults();
    doom_log("I_Quit: M_SaveDefaults done — exiting\n");
    if (g_logfile) { fclose(g_logfile); g_logfile = NULL; }
    ExitToShell();   /* bypasses C++ destructor chain which can crash on exit */
}

byte *I_AllocLow(int length)
{
    byte *mem = (byte *)malloc(length);
    if (mem == NULL)
        I_Error("I_AllocLow: out of memory allocating %d bytes", length);
    memset(mem, 0, length);
    return mem;
}

void I_Tactile(int on, int off, int total)
{
    /* No force feedback on Mac */
    (void)on; (void)off; (void)total;
}

void I_Error(char *error, ...)
{
    va_list argptr;
    char    msg[512];
    EventRecord evt;

    va_start(argptr, error);
    vsnprintf(msg, sizeof(msg), error, argptr);
    va_end(argptr);

    /* Write to console and log file */
    doom_log("I_Error: %s\n", msg);
    if (g_logfile) {
        fflush(g_logfile);
        fclose(g_logfile);
        g_logfile = NULL;
    }

    /* Wait for mouse click so the user can read the console */
    doom_log("(click mouse button to exit)\n");
    while (!Button())
        GetOSEvent(everyEvent, &evt);  /* keep Mac OS happy */

    D_QuitNetGame();
    I_ShutdownGraphics();
    ExitToShell();
}

void I_StartFrame(void)
{
    /* Nothing needed on Mac — input is polled in I_StartTic */
}

void I_StartTic(void)
{
    /* Will be implemented in i_input_mac.c */
    extern void I_PollMacInput(void);
    I_PollMacInput();
}
