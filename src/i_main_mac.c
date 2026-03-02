/*
 * i_main_mac.c — Mac entry point for Doom SE/30
 *
 * Initializes the Mac Toolbox, shows a WAD file picker dialog,
 * sets the working directory to the WAD's folder, then hands off
 * to D_DoomMain.
 */

#include <QuickDraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <Events.h>
#include <Memory.h>
#include <Processes.h>
#include <Files.h>
#include <StandardFile.h>

#include "doomdef.h"
#include "m_argv.h"

extern void D_DoomMain(void);
extern void I_OpenLog(void);

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

/*
 * PickWADFile — show a standard Open dialog so the user can locate
 * their DOOM WAD file. Sets the default directory to the WAD's folder
 * so that Doom's IdentifyVersion() can find it by name.
 *
 * Returns true if a file was selected.
 */
static int PickWADFile(void)
{
    StandardFileReply reply;
    SFTypeList        typeList;   /* unused — show all files */

    /* Show "Open" dialog — no type filter, show all files */
    StandardGetFile(NULL, -1, typeList, &reply);

    if (!reply.sfGood)
        return 0;   /* user cancelled */

    /* Set default directory to the folder containing the chosen WAD */
    SetDefaultDirToFolder(reply.sfFile.vRefNum, reply.sfFile.parID);

    /* Log the selection */
    doom_log("WAD picker: selected '%.*s' (vRefNum=%d parID=%ld)\n",
             (int)reply.sfFile.name[0], &reply.sfFile.name[1],
             (int)reply.sfFile.vRefNum, reply.sfFile.parID);

    return 1;
}

static char *mac_argv[] = { "DoomSE30" };

int main(void)
{
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

    /* Open debug log and set CWD to the app folder as a baseline */
    SetCWDToAppFolder();
    I_OpenLog();

    doom_log("=== Doom SE/30 starting ===\n");

    /* Ask the user to pick their WAD file.
     * This also sets the default directory so IdentifyVersion() finds it. */
    if (!PickWADFile()) {
        /* User cancelled — fall back to app folder (doom1.wad must be there) */
        doom_log("WAD picker cancelled — trying app folder\n");
    }

    /* Open the console window NOW with a loading message so the user knows
     * the app is alive during the ~15 second WAD load + init sequence.
     * The first printf() call opens the Retro68 ConsoleWindow. */
    printf("Doom SE/30: loading WAD file, please wait...\n");

    /* Set up Doom's command-line argument globals */
    myargc = 1;
    myargv = mac_argv;

    /* Doom's real entry point */
    D_DoomMain();

    /* Should never return */
    return 0;
}
