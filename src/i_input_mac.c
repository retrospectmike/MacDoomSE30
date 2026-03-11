/*
 * i_input_mac.c — Input handling for Doom SE/30
 *
 * Reads keyboard and mouse events via Mac Toolbox, posts
 * them as Doom events via D_PostEvent.
 */

#include <Events.h>
#include <MacTypes.h>

#include "doomdef.h"
#include "doomstat.h"
#include "d_event.h"
#include "d_main.h"
#include "i_video.h"

/* Mac virtual keycode -> Doom KEY_ mapping */
static int MacKeyToDoom(int macKey)
{
    switch (macKey) {
        case 0x7E: return KEY_UPARROW;
        case 0x7D: return KEY_DOWNARROW;
        case 0x7B: return KEY_LEFTARROW;
        case 0x7C: return KEY_RIGHTARROW;

        /* WASD */
        case 0x0D: return KEY_UPARROW;     /* W */
        case 0x01: return KEY_DOWNARROW;    /* S */
        case 0x00: return KEY_LEFTARROW;    /* A */
        case 0x02: return KEY_RIGHTARROW;   /* D */

        case 0x31: return ' ';              /* Space = use */
        case 0x24: return KEY_ENTER;        /* Return */
        case 0x35: return KEY_ESCAPE;       /* Escape */
        case 0x30: return KEY_TAB;          /* Tab = automap */

        /* Ctrl = fire */
        case 0x3B: return KEY_RCTRL;        /* Left Control */

        /* Shift = run */
        case 0x38: return KEY_RSHIFT;       /* Left Shift */

        /* Alt = strafe */
        case 0x3A: return KEY_RALT;         /* Left Option/Alt */

        /* Number keys for weapons */
        case 0x12: return '1';
        case 0x13: return '2';
        case 0x14: return '3';
        case 0x15: return '4';
        case 0x17: return '5';
        case 0x16: return '6';
        case 0x1A: return '7';
        case 0x1C: return '8';

        /* Comma/Period for strafe */
        case 0x2B: return ',';
        case 0x2F: return '.';

        /* F-keys */
        case 0x7A: return KEY_F1;
        case 0x78: return KEY_F2;
        case 0x63: return KEY_F3;
        case 0x76: return KEY_F4;
        case 0x60: return KEY_F5;
        case 0x61: return KEY_F6;
        case 0x62: return KEY_F7;
        case 0x64: return KEY_F8;
        case 0x65: return KEY_F9;
        case 0x6D: return KEY_F10;
        case 0x67: return KEY_F11;
        case 0x6F: return KEY_F12;

        /* Minus/Equals for screen size */
        case 0x1B: return KEY_MINUS;
        case 0x18: return KEY_EQUALS;

        /* Pause */
        case 0x71: return KEY_PAUSE;

        /* Y/N for prompts */
        case 0x10: return 'y';
        case 0x2D: return 'n';

        default: return 0;
    }
}

/* Table of all key codes we care about */
static const struct { int macKey; int doomKey; } kKeyTable[] = {
    {0x7E, KEY_UPARROW},   {0x7D, KEY_DOWNARROW},
    {0x7B, KEY_LEFTARROW}, {0x7C, KEY_RIGHTARROW},
    {0x0D, KEY_UPARROW},   /* W */
    {0x01, KEY_DOWNARROW}, /* S */
    {0x00, KEY_LEFTARROW}, /* A */
    {0x02, KEY_RIGHTARROW},/* D */
    {0x31, ' '},           /* Space = use */
    {0x24, KEY_ENTER},
    {0x35, KEY_ESCAPE},
    {0x30, KEY_TAB},       /* automap */
    {0x3B, KEY_RCTRL},     /* Left Ctrl = fire */
    {0x38, KEY_RSHIFT},    /* Left Shift = run */
    {0x3A, KEY_RALT},      /* Left Option = strafe */
    {0x12, '1'}, {0x13, '2'}, {0x14, '3'}, {0x15, '4'},
    {0x17, '5'}, {0x16, '6'}, {0x1A, '7'}, {0x1C, '8'},
    {0x2B, ','}, {0x2F, '.'},
    {0x7A, KEY_F1},  {0x78, KEY_F2},  {0x63, KEY_F3},  {0x76, KEY_F4},
    {0x60, KEY_F5},  {0x61, KEY_F6},  {0x62, KEY_F7},  {0x64, KEY_F8},
    {0x65, KEY_F9},  {0x6D, KEY_F10}, {0x67, KEY_F11}, {0x6F, KEY_F12},
    {0x1B, KEY_MINUS}, {0x18, KEY_EQUALS},
    {0x71, KEY_PAUSE},
    {0x10, 'y'}, {0x2D, 'n'},
    /* letters for cheat codes (IDDQD, IDKFA, etc.) */
    {0x00, 'a'}, {0x0B, 'b'}, {0x08, 'c'}, {0x02, 'd'},
    {0x0E, 'e'}, {0x03, 'f'}, {0x05, 'g'}, {0x04, 'h'},
    {0x22, 'i'}, {0x26, 'j'}, {0x28, 'k'}, {0x25, 'l'},
    {0x2E, 'm'}, {0x1F, 'o'}, {0x23, 'p'}, {0x0C, 'q'},
    {0x0F, 'r'}, {0x01, 's'}, {0x11, 't'}, {0x20, 'u'},
    {0x09, 'v'}, {0x0D, 'w'}, {0x07, 'x'}, {0x06, 'z'},
    {-1, 0}
};

/* Dither hotkeys — raw Mac key codes, handled before Doom's event loop.
 *
 *  [ (0x21)  game_black  -5      ] (0x1E)  game_black  +5
 *  ; (0x29)  game_white  -5      ' (0x27)  game_white  +5
 *  O (0x1F)  gamma       -0.05   P (0x23)  gamma       +0.05
 *  L (0x25)  toggle no_lighting
 *  K (0x28)  save doom_dither.cfg
 *  ` (0x32)  fog_scale   +2048   \ (0x2A)  fog_scale   -2048
 *  Z (0x06)  solidfloor_gray cycle 0→1→2→3→4→0
 */
static const struct { int macKey; int param; int delta; } kDitherKeys[] = {
    { 0x21, 1, -1 },   /* [  → game_black -5               */
    { 0x1E, 1, +1 },   /* ]  → game_black +5               */
    { 0x29, 2, -1 },   /* ;  → game_white -5               */
    { 0x27, 2, +1 },   /* '  → game_white +5               */
    { 0x1F, 0, -1 },   /* O  → gamma -0.05                 */
    { 0x23, 0, +1 },   /* P  → gamma +0.05                 */
    { 0x25, 3,  0 },   /* L  → toggle lighting             */
    { 0x28, 4,  0 },   /* K  → save config                 */
    { 0x32, 5, +1 },   /* `  → fog_scale +2048             */
    { 0x2A, 5, -1 },   /* \  → fog_scale -2048             */
    { 0x06, 6,  0 },   /* Z  → solidfloor_gray cycle 0-4   */
    { -1, 0, 0 }
};

void I_PollMacInput(void)
{
    /*
     * Read keyboard state directly from hardware via GetKeys().
     * This bypasses the OS event queue entirely, so it is immune to
     * Retro68's console window draining keyDown events during printf.
     * KeyMap is 16 bytes (128 bits); bit K = key code K is currently held.
     */
    static unsigned char prev[16];
    unsigned char        curr[16];
    EventRecord          evt;
    event_t              doom_evt;
    int                  i;

    GetKeys((UInt32 *)curr);

    /* Dither hotkeys: fire once on key-down, never sent to Doom event loop */
    {
        int d;
        for (d = 0; kDitherKeys[d].macKey >= 0; d++) {
            int k       = kDitherKeys[d].macKey;
            int was_dn  = (prev[k >> 3] >> (k & 7)) & 1;
            int is_dn   = (curr[k >> 3] >> (k & 7)) & 1;
            if (is_dn && !was_dn)
                I_AdjustDither(kDitherKeys[d].param, kDitherKeys[d].delta);
        }
    }

    for (i = 0; kKeyTable[i].macKey >= 0; i++) {
        int k        = kKeyTable[i].macKey;
        int was_down = (prev[k >> 3] >> (k & 7)) & 1;
        int is_down  = (curr[k >> 3] >> (k & 7)) & 1;

        if (is_down && !was_down) {
            doom_evt.type  = ev_keydown;
            doom_evt.data1 = kKeyTable[i].doomKey;
            D_PostEvent(&doom_evt);
            if (kKeyTable[i].doomKey >= 'a' && kKeyTable[i].doomKey <= 'z')
                doom_log("KEY: mac=0x%02x doom='%c'(0x%02x)\r",
                         kKeyTable[i].macKey, kKeyTable[i].doomKey, kKeyTable[i].doomKey);
        } else if (!is_down && was_down) {
            doom_evt.type  = ev_keyup;
            doom_evt.data1 = kKeyTable[i].doomKey;
            D_PostEvent(&doom_evt);
        }
    }

    /* Save state for next tick */
    for (i = 0; i < 16; i++)
        prev[i] = curr[i];

    /* Process mouse button events from the event queue.
     * Drain keyboard events too so the queue doesn't fill up. */
    while (GetOSEvent(mDownMask | mUpMask | keyDownMask | keyUpMask | autoKeyMask, &evt)) {
        switch (evt.what) {
            case mouseDown:
                doom_evt.type  = ev_keydown;
                doom_evt.data1 = KEY_RCTRL;
                D_PostEvent(&doom_evt);
                break;
            case mouseUp:
                doom_evt.type  = ev_keyup;
                doom_evt.data1 = KEY_RCTRL;
                D_PostEvent(&doom_evt);
                break;
            default:
                break; /* keyboard events discarded — handled via GetKeys above */
        }
    }
}
