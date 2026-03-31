/* doom.r — Resource definitions for Doom SE/30 */

#include "Retro68APPL.r"

/* SIZE resource: request 16MB preferred, 10MB minimum */
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
    16 * 1024 * 1024,    /* preferred: 16 MB */
    10 * 1024 * 1024     /* minimum:  10 MB */
};
