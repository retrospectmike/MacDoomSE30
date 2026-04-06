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
#include <Gestalt.h>

#include <stdlib.h>
#include <setjmp.h>
extern jmp_buf doom_quit_jmp;
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <Files.h>

/* MacModelID enum is in i_system.h */

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
/* Log file opened via direct HFS calls (bypasses newlib fopen/WD ambiguity).
 * g_log_refnum: HFS file reference number (-1 = not open).
 * g_log_vrefnum: volume reference number of the log file's volume (for FlushVol). */
static short g_log_refnum  = -1;
static short g_log_vrefnum =  0;

#ifndef DOOM_RELEASE_BUILD

void doom_log(const char *fmt, ...)
{
    char buf[1024];
    char out[1024];
    va_list ap;
    const char *p;
    char *q;
    long len;

    if (g_log_refnum < 0) return;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* Classic Mac OS text files use \r; translate \n here. */
    for (p = buf, q = out; *p && q < out + sizeof(out) - 1; p++, q++)
        *q = (*p == '\n') ? '\r' : *p;
    len = q - out;
    if (len > 0)
        FSWrite(g_log_refnum, &len, out);  /* FSWrite goes straight to HFS */
}

/* Commit HFS sector cache to the underlying volume.
 * FSWrite goes to HFS but HFS may buffer; FlushVol forces it to disk/ExtFS.
 * Call after any log block whose loss would be unacceptable. */
void doom_log_flush(void)
{
    if (g_log_refnum >= 0)
        FlushVol(NULL, g_log_vrefnum);
}

#endif /* !DOOM_RELEASE_BUILD */

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
#ifndef DOOM_RELEASE_BUILD
    /* Use GetProcessInformation to get the app's FSSpec, then open the log
     * file in the same folder using explicit HFS calls.  This avoids the
     * PBHSetVol/WD ambiguity that causes fopen("doom_log.txt","w") to
     * silently fail when the default working directory isn't set correctly. */
    ProcessSerialNumber psn;
    ProcessInfoRec      info;
    FSSpec              appSpec, logSpec;
    OSErr               err;
    /* Pascal string for the log filename */
    static const unsigned char kLogName[14] = {
        12, 'd','o','o','m','_','l','o','g','.','t','x','t'
    };

    psn.highLongOfPSN = 0;
    psn.lowLongOfPSN  = kCurrentProcess;
    info.processInfoLength = sizeof(ProcessInfoRec);
    info.processName       = NULL;
    info.processAppSpec    = &appSpec;

    if (GetProcessInformation(&psn, &info) != noErr) {
        printf("I_OpenLog: GetProcessInformation failed\n");
        return;
    }

    err = FSMakeFSSpec(appSpec.vRefNum, appSpec.parID, kLogName, &logSpec);
    if (err != noErr && err != fnfErr) {
        printf("I_OpenLog: FSMakeFSSpec err=%d\n", (int)err);
        return;
    }

    /* Create the file (type TEXT, creator ttxt=SimpleText).
     * Ignore dupFNErr — file exists from a previous run; we'll truncate it. */
    err = FSpCreate(&logSpec, 'ttxt', 'TEXT', 0 /* smRoman */);
    if (err != noErr && err != dupFNErr) {
        printf("I_OpenLog: FSpCreate err=%d vRef=%d parID=%ld\n",
               (int)err, (int)appSpec.vRefNum, (long)appSpec.parID);
        return;
    }

    err = FSpOpenDF(&logSpec, fsWrPerm, &g_log_refnum);
    if (err != noErr) {
        printf("I_OpenLog: FSpOpenDF err=%d vRef=%d parID=%ld\n",
               (int)err, (int)appSpec.vRefNum, (long)appSpec.parID);
        g_log_refnum = -1;
        return;
    }

    /* Truncate to zero so this run's log starts fresh */
    SetEOF(g_log_refnum, 0);
    SetFPos(g_log_refnum, fsFromStart, 0);

    g_log_vrefnum = logSpec.vRefNum;
    doom_log("=== Doom SE/30 log opened ===\n");
#endif
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
    long machine_type = 0;
    Gestalt(gestaltMachineType, &machine_type);
    doom_log("I_Init: gestaltMachineType=%ld\n", machine_type);

    /* Phase 1A: Enable 68030 data cache if disabled.
     * The SE/30 ROM enables instruction cache but NOT data cache.
     * 68030 data cache is write-through (safe for DMA/framebuffer).
     * HWPriv selector 2 = SwapDataCache(enable). Returns previous state.
     *
     * SE/30 ONLY: other 68030 ROMs (IIci, IIcx, etc.) map _HWPriv selector 2
     * to a different routine.  On those machines the call writes unexpected
     * values to CACR — sets FD (freeze data cache) and reserved bits — which
     * produces a frozen, stale data cache.  When a Level 2 interrupt fires
     * shortly after, the dispatch chain is read through the frozen cache and
     * follows a corrupt function pointer → crash.  Register forensics at crash
     * time: CACR=$2901 (FD=1, reserved bit 13 set), PC=$0001AD38 (system data
     * area), SR=$2204 (supervisor, IPL=2), D0=A0=$0001ABDA (bad dispatch ptr).
     * Confirmed on real Quadra 650 hardware and Snow/Mac IIci emulation. */
    if (machine_type == kMacModelSE30) {
        long prev_d0 = 2;           /* selector: SwapDataCache */
        long prev_a0 = 1;           /* TRUE = enable */
        asm volatile (
            "move.l %1, %%d0\n\t"
            "move.l %2, %%a0\n\t"
            "dc.w 0xA198\n\t"       /* _HWPriv */
            "move.l %%a0, %0"
            : "=g"(prev_a0)
            : "g"(prev_d0), "g"(prev_a0)
            : "d0", "d1", "d2", "a0", "a1", "memory"
        );
        doom_log("I_Init: SwapDataCache(TRUE) prev=%ld\n", prev_a0);
    } else {
        doom_log("I_Init: skipping SwapDataCache on model %ld\n", machine_type);
    }

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
    /* Try to get 48MB first, fall back to smaller sizes.
     *
     * MaxMem guard: in 24-bit addressing mode (no Mode32), NewPtr() treats
     * its size argument as 24 bits.  Sizes whose low 24 bits are zero —
     * 48MB (0x03000000), 32MB (0x02000000), 16MB (0x01000000) — silently
     * become NewPtr(0) which returns a valid non-NULL pointer to a zero-byte
     * block.  Z_Init then uses that as a multi-MB zone → heap walk → crash.
     * MaxMem() returns the largest contiguous free block (compacting first),
     * correctly bounded by the addressing mode.  Skip any size that exceeds it. */
    int try_sizes[] = //{ 48*1024*1024, 32*1024*1024, 16*1024*1024,
                      { 8*1024*1024,  4*1024*1024,  
                        2*1024*1024 };
    Size   grow_size = 0;
    Size   max_block = MaxMem(&grow_size);
    int i;
    byte *zone;

    doom_log("I_ZoneBase: MaxMem=%ld FreeMem=%ld\n", (long)max_block, FreeMem());
    doom_log_flush();
    for (i = 0; i < 3; i++) {
        if ((long)try_sizes[i] > (long)max_block) {
            doom_log("I_ZoneBase: skipping %d MB (> MaxMem)\n",
                     try_sizes[i] / (1024*1024));
            doom_log_flush();
            continue;
        }
        doom_log("I_ZoneBase: trying %d MB...\n", try_sizes[i] / (1024*1024));
        doom_log_flush();
        zone = (byte *)NewPtr(try_sizes[i]);
        if (zone != NULL) {
            *size = try_sizes[i];
            doom_log("I_ZoneBase: allocated %d MB zone at %p\n",
                     try_sizes[i] / (1024*1024), zone);
            doom_log_flush();
            return zone;
        }
        doom_log("I_ZoneBase: %d MB failed\n", try_sizes[i] / (1024*1024));
        doom_log_flush();
    }

    /* Last resort: use malloc */
    *size = 2 * 1024 * 1024;
    zone = (byte *)malloc(*size);
    if (zone == NULL)
        I_Error("I_ZoneBase: failed to allocate zone memory");

    doom_log("I_ZoneBase: malloc fallback, %d MB\n", *size / (1024*1024));
    doom_log_flush();
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
    doom_log("I_Quit: M_SaveDefaults done — returning to main\n");
    if (g_log_refnum >= 0) {
        FSClose(g_log_refnum);
        FlushVol(NULL, g_log_vrefnum);
        g_log_refnum = -1;
    }
    longjmp(doom_quit_jmp, 1);
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
    if (g_log_refnum >= 0) {
        FSClose(g_log_refnum);
        FlushVol(NULL, g_log_vrefnum);
        g_log_refnum = -1;
    }

    /* Wait for mouse click so the user can read the console */
    doom_log("(click mouse button to exit)\n");
    while (!Button())
        GetOSEvent(everyEvent, &evt);  /* keep Mac OS happy */

    D_QuitNetGame();
    I_ShutdownGraphics();
    longjmp(doom_quit_jmp, 2);   /* fatal error — unwind to main() for clean exit */
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
