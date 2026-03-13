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

#include "doomdef.h"
#include "m_argv.h"
#include <stdlib.h>
#include <setjmp.h>
extern void D_DoomMain(void);
extern void I_OpenLog(void);

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

// Generate random interval in ticks (between 0.1 and 2.0 seconds)
// 60 ticks = 1 second
// 0.1 sec = 6 ticks, 2.0 sec = 120 ticks
unsigned long GetRandomInterval(void)
{
// Random number between 6 and 120 ticks
return 6 + (Random() & 0x7FFF) % 115;  // 6 to 120 inclusive
}

static char *mac_argv[] = { "DoomSE30" };

int main(void)
{
    DialogPtr       dialog;
    short           itemHit;
    short           currentPictID = kFirstPICT;
    unsigned long   lastUpdateTick;
    unsigned long   currentTick;
    unsigned long   nextUpdateInterval;
    Boolean         gotEvent;
    WindowPtr       bg_window;   /* fullscreen black background window */

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

    FlushEvents(everyEvent, 0); //for dialog box

    /* Open debug log and set CWD to the app folder as a baseline */
    SetCWDToAppFolder();
    I_OpenLog();

    doom_log("=== Doom SE/30 starting ===\r");

        // Load dialog from resource file
    dialog = GetNewDialog(rDLOG,            // resource ID
                         nil,                // storage (nil = allocate)
                         (WindowPtr)-1);     // in front of all windows

    if (dialog == nil)
    {
        ExitToShell();  // Quit if dialog resource not found
    }

    // Center the dialog before showing it
    CenterDialog(dialog);
    // Show the dialog
    ShowWindow(dialog);
    SetPort(dialog);
    // Seed random number generator with current time
    GetDateTime((unsigned long *)&currentTick);
    srand(currentTick);
    // Initialize timing
    lastUpdateTick = TickCount();
    nextUpdateInterval = GetRandomInterval();

    // Dialog event loop
    Boolean done = false;
    while (!done)
    {
        EventRecord event;

        // Use WaitNextEvent with short sleep
        gotEvent = WaitNextEvent(everyEvent, &event, 15, nil);

        // Check if it's time to update
        currentTick = TickCount();

        if (currentTick - lastUpdateTick >= nextUpdateInterval)
        {
            // Increment to next PICT
            currentPictID++;

            // Wrap around if we exceed the last PICT
            if (currentPictID > kLastPICT)
                currentPictID = kFirstPICT;

            // Update the picture display
            UpdatePictureItem(dialog, kPictureItem, currentPictID);

            // Reset timer with new random interval
            lastUpdateTick = currentTick;
            nextUpdateInterval = GetRandomInterval();
        }

        // Handle dialog events if we got one
        if (gotEvent && IsDialogEvent(&event))
        {
            if (DialogSelect(&event, &dialog, &itemHit))
            {
                switch (itemHit)
                {
                    case kButtonOK:
                        done = true;
                        break;
                }
            }
        }
    }

    DisposeDialog(dialog);

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
