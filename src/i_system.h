// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// DESCRIPTION:
//	System specific interface stuff.
//
//-----------------------------------------------------------------------------


#ifndef __I_SYSTEM__
#define __I_SYSTEM__

#include "d_ticcmd.h"
#include "d_event.h"

#ifdef __GNUG__
#pragma interface
#endif


// Called by DoomMain.
void I_Init (void);

// Called by startup code
// to get the ammount of memory to malloc
// for the zone management.
byte*	I_ZoneBase (int *size);


// Called by D_DoomLoop,
// returns current time in tics.
int I_GetTime (void);

// Returns the raw Mac 60Hz TickCount — safe to call from any module.
// Use this instead of calling TickCount() directly: that function is
// declared 'pascal ULONGINT' which can affect register-save code
// generation for the entire translation unit on 68k.
long I_GetMacTick (void);


//
// Called by D_DoomLoop,
// called before processing any tics in a frame
// (just after displaying a frame).
// Time consuming syncronous operations
// are performed here (joystick reading).
// Can call D_PostEvent.
//
void I_StartFrame (void);


//
// Called by D_DoomLoop,
// called before processing each tic in a frame.
// Quick syncronous operations are performed here.
// Can call D_PostEvent.
void I_StartTic (void);

// Asynchronous interrupt functions should maintain private queues
// that are read by the synchronous functions
// to be converted into events.

// Either returns a null ticcmd,
// or calls a loadable driver to build it.
// This ticcmd will then be modified by the gameloop
// for normal input.
ticcmd_t* I_BaseTiccmd (void);


// Called by M_Responder when quit is selected.
// Clean exit, displays sell blurb.
void I_Quit (void);


// Allocates from low memory under dos,
// just mallocs under unix
byte* I_AllocLow (int length);

void I_Tactile (int on, int off, int total);


void I_Error (char *error, ...);

/* doom_log / doom_log_flush — release builds: no-op macros.
 * Debug builds: write to doom_log.txt.
 * NOTE: doomdef.h also defines these macros; this guard prevents redefinition. */
#ifndef doom_log
#  ifdef DOOM_RELEASE_BUILD
#    define doom_log(fmt, ...)  ((void)0)
#    define doom_log_flush()    ((void)0)
#  else
void doom_log (const char *fmt, ...);
void doom_log_flush (void);
#  endif
#endif
void I_MacBeep (int n);      /* play n short system beeps (detail-level feedback) */

/*
 * Mac model IDs returned by Gestalt(gestaltMachineType, ...).
 * Add entries here as we discover model-specific init paths.
 * Gestalt values from Inside Macintosh: Overview, Appendix B.
 */
typedef enum {
    kMacModelSE     =  5,   /* Mac SE      — 68000, no cache          */
    kMacModelII     =  6,   /* Mac II      — 68020                    */
    kMacModelIIx    =  7,   /* Mac IIx     — 68030                    */
    kMacModelIIcx   =  8,   /* Mac IIcx    — 68030                    */
    kMacModelSE30   =  9,   /* Mac SE/30   — 68030, primary target    */
    kMacModelIIci   = 11,   /* Mac IIci    — 68030                    */
    kMacModelIIfx   = 13,   /* Mac IIfx    — 68030                    */
    kMacModelIIsi   = 18,   /* Mac IIsi    — 68030                    */
    kMacModelLC     = 19,   /* Mac LC      — 68020                    */
    kMacModelQ700   = 20,   /* Quadra 700  — 68040                    */
    kMacModelQ900   = 21,   /* Quadra 900  — 68040                    */
    kMacModelQ650   = 32,   /* Quadra 650  — 68040                    */
    kMacModelQ610   = 53,   /* Quadra 610  — 68040                    */
} MacModelID;


#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
