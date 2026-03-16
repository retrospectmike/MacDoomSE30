/*
 * i_main_mac.c — Mac entry point for Doom SE/30
 *
 * Initializes the Mac Toolbox, sets the working directory to the app folder,
 * shows a loading splash screen, then hands off to D_DoomMain.
 *
 * IdentifyVersion() in d_main.c auto-detects doom2.wad / doom.wad / doom1.wad
 * from the current directory — no WAD picker needed.
 */

#include <QuickDraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <Events.h>
#include <Memory.h>
#include <Processes.h>
#include <Files.h>
#include <ToolUtils.h>
#include <Dialogs.h>

#include "doomdef.h"
#include "i_system.h"
#include "m_argv.h"
#include <stdlib.h>
#include <setjmp.h>
extern void D_DoomMain(void);
extern void I_OpenLog(void);

/* Settings dialog globals — read/written by ShowSettingsDialog */
extern void M_LoadDefaults(void);
extern void M_SaveDefaults(void);
extern int  opt_halfline;
extern int  opt_affine_texcol;
extern int  opt_solidfloor;
extern int  opt_scale2x;
extern int  fog_scale;
extern int  solidfloor_gray;
extern int  detailLevel;
extern int  no_lighting;
extern int  dither_gamma_x100;
extern int  dither_gblack;
extern int  dither_gwhite;

jmp_buf doom_quit_jmp;   /* longjmp target — I_Quit() jumps here to return to main() */

/*
 * Set the Mac default volume+directory to a given FSSpec's parent folder.
 * Retro68's _open_r uses HOpenDF(0, 0, name) — vRefNum=0/dirID=0 means
 * "use the current default directory." PBHSetVolSync sets that here.
 */
static void SetDefaultDirToFolder(short vRefNum, long dirID)
{
    WDPBRec wdpb;
    wdpb.ioNamePtr = NULL;
    wdpb.ioVRefNum = vRefNum;
    wdpb.ioWDDirID = dirID;
    PBHSetVolSync(&wdpb);
}

/*
 * SetCWDToAppFolder — fallback if user cancels the WAD dialog.
 * Uses the Process Manager to find the app's own folder.
 */
static void SetCWDToAppFolder(void)
{
    ProcessSerialNumber psn;
    ProcessInfoRec      info;
    FSSpec              appSpec;

    psn.highLongOfPSN = 0;
    psn.lowLongOfPSN  = kCurrentProcess;

    info.processInfoLength = sizeof(ProcessInfoRec);
    info.processName       = NULL;
    info.processAppSpec    = &appSpec;

    if (GetProcessInformation(&psn, &info) == noErr)
        SetDefaultDirToFolder(appSpec.vRefNum, appSpec.parID);
}

void CenterDialog(DialogPtr dialog)
{
    Rect    dialogRect;
    Rect    screenRect;
    short   dialogWidth, dialogHeight;
    short   screenWidth, screenHeight;
    short   newLeft, newTop;

    // Get dialog bounds
    dialogRect = dialog->portRect;
    dialogWidth = dialogRect.right - dialogRect.left;
    dialogHeight = dialogRect.bottom - dialogRect.top;

    // Get screen bounds (main screen)
    screenRect = qd.screenBits.bounds;
    screenWidth = screenRect.right - screenRect.left;
    screenHeight = screenRect.bottom - screenRect.top;

    // Calculate centered position
    newLeft = (screenWidth - dialogWidth) / 2;
    newTop = (screenHeight - dialogHeight) / 2;

    // Adjust for menu bar (move down a bit)
    newTop += 20;

    // Move the dialog
    MoveWindow((WindowPtr)dialog, newLeft, newTop, false);
}

#define rDLOG 128           // Resource ID for our startup dialog
#define kButtonOK 1         // OK button
#define kPictureItem 3      // Picture item
#define kFirstPICT 128      // First PICT resource ID
#define kLastPICT 133       // Last PICT resource ID (adjust as needed)

void UpdatePictureItem(DialogPtr dialog, short itemNum, short pictID)
{
    Handle      itemHandle;
    short       itemType;
    Rect        itemRect;
    PicHandle   picture;

    // Get the dialog item
    GetDialogItem(dialog, itemNum, &itemType, &itemHandle, &itemRect);

    // Load the new PICT resource
    picture = GetPicture(pictID);

    if (picture != nil)
    {
        // Update the item's handle to point to the new picture
        SetDialogItem(dialog, itemNum, itemType, (Handle)picture, &itemRect);

        // Force redraw of the item
        InvalRect(&itemRect);
    }
}

/* ---- Settings dialog helpers -------------------------------------------- */
static void dlg_disable_check(DialogPtr dlg, short item)
{
    Handle h; short type; Rect r;
    GetDialogItem(dlg, item, &type, &h, &r);
    HiliteControl((ControlHandle)h, 255);
}

static void dlg_set_check(DialogPtr dlg, short item, int val)
{
    Handle h; short type; Rect r;
    GetDialogItem(dlg, item, &type, &h, &r);
    SetControlValue((ControlHandle)h, val ? 1 : 0);
}

static int dlg_get_check(DialogPtr dlg, short item)
{
    Handle h; short type; Rect r;
    GetDialogItem(dlg, item, &type, &h, &r);
    return GetControlValue((ControlHandle)h);
}

static void dlg_toggle_check(DialogPtr dlg, short item)
{
    Handle h; short type; Rect r;
    GetDialogItem(dlg, item, &type, &h, &r);
    SetControlValue((ControlHandle)h, !GetControlValue((ControlHandle)h));
}

static void dlg_set_popup(DialogPtr dlg, short item, int menuItem)
{
    Handle h; short type; Rect r;
    GetDialogItem(dlg, item, &type, &h, &r);
    SetControlValue((ControlHandle)h, menuItem);
}

static int dlg_get_popup(DialogPtr dlg, short item)
{
    Handle h; short type; Rect r;
    GetDialogItem(dlg, item, &type, &h, &r);
    return GetControlValue((ControlHandle)h);
}

static void dlg_set_int(DialogPtr dlg, short item, int val)
{
    Handle h; short type; Rect r; Str255 s;
    GetDialogItem(dlg, item, &type, &h, &r);
    NumToString((long)val, s);
    SetDialogItemText(h, s);
}

static int dlg_get_int(DialogPtr dlg, short item)
{
    Handle h; short type; Rect r; Str255 s; long val;
    GetDialogItem(dlg, item, &type, &h, &r);
    GetDialogItemText(h, s);
    StringToNum(s, &val);
    return (int)val;
}

/* Hardcoded factory defaults — these never change 
   and are loaded when "Restore Defaults" is 
   pressed in the MacDoom options screen.         */
static void apply_factory_defaults(void)
{
    opt_halfline      = 1;
    opt_affine_texcol = 1;
    opt_solidfloor    = 1;
    solidfloor_gray   = 4;
    fog_scale         = 10240;
    detailLevel       = 2;
    opt_scale2x       = 0;
    no_lighting       = 0;
    dither_gamma_x100 = 60;
    dither_gblack     = 5;
    dither_gwhite     = 195;
}

static void populate_settings(DialogPtr dlg)
{
    dlg_set_check(dlg, 15, opt_halfline);
    dlg_set_check(dlg, 16, opt_affine_texcol);
    dlg_set_check(dlg, 17, opt_solidfloor);
    dlg_set_check(dlg, 18, opt_scale2x);
    dlg_set_check(dlg, 19, no_lighting);
    dlg_set_int(dlg, 20, dither_gamma_x100);
    dlg_set_int(dlg, 21, dither_gblack);
    dlg_set_int(dlg, 22, dither_gwhite);
    dlg_set_int(dlg, 28, fog_scale);
    dlg_set_popup(dlg, 29, solidfloor_gray + 1);  /* Shade: 1=White..5=Black */
    dlg_set_popup(dlg, 30, detailLevel + 1);      /* Detail: 1=High, 2=Low, 3=Quad */
}

static void read_settings(DialogPtr dlg)
{
    int v;
    opt_halfline      = dlg_get_check(dlg, 15);
    opt_affine_texcol = dlg_get_check(dlg, 16);
    opt_solidfloor    = dlg_get_check(dlg, 17);
    opt_scale2x       = dlg_get_check(dlg, 18);
    no_lighting       = dlg_get_check(dlg, 19);
    v = dlg_get_int(dlg, 20);
    dither_gamma_x100 = (v < 5) ? 5 : (v > 300) ? 300 : v;
    v = dlg_get_int(dlg, 21);
    dither_gblack = (v < 0) ? 0 : (v > 245) ? 245 : v;
    v = dlg_get_int(dlg, 22);
    dither_gwhite = (v < 10) ? 10 : (v > 255) ? 255 : v;
    if (dither_gwhite < dither_gblack + 10) dither_gwhite = dither_gblack + 10;
    v = dlg_get_int(dlg, 28);
    fog_scale = (v < 0) ? 0 : (v > 65536) ? 65536 : v;
    solidfloor_gray = dlg_get_popup(dlg, 29) - 1;  /* 1-based → 0-based */
    detailLevel = dlg_get_popup(dlg, 30) - 1;
}

static void ShowSettingsDialog(void)
{
    DialogPtr   dlg;
    short       itemHit;
    Boolean     done = false;

    dlg = GetNewDialog(129, nil, (WindowPtr)-1);
    if (!dlg) return;
    CenterDialog(dlg);
    ShowWindow(dlg);
    SetPort(dlg);
    SetDialogDefaultItem(dlg, 1);
    SetDialogCancelItem(dlg, 2);
    populate_settings(dlg);

    while (!done) {
        ModalDialog(nil, &itemHit);
        switch (itemHit) {
            case 1:  /* OK — commit and save */
                read_settings(dlg);
                M_SaveDefaults();
                done = true;
                break;
            case 2:  /* Cancel */
                done = true;
                break;
            case 3:  /* Restore Defaults */
                apply_factory_defaults();
                populate_settings(dlg);
                break;
            case 15: case 16: case 17: case 18: case 19:  /* checkboxes */
                dlg_toggle_check(dlg, itemHit);
                break;
        }
    }
    DisposeDialog(dlg);
}

/* ---- Splash PICT animation ----------------------------------------------- */

// Random interval in ticks: 0.1–2.0 seconds (6–120 ticks)
static unsigned long GetRandomInterval(void)
{
    return 6 + (Random() & 0x7FFF) % 115;
}

/* Animation state — written before ModalDialog, read by filter proc */
static short         s_pictID;
static unsigned long s_lastTick;
static unsigned long s_nextInterval;
static ProcPtr s_stdFilter = nil;
typedef pascal Boolean (*StdFilterProcPtr)(DialogPtr, EventRecord *, short *);

static pascal Boolean SplashFilterProc(DialogPtr dlg, EventRecord *event, short *itemHit)
{
    /* Escape → quit sentinel */
    if (event->what == keyDown) {
        char key = event->message & charCodeMask;
        if (key == 0x1B) {
            *itemHit = -1;
            return true;
        }
    }

    /* Delegate to standard filter for Return/Enter, default ring */
    if (s_stdFilter && ((StdFilterProcPtr)s_stdFilter)(dlg, event, itemHit))
        return true;

    /* PICT animation on null events */
    if (event->what == nullEvent) {
        unsigned long now = TickCount();
        if (now - s_lastTick >= s_nextInterval) {
            if (++s_pictID > kLastPICT) s_pictID = kFirstPICT;
            UpdatePictureItem(dlg, kPictureItem, s_pictID);
            s_lastTick = now;
            s_nextInterval = GetRandomInterval();
        }
    }
    return false;
}

static char *mac_argv[] = { "DoomSE30" };
static WindowPtr bg_window = nil;  /* fullscreen black background window */

void I_NoWadAlert(void)
{
    EventRecord evt;
    ConstStr255Param line1 = "\pNo WAD file found.";
    ConstStr255Param line2 = "\pPlace a WAD file next to this application.";
    ConstStr255Param line3 = "\pClick or press any key to exit.";
    int cx = (qd.screenBits.bounds.right - qd.screenBits.bounds.left) / 2;
    int cy = (qd.screenBits.bounds.bottom - qd.screenBits.bounds.top) / 2;

    if (!bg_window) return;
    SetPort(bg_window);
    /* Clear loading text by repainting black */
    {
        Rect r = bg_window->portRect;
        FillRect(&r, &qd.black);
    }
    TextFont(systemFont);
    TextSize(12);
    TextFace(bold);
    TextMode(srcBic);
    MoveTo(cx - StringWidth(line1) / 2, cy - 14);
    DrawString(line1);
    MoveTo(cx - StringWidth(line2) / 2, cy);
    DrawString(line2);
    TextFace(normal);
    MoveTo(cx - StringWidth(line3) / 2, cy + 18);
    DrawString(line3);

    while (!GetNextEvent(mDownMask | keyDownMask, &evt))
        ;
}

int main(void)
{
    DialogPtr       splashdialog;

    /* Standard Mac Toolbox init — MUST come before any QuickDraw/printf */
    MaxApplZone();
    MoreMasters();
    MoreMasters();
    MoreMasters();
    MoreMasters();

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(nil);
    InitCursor();

    FlushEvents(everyEvent, 0); //for splashdialog box

    /* Open debug log and set CWD to the app folder as a baseline */
    SetCWDToAppFolder();
    I_OpenLog();

    doom_log("=== Doom SE/30 starting ===\r");

    /* Load doom.cfg now so settings dialog reflects saved values.
     * basedefault must be set first — IdentifyVersion() normally does this
     * inside D_DoomMain, but we need it earlier for the settings dialog.
     * myargc/myargv must be set first so M_LoadDefaults can parse -config. */
    extern char basedefault[1024];
    strcpy(basedefault, "doom.cfg");
    myargc = 1;
    myargv = mac_argv;
    M_LoadDefaults();

        // Load dialog from resource file
    splashdialog = GetNewDialog(rDLOG,            // resource ID
                         nil,                // storage (nil = allocate)
                         (WindowPtr)-1);     // in front of all windows

    if (splashdialog == nil)
    {
        ExitToShell();  // Quit if dialog resource not found
    }
    CenterDialog(splashdialog);
    ShowWindow(splashdialog);
    SetPort(splashdialog);
    SetDialogDefaultItem(splashdialog, kButtonOK);

    /* Seed RNG and init animation state */
    {
        unsigned long now;
        GetDateTime(&now);
        srand(now);
    }
    s_pictID      = kFirstPICT;
    s_lastTick    = TickCount();
    s_nextInterval = GetRandomInterval();
    GetStdFilterProc(&s_stdFilter);

    {
        Boolean done = false;
        while (!done) {
            short itemHit;
            ModalDialog((ModalFilterUPP)SplashFilterProc, &itemHit);
            switch (itemHit) {
                case -1:  /* Escape — quit */
                    DisposeDialog(splashdialog);
                    ExitToShell();
                    break;
                case kButtonOK:
                    done = true;
                    break;
                case 4:  /* Options... */
                    ShowSettingsDialog();
                    SelectWindow(splashdialog);
                    SetPort(splashdialog);
                    break;
            }
        }
    }

    DisposeDialog(splashdialog);

    /* Hide menu bar: zero the MBarHeight low-memory global, then explicitly
     * paint the top rows black via a direct screen port — DrawMenuBar() alone
     * doesn't erase existing menu bar pixels, and the WM clips windows to y>=20. */
    *(short *)0x0BAA = 0;
    DrawMenuBar();
    {
        GrafPort  screenPort;
        Rect      menuBarRect;
        OpenPort(&screenPort);
        SetPort(&screenPort);
        menuBarRect.top    = 0;
        menuBarRect.left   = 0;
        menuBarRect.bottom = 20;
        menuBarRect.right  = qd.screenBits.bounds.right;
        FillRect(&menuBarRect, &qd.black);
        ClosePort(&screenPort);
    }

    /* Open a fullscreen black window to give the game a clean black surround
     * and to ensure the Window Manager repaints the desktop cleanly on exit. */
    
    bg_window = NewWindow(nil, &qd.screenBits.bounds, "\p", true,
                          plainDBox, (WindowPtr)-1, false, 0);
    if (bg_window != nil)
    {
        Rect local_r;
        SetPort(bg_window);
        local_r = bg_window->portRect;
        FillRect(&local_r, &qd.black);
        TextFont(systemFont);
        TextSize(12);
        TextFace(bold);
        TextMode(srcBic);  /* white text on black */
        {
            ConstStr255Param line1 = "\pLOADING WAD FILE...";
            ConstStr255Param line2 = "\pThis takes about 10 seconds...";
            MoveTo((qd.screenBits.bounds.right - qd.screenBits.bounds.left - StringWidth(line1)) / 2, 
                    (qd.screenBits.bounds.bottom - qd.screenBits.bounds.top-12)/2);
            DrawString(line1);
            MoveTo((qd.screenBits.bounds.right - qd.screenBits.bounds.left - StringWidth(line2)) / 2,
            (qd.screenBits.bounds.bottom - qd.screenBits.bounds.top+12)/2);
            DrawString(line2);
        }
        TextFace(normal);
    }

    /* Set up Doom's command-line argument globals */
    myargc = 1;
    myargv = mac_argv;

    /* Doom's real entry point — setjmp here so I_Quit()'s longjmp lands back */
    if (setjmp(doom_quit_jmp) == 0)
        D_DoomMain();

    /* I_Quit longjmp'd here (or D_DoomMain returned) — tear down and exit cleanly */
    /* Restore menu bar before handing back to Finder */
    *(short *)0x0BAA = 20;
    DrawMenuBar();
    if (bg_window != nil)
        DisposeWindow(bg_window);
    ExitToShell();
    return 0;
}
