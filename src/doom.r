/* doom.r — Resource definitions for Doom SE/30 */

#include "Retro68APPL.r"

/* SIZE resource: request 48MB preferred, 8MB minimum */
resource 'SIZE' (-1) {
    reserved,
    acceptSuspendResumeEvents,
    reserved,
    canBackground,
    needsActivateOnFGSwitch,
    backgroundAndForeground,
    dontGetFrontClicks,
    ignoreChildDiedEvents,
    is32BitCompatible,
    notHighLevelEventAware,
    onlyLocalHLEvents,
    notStationeryAware,
    dontUseTextEditServices,
    reserved,
    reserved,
    reserved,
    48 * 1024 * 1024,    /* preferred: 48 MB */
    8  * 1024 * 1024     /* minimum:   8 MB */
};
