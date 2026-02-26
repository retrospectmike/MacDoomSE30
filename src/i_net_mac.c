/*
 * i_net_mac.c — Network stubs for Doom SE/30
 *
 * No multiplayer support. Single-player only.
 */

#include "doomdef.h"
#include "doomstat.h"
#include "d_event.h"
#include "d_net.h"
#include "i_net.h"
#include "i_system.h"
#include "m_argv.h"

#include <stdlib.h>
#include <string.h>

void I_InitNetwork(void)
{
    /* Single player only */
    doomcom = malloc(sizeof(*doomcom));
    memset(doomcom, 0, sizeof(*doomcom));

    doomcom->ticdup = 1;
    doomcom->extratics = 0;
    doomcom->id = DOOMCOM_ID;
    doomcom->numplayers = 1;
    doomcom->numnodes = 1;
    doomcom->deathmatch = false;
    doomcom->consoleplayer = 0;
}

void I_NetCmd(void)
{
    /* No-op for single player */
}
