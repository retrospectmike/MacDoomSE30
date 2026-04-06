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
// $Log:$
//
// DESCRIPTION:
//	Rendering main loop and setup functions,
//	 utility functions (BSP, geometry, trigonometry).
//	See tables.c, too.
//
//-----------------------------------------------------------------------------


static const char rcsid[] = "$Id: r_main.c,v 1.5 1997/02/03 22:45:12 b1 Exp $";



#include <stdlib.h>
#include <math.h>
#include <string.h>


#include "doomdef.h"
#include "doomstat.h"
#include "d_net.h"
#include "i_system.h"   /* I_GetMacTick */

/* Render sub-profiling accumulators (60Hz ticks, reset by d_main.c). */
long prof_r_setup   = 0;   /* memset + R_SetupFrame + R_ClearXxx */
long prof_r_bsp     = 0;   /* R_RenderBSPNode */
long prof_r_planes  = 0;   /* R_DrawPlanes */
long prof_r_masked  = 0;   /* R_DrawMasked */

#include "m_bbox.h"

#include "r_local.h"
#include "r_sky.h"
#include "v_video.h"





// Fineangles in the SCREENWIDTH wide window.
#define FIELDOFVIEW		2048	



int			viewangleoffset;

// increment every time a check is made
int			validcount = 1;		


lighttable_t*		fixedcolormap;
extern lighttable_t**	walllights;

int			centerx;
int			centery;

fixed_t			centerxfrac;
fixed_t			centeryfrac;
fixed_t			projection;

// just for profiling purposes
int			framecount;	

int			sscount;
int			linecount;
int			loopcount;

fixed_t			viewx;
fixed_t			viewy;
fixed_t			viewz;

angle_t			viewangle;

fixed_t			viewcos;
fixed_t			viewsin;

player_t*		viewplayer;

// 0 = high, 1 = low
int			detailshift;	

//
// precalculated math tables
//
angle_t			clipangle;

// The viewangletox[viewangle + FINEANGLES/4] lookup
// maps the visible view angles to screen X coordinates,
// flattening the arc to a flat projection plane.
// There will be many angles mapped to the same X. 
int			viewangletox[FINEANGLES/2];

// The xtoviewangleangle[] table maps a screen pixel
// to the lowest viewangle that maps back to x ranges
// from clipangle to -clipangle.
angle_t			xtoviewangle[SCREENWIDTH+1];


// UNUSED.
// The finetangentgent[angle+FINEANGLES/4] table
// holds the fixed_t tangent values for view angles,
// ranging from MININT to 0 to MAXINT.
// fixed_t		finetangent[FINEANGLES/2];

// fixed_t		finesine[5*FINEANGLES/4];
fixed_t*		finecosine = &finesine[FINEANGLES/4];


lighttable_t*		scalelight[LIGHTLEVELS][MAXLIGHTSCALE];
lighttable_t*		scalelightfixed[MAXLIGHTSCALE];
lighttable_t*		zlight[LIGHTLEVELS][MAXLIGHTZ];

// bumped light from gun blasts
int			extralight;			



void (*colfunc) (void);
void (*basecolfunc) (void);
void (*fuzzcolfunc) (void);
void (*transcolfunc) (void);
void (*spanfunc) (void);



//
// R_AddPointToBox
// Expand a given bbox
// so that it encloses a given point.
//
void
R_AddPointToBox
( int		x,
  int		y,
  fixed_t*	box )
{
    if (x< box[BOXLEFT])
	box[BOXLEFT] = x;
    if (x> box[BOXRIGHT])
	box[BOXRIGHT] = x;
    if (y< box[BOXBOTTOM])
	box[BOXBOTTOM] = y;
    if (y> box[BOXTOP])
	box[BOXTOP] = y;
}


// R_PointOnSide — moved to r_main.h as static inline (Phase 3B)

int
R_PointOnSegSide
( fixed_t	x,
  fixed_t	y,
  seg_t*	line )
{
    fixed_t	lx;
    fixed_t	ly;
    fixed_t	ldx;
    fixed_t	ldy;
    fixed_t	dx;
    fixed_t	dy;
    fixed_t	left;
    fixed_t	right;
	
    lx = line->v1->x;
    ly = line->v1->y;
	
    ldx = line->v2->x - lx;
    ldy = line->v2->y - ly;
	
    if (!ldx)
    {
	if (x <= lx)
	    return ldy > 0;
	
	return ldy < 0;
    }
    if (!ldy)
    {
	if (y <= ly)
	    return ldx < 0;
	
	return ldx > 0;
    }
	
    dx = (x - lx);
    dy = (y - ly);
	
    // Try to quickly decide by looking at sign bits.
    if ( (ldy ^ ldx ^ dx ^ dy)&0x80000000 )
    {
	if  ( (ldy ^ dx) & 0x80000000 )
	{
	    // (left is negative)
	    return 1;
	}
	return 0;
    }

    left = FixedMul ( ldy>>FRACBITS , dx );
    right = FixedMul ( dy , ldx>>FRACBITS );
	
    if (right < left)
    {
	// front side
	return 0;
    }
    // back side
    return 1;			
}


//
// R_PointToAngle
// To get a global angle from cartesian coordinates,
//  the coordinates are flipped until they are in
//  the first octant of the coordinate system, then
//  the y (<=x) is scaled and divided by x to get a
//  tangent (slope) value which is looked up in the
//  tantoangle[] table.

//




angle_t
R_PointToAngle
( fixed_t	x,
  fixed_t	y )
{	
    x -= viewx;
    y -= viewy;
    
    if ( (!x) && (!y) )
	return 0;

    if (x>= 0)
    {
	// x >=0
	if (y>= 0)
	{
	    // y>= 0

	    if (x>y)
	    {
		// octant 0
		return tantoangle[ SlopeDiv(y,x)];
	    }
	    else
	    {
		// octant 1
		return ANG90-1-tantoangle[ SlopeDiv(x,y)];
	    }
	}
	else
	{
	    // y<0
	    y = -y;

	    if (x>y)
	    {
		// octant 8
		return -tantoangle[SlopeDiv(y,x)];
	    }
	    else
	    {
		// octant 7
		return ANG270+tantoangle[ SlopeDiv(x,y)];
	    }
	}
    }
    else
    {
	// x<0
	x = -x;

	if (y>= 0)
	{
	    // y>= 0
	    if (x>y)
	    {
		// octant 3
		return ANG180-1-tantoangle[ SlopeDiv(y,x)];
	    }
	    else
	    {
		// octant 2
		return ANG90+ tantoangle[ SlopeDiv(x,y)];
	    }
	}
	else
	{
	    // y<0
	    y = -y;

	    if (x>y)
	    {
		// octant 4
		return ANG180+tantoangle[ SlopeDiv(y,x)];
	    }
	    else
	    {
		 // octant 5
		return ANG270-1-tantoangle[ SlopeDiv(x,y)];
	    }
	}
    }
    return 0;
}


angle_t
R_PointToAngle2
( fixed_t	x1,
  fixed_t	y1,
  fixed_t	x2,
  fixed_t	y2 )
{	
    viewx = x1;
    viewy = y1;
    
    return R_PointToAngle (x2, y2);
}


fixed_t
R_PointToDist
( fixed_t	x,
  fixed_t	y )
{
    int		angle;
    fixed_t	dx;
    fixed_t	dy;
    fixed_t	temp;
    fixed_t	dist;
	
    dx = abs(x - viewx);
    dy = abs(y - viewy);
	
    if (dy>dx)
    {
	temp = dx;
	dx = dy;
	dy = temp;
    }
	
    angle = (tantoangle[ FixedDiv(dy,dx)>>DBITS ]+ANG90) >> ANGLETOFINESHIFT;

    // use as cosine
    dist = FixedDiv (dx, finesine[angle] );	
	
    return dist;
}




//
// R_InitPointToAngle
//
void R_InitPointToAngle (void)
{
    // UNUSED - now getting from tables.c
#if 0
    int	i;
    long	t;
    float	f;
//
// slope (tangent) to angle lookup
//
    for (i=0 ; i<=SLOPERANGE ; i++)
    {
	f = atan( (float)i/SLOPERANGE )/(3.141592657*2);
	t = 0xffffffff*f;
	tantoangle[i] = t;
    }
#endif
}


/* R_ScaleFromGlobalAngle removed — inlined at call site in r_segs.c (R_StoreWallRange).
 * Inlining enables short-seg skip (saves 3 divisions/seg for spans <= SCALE_SKIP_THRESH)
 * and eliminates function call overhead (~40 cycles/seg). */



//
// R_InitTables
//
void R_InitTables (void)
{
    // UNUSED: now getting from tables.c
#if 0
    int		i;
    float	a;
    float	fv;
    int		t;
    
    // viewangle tangent table
    for (i=0 ; i<FINEANGLES/2 ; i++)
    {
	a = (i-FINEANGLES/4+0.5)*PI*2/FINEANGLES;
	fv = FRACUNIT*tan (a);
	t = fv;
	finetangent[i] = t;
    }
    
    // finesine table
    for (i=0 ; i<5*FINEANGLES/4 ; i++)
    {
	// OPTIMIZE: mirror...
	a = (i+0.5)*PI*2/FINEANGLES;
	t = FRACUNIT*sin (a);
	finesine[i] = t;
    }
#endif

}



//
// R_InitTextureMapping
//
void R_InitTextureMapping (void)
{
    int			i;
    int			x;
    int			t;
    fixed_t		focallength;
    
    // Use tangent table to generate viewangletox:
    //  viewangletox will give the next greatest x
    //  after the view angle.
    //
    // Calc focallength
    //  so FIELDOFVIEW angles covers SCREENWIDTH.
    focallength = FixedDiv (centerxfrac,
			    finetangent[FINEANGLES/4+FIELDOFVIEW/2] );
	
    for (i=0 ; i<FINEANGLES/2 ; i++)
    {
	if (finetangent[i] > FRACUNIT*2)
	    t = -1;
	else if (finetangent[i] < -FRACUNIT*2)
	    t = viewwidth+1;
	else
	{
	    t = FixedMul (finetangent[i], focallength);
	    t = (centerxfrac - t+FRACUNIT-1)>>FRACBITS;

	    if (t < -1)
		t = -1;
	    else if (t>viewwidth+1)
		t = viewwidth+1;
	}
	viewangletox[i] = t;
    }
    
    // Scan viewangletox[] to generate xtoviewangle[]:
    //  xtoviewangle will give the smallest view angle
    //  that maps to x.	
    for (x=0;x<=viewwidth;x++)
    {
	i = 0;
	while (viewangletox[i]>x)
	    i++;
	xtoviewangle[x] = (i<<ANGLETOFINESHIFT)-ANG90;
    }
    
    // Take out the fencepost cases from viewangletox.
    for (i=0 ; i<FINEANGLES/2 ; i++)
    {
	t = FixedMul (finetangent[i], focallength);
	t = centerx - t;
	
	if (viewangletox[i] == -1)
	    viewangletox[i] = 0;
	else if (viewangletox[i] == viewwidth+1)
	    viewangletox[i]  = viewwidth;
    }
	
    clipangle = xtoviewangle[0];
}



//
// R_InitLightTables
// Only inits the zlight table,
//  because the scalelight table changes with view size.
//
#define DISTMAP		2

void R_InitLightTables (void)
{
    int		i;
    int		j;
    int		level;
    int		startmap; 	
    int		scale;
    
    // Calculate the light levels to use
    //  for each level / distance combination.
    for (i=0 ; i< LIGHTLEVELS ; i++)
    {
	startmap = ((LIGHTLEVELS-1-i)*2)*NUMCOLORMAPS/LIGHTLEVELS;
	for (j=0 ; j<MAXLIGHTZ ; j++)
	{
	    scale = FixedDiv ((SCREENWIDTH/2*FRACUNIT), (j+1)<<LIGHTZSHIFT);
	    scale >>= LIGHTSCALESHIFT;
	    level = startmap - scale/DISTMAP;
	    
	    if (level < 0)
		level = 0;

	    if (level >= NUMCOLORMAPS)
		level = NUMCOLORMAPS-1;

	    zlight[i][j] = colormaps + level*256;
	}
    }
}



//
// R_SetViewSize
// Do not really change anything here,
//  because it might be in the middle of a refresh.
// The change will take effect next refresh.
//
boolean		setsizeneeded;
int		setblocks;
int		setdetail;


void
R_SetViewSize
( int		blocks,
  int		detail )
{
    setsizeneeded = true;
    setblocks = blocks;
    setdetail = detail;
}


//
// R_ExecuteSetViewSize
//
void R_ExecuteSetViewSize (void)
{
    fixed_t	cosadj;
    fixed_t	dy;
    int		i;
    int		j;
    int		level;
    int		startmap;
    extern int  opt_scale2x;

    setsizeneeded = false;

    /* 2x pixel-scale mode: blocks=9+ would expand to >512px at 2×.
     * Clamp to blocks=8 (scaledviewwidth=256 → 512px at 2×, exact fill). */
    if (opt_scale2x && setblocks > 8)
	setblocks = 8;

    if (setblocks == 11)
    {
	scaledviewwidth = SCREENWIDTH;
	viewheight = SCREENHEIGHT;
    }
    else
    {
	scaledviewwidth = setblocks*32;
	viewheight = (setblocks*168/10)&~7;
    }
    
    detailshift = setdetail;
    viewwidth = scaledviewwidth>>detailshift;
	
    centery = viewheight/2;
    centerx = viewwidth/2;
    centerxfrac = centerx<<FRACBITS;
    centeryfrac = centery<<FRACBITS;
    projection = centerxfrac;

    /* Use 8-bit renderers when the menu is active (avoids flicker from
     * partial direct-draw frames) OR when the display is color/grayscale
     * (screens[0] is the entire frame; I_FinishUpdate memcpys it to the
     * hardware framebuffer without any 1-bit conversion). */
    {
	extern boolean menuactive;
	extern int     g_color_depth;
	if (menuactive || g_color_depth >= 8)
	{
	    /* 8-bit path: renders to screens[0]; I_FinishUpdate does full blit */
	    if (!detailshift)
	    {
		colfunc    = basecolfunc = R_DrawColumn;
		fuzzcolfunc  = R_DrawFuzzColumn;
		transcolfunc = R_DrawTranslatedColumn;
		spanfunc     = R_DrawSpan;
	    }
	    else if (detailshift == 1)
	    {
		colfunc    = basecolfunc = R_DrawColumnLow;
		fuzzcolfunc  = R_DrawFuzzColumn;
		transcolfunc = R_DrawTranslatedColumn;
		spanfunc     = R_DrawSpanLow;
	    }
	    else if (detailshift == 2)
	    {
		/* QUAD: 4-wide renderers */
		colfunc    = basecolfunc = R_DrawColumnQuadColor;
		fuzzcolfunc  = R_DrawFuzzColumn;
		transcolfunc = R_DrawTranslatedColumn;
		spanfunc     = R_DrawSpanQuadColor;
	    }
	    else
	    {
		/* MUSH (detailshift=3): 8-wide renderers */
		colfunc    = basecolfunc = R_DrawColumnMushColor;
		fuzzcolfunc  = R_DrawFuzzColumn;
		transcolfunc = R_DrawTranslatedColumn;
		spanfunc     = R_DrawSpanMushColor;
	    }
	}
	else if (!detailshift)
	{
	    /* Phase 4: use direct 1-bit renderers for 68030 SE/30 performance */
	    colfunc    = basecolfunc = R_DrawColumn_Mono;
	    fuzzcolfunc  = R_DrawFuzzColumn_Mono;
	    transcolfunc = R_DrawTranslatedColumn_Mono;
	    spanfunc     = R_DrawSpan_Mono;
	}
	else if (detailshift == 1)
	{
	    colfunc    = basecolfunc = R_DrawColumnLow_Mono;
	    fuzzcolfunc  = R_DrawFuzzColumn_Mono;
	    transcolfunc = R_DrawTranslatedColumn_Mono;
	    spanfunc     = R_DrawSpanLow_Mono;
	}
	else if (detailshift == 2)
	{
	    /* detailshift=2: 4px-wide columns/spans, ~4× fewer pixels to render */
	    colfunc    = basecolfunc = R_DrawColumnQuadLow_Mono;
	    fuzzcolfunc  = R_DrawFuzzColumn_Mono;
	    transcolfunc = R_DrawTranslatedColumn_Mono;
	    spanfunc     = R_DrawSpanQuadLow_Mono;
	}
	else
	{
	    /* detailshift=3: 8px-wide columns/spans (MUSH), ~8× fewer pixels to render */
	    colfunc    = basecolfunc = R_DrawColumnMushLow_Mono;
	    fuzzcolfunc  = R_DrawFuzzColumn_Mono;
	    transcolfunc = R_DrawTranslatedColumn_Mono;
	    spanfunc     = R_DrawSpanMushLow_Mono;
	}
    }

    R_InitBuffer (scaledviewwidth, viewheight);

    /* 2x pixel-scale mode: the source render buffer starts at (0,0) —
     * no centering offset within it. Override the viewwindow position that
     * R_InitBuffer computed (which centers in the logical 320×200 space),
     * and update fb_mono_rowbytes to match the narrower source buffer stride. */
    if (opt_scale2x)
    {
	extern int fb_mono_rowbytes;
	viewwindowx = 0;
	viewwindowy = 0;
	fb_mono_rowbytes = scaledviewwidth >> 3;  /* e.g. 32 bytes/row for blocks=8 */
    }

    /* Clear the 1-bit framebuffer view area after a view-size change.
     * The old view area may contain stale direct-render pixels that now fall
     * inside the new border region (shrink) or in the expanded view area
     * (grow).  Clearing here, before the next R_RenderPlayerView, avoids
     * residual ghost pixels from the previous viewport size. */
    {
	extern void   *fb_mono_base;
	extern int     fb_mono_rowbytes;
	extern int     fb_mono_xoff;
	extern int     fb_mono_yoff;
	doom_log("RESV: vwx=%d vwy=%d vw=%d scvw=%d vh=%d fb=%p\r",
		 viewwindowx, viewwindowy, viewwidth, scaledviewwidth, viewheight, fb_mono_base);
	if (fb_mono_base && scaledviewwidth > 0 && viewheight > 0) {
	    doom_log("RESV: CLEAR fb view area (resize) n=%d\r",
		     scaledviewwidth >> 3);
	    int n_bytes = scaledviewwidth >> 3;  /* low-detail: scaledviewwidth=2*viewwidth */
	    int x_byte  = (viewwindowx + fb_mono_xoff) >> 3;
	    int fy;
	    for (fy = 0; fy < viewheight; fy++) {
		int fb_y = viewwindowy + fy + fb_mono_yoff;
		memset((unsigned char*)fb_mono_base
		       + fb_y * fb_mono_rowbytes + x_byte,
		       0x00, n_bytes);
	    }
	}
    }

    R_InitTextureMapping ();
    
    // psprite scales
    pspritescale = FRACUNIT*viewwidth/SCREENWIDTH;
    pspriteiscale = FRACUNIT*SCREENWIDTH/viewwidth;
    
    // thing clipping
    for (i=0 ; i<viewwidth ; i++)
	screenheightarray[i] = viewheight;
    
    // planes
    for (i=0 ; i<viewheight ; i++)
    {
	dy = ((i-viewheight/2)<<FRACBITS)+FRACUNIT/2;
	dy = abs(dy);
	yslope[i] = FixedDiv ( (viewwidth<<detailshift)/2*FRACUNIT, dy);
    }
	
    for (i=0 ; i<viewwidth ; i++)
    {
	cosadj = abs(finecosine[xtoviewangle[i]>>ANGLETOFINESHIFT]);
	distscale[i] = FixedDiv (FRACUNIT,cosadj);
    }
    
    // Calculate the light levels to use
    //  for each level / scale combination.
    for (i=0 ; i< LIGHTLEVELS ; i++)
    {
	startmap = ((LIGHTLEVELS-1-i)*2)*NUMCOLORMAPS/LIGHTLEVELS;
	for (j=0 ; j<MAXLIGHTSCALE ; j++)
	{
	    level = startmap - j*SCREENWIDTH/(viewwidth<<detailshift)/DISTMAP;
	    
	    if (level < 0)
		level = 0;

	    if (level >= NUMCOLORMAPS)
		level = NUMCOLORMAPS-1;

	    scalelight[i][j] = colormaps + level*256;
	}
    }
}



//
// R_Init
//
extern int	detailLevel;
extern int	screenblocks;



void R_Init (void)
{
    R_InitData ();
    printf ("\nR_InitData");
    R_InitPointToAngle ();
    printf ("\nR_InitPointToAngle");
    R_InitTables ();
    // viewwidth / viewheight / detailLevel are set by the defaults
    printf ("\nR_InitTables");

    R_SetViewSize (screenblocks, detailLevel);
    R_InitPlanes ();
    printf ("\nR_InitPlanes");
    R_InitLightTables ();
    printf ("\nR_InitLightTables");
    R_InitSkyMap ();
    printf ("\nR_InitSkyMap");
    R_InitTranslationTables ();
    printf ("\nR_InitTranslationsTables");
    R_InitQuadNibbleTables ();
    printf ("\nR_InitQuadNibbleTables");
    R_InitMushByteTables ();
    printf ("\nR_InitMushByteTables");

    framecount = 0;
}


//
// R_PointInSubsector
//
subsector_t*
R_PointInSubsector
( fixed_t	x,
  fixed_t	y )
{
    node_t*	node;
    int		side;
    int		nodenum;

    // single subsector is a special case
    if (!numnodes)				
	return subsectors;
		
    nodenum = numnodes-1;

    while (! (nodenum & NF_SUBSECTOR) )
    {
	node = &nodes[nodenum];
	side = R_PointOnSide (x, y, node);
	nodenum = node->children[side];
    }
	
    return &subsectors[nodenum & ~NF_SUBSECTOR];
}



//
// R_SetupFrame
//
void R_SetupFrame (player_t* player)
{		
    int		i;
    
    viewplayer = player;
    viewx = player->mo->x;
    viewy = player->mo->y;
    viewangle = player->mo->angle + viewangleoffset;
    extralight = player->extralight;

    viewz = player->viewz;
    
    viewsin = finesine[viewangle>>ANGLETOFINESHIFT];
    viewcos = finecosine[viewangle>>ANGLETOFINESHIFT];
	
    sscount = 0;
	
    if (player->fixedcolormap)
    {
	fixedcolormap =
	    colormaps
	    + player->fixedcolormap*256*sizeof(lighttable_t);
	
	walllights = scalelightfixed;

	for (i=0 ; i<MAXLIGHTSCALE ; i++)
	    scalelightfixed[i] = fixedcolormap;
    }
    else
	fixedcolormap = 0;
		
    framecount++;
    validcount++;
}



//
// R_RenderView
//
void R_RenderPlayerView (player_t* player)
{
    int y;

    { long _t = I_GetMacTick();
    /* Phase 4: clear the screens[0] view area to 0 before rendering.
     * The mono renderers write directly to the 1-bit framebuffer and do not
     * touch screens[0].  I_FinishUpdate treats non-zero screens[0] view pixels
     * as HUD/menu overlays and blits them on top of the direct render.
     * Pre-clearing ensures only genuine HUD content (non-zero) gets overlaid.
     * Skip when menuactive: the 8-bit renderers need screens[0] intact, and
     * I_FinishUpdate will do a full blit (no selective overlay) anyway. */
    {
	extern boolean menuactive;
	extern void   *fb_mono_base;
	extern int     fb_mono_rowbytes;
	extern int     fb_mono_xoff;
	extern int     fb_mono_yoff;
	extern int     opt_solidfloor;
	extern int     solidfloor_gray;

	if (!menuactive) {
	    /* Clear screens[0] view area: non-zero pixels are treated as HUD overlay
	     * by I_FinishUpdate and blitted on top of the direct 1-bit render. */
	    for (y = 0; y < viewheight; y++)
		memset(screens[0] + (viewwindowy + y) * SCREENWIDTH + viewwindowx,
		       0, scaledviewwidth);

	    /* opt_solidfloor: R_DrawPlanes skips flat rendering entirely (including
	     * R_DrawSpan_Mono which previously filled floor/ceiling with a solid grey).
	     * Clear the 1-bit FB view area each frame so floor/ceiling stay clean white
	     * (0x00 = white on Mac 1-bit) rather than showing stale FB content.
	     * Cost: ~6 KB memset per frame — negligible vs the 33 ms saved by skipping
	     * the full R_MakeSpans + R_DrawSpan_Mono plane rendering pass. */
	    if (opt_solidfloor && fb_mono_base && scaledviewwidth > 0) {
		/* Fill pattern table [gray_level][row_group], row_group=(fy>>1)&3.
		 * Scanline-doubling pairs rows, so each group of 2 screen rows shares
		 * one fill byte.  Patterns cycle over 4 row groups to avoid vertical
		 * stripes.  Level 3 (~88%): 1 white pixel per 8 shifted diagonally.
		 *   0x7F=01111111 white@col7   0xBF=10111111 white@col6
		 *   0xDF=11011111 white@col5   0xEF=11101111 white@col4  */
		static const unsigned char sfill[5][4] = {
		    {0x00,0x00,0x00,0x00},  /* 0: white          */
		    {0x11,0x11,0x11,0x11},  /* 1: 25% light gray */
		    {0xAA,0xAA,0xAA,0xAA},  /* 2: 50% mid gray   */
		    {0x7F,0xBF,0xDF,0xEF},  /* 3: ~88% dark gray  */
		    {0xFF,0xFF,0xFF,0xFF},  /* 4: black          */
		};
		int glevel = solidfloor_gray;
		int x_byte, n_bytes, fy;
		if (glevel < 0) glevel = 0;
		if (glevel > 4) glevel = 4;
		x_byte  = (viewwindowx + fb_mono_xoff) >> 3;
		n_bytes = scaledviewwidth >> 3;
		for (fy = 0; fy < viewheight; fy++) {
		    memset((unsigned char*)fb_mono_base
			   + (viewwindowy + fy + fb_mono_yoff) * fb_mono_rowbytes
			   + x_byte, sfill[glevel][(fy >> 1) & 3], n_bytes);
		}
	    }

	    /* Half-line (#2): odd rows are NOT pre-cleared.  They retain the previous
	     * frame's even-row data (temporal interlacing).  This eliminates the white
	     * flash that occurred when clearing odd rows to 0x00 at frame start — on a
	     * non-double-buffered display the cleared rows were visible for the entire
	     * render time (~300-500ms at 2-3 FPS). */
	}

	/* On menu→gameplay transition: clear the 1-bit FB view area.
	 * The preceding non-direct frame wrote a full-blit of screens[0] (mostly
	 * black + menu text) into the 1-bit framebuffer.  The direct renderers
	 * overwrite most pixels, but any edge-cases they miss leave a permanent
	 * ghost (the display is live; there is no back-buffer).  A one-time clear
	 * ensures a clean slate; the direct render fills everything in immediately.
	 * 0x00 = white on Mac 1-bit display. */
	{
	    static boolean prev_menuactive = false;
	    /* Instrument: log every menuactive state change seen here */
	    if (menuactive != prev_menuactive) {
		doom_log("RRV: menuactive %d->%d fb=%p vwx=%d vwy=%d vw=%d vh=%d\r",
			 (int)prev_menuactive, (int)menuactive,
			 fb_mono_base,
			 viewwindowx, viewwindowy, viewwidth, viewheight);
	    }
	    if (prev_menuactive && !menuactive && fb_mono_base) {
		/* Re-run view size setup so Mono colfuncs are selected instead of
		 * the 8-bit fallbacks that R_ExecuteSetViewSize chose while the
		 * menu was open. */
		setsizeneeded = true;
		doom_log("RRV: CLEAR fb view area (menu dismissed) scvw=%d\r",
			 scaledviewwidth);
		int x_byte = (viewwindowx + fb_mono_xoff) >> 3;
		int n_bytes = scaledviewwidth >> 3;  /* low-detail: scaledviewwidth=2*viewwidth */
		for (y = 0; y < viewheight; y++) {
		    int fb_y = viewwindowy + y + fb_mono_yoff;
		    memset((unsigned char*)fb_mono_base
			   + fb_y * fb_mono_rowbytes + x_byte,
			   0x00, n_bytes);
		}
	    } else if (prev_menuactive && !menuactive) {
		doom_log("RRV: menu dismissed but fb_mono_base=NULL, no clear\r");
	    }
	    prev_menuactive = menuactive;
	}
    }

    R_SetupFrame (player);

    // Clear buffers.
    R_ClearClipSegs ();
    R_ClearDrawSegs ();
    R_ClearPlanes ();
    R_ClearSprites ();
    if (netgame) NetUpdate ();
    prof_r_setup += I_GetMacTick() - _t; }

    { long _t = I_GetMacTick();
    // The head node is the last node output.
    R_RenderBSPNode (numnodes-1);
    if (netgame) NetUpdate ();
    prof_r_bsp += I_GetMacTick() - _t; }

    { long _t = I_GetMacTick();
    R_DrawPlanes ();
    if (netgame) NetUpdate ();
    prof_r_planes += I_GetMacTick() - _t; }

    { long _t = I_GetMacTick();
    R_DrawMasked ();
    if (netgame) NetUpdate ();
    prof_r_masked += I_GetMacTick() - _t; }

    /* Half-line scanline doubling: after the direct renderer fills even FB rows,
     * copy each even row to the adjacent odd row.  This replaces temporal
     * interlacing with same-frame doubling, which avoids:
     *   - White stripes (odd rows cleared but never re-rendered)
     *   - Ghost images (odd rows left with stale menu/wipe content)
     * Cost is trivial: N/2 memcpy calls of ~40 bytes each. */
    {
	extern int   opt_halfline;
	extern void *fb_mono_base;
	extern int   fb_mono_rowbytes;
	extern int   fb_mono_xoff;
	extern int   fb_mono_yoff;
	if (opt_halfline && fb_mono_base && scaledviewwidth > 0 && viewheight > 1) {
	    int x_byte = (viewwindowx + fb_mono_xoff) >> 3;
	    int n_bytes = scaledviewwidth >> 3;
	    int ey;
	    for (ey = 0; ey + 1 < viewheight; ey += 2) {
		unsigned char *even_row = (unsigned char*)fb_mono_base
		    + (viewwindowy + ey + fb_mono_yoff) * fb_mono_rowbytes + x_byte;
		memcpy(even_row + fb_mono_rowbytes, even_row, n_bytes);
	    }
	} else if (opt_halfline && !fb_mono_base && scaledviewwidth > 0 && viewheight > 1) {
	    /* Color path: duplicate even screen rows to odd rows in screens[0] view area */
	    extern int g_color_depth;
	    if (g_color_depth >= 8) {
		int ey;
		for (ey = 0; ey + 1 < viewheight; ey += 2) {
		    byte *even_row = screens[0]
			+ (viewwindowy + ey) * SCREENWIDTH + viewwindowx;
		    memcpy(even_row + SCREENWIDTH, even_row, scaledviewwidth);
		}
	    }
	}
    }

    /* PROBE A: read back two fb bytes after all rendering (post scanline-double).
     * Byte at center row (wall area) and bottom row (floor area).
     * If wall_byte == 0xFF, column renders did NOT reach this location.
     * If wall_byte is dithered (e.g. 0xAF/0x5F), renders are working. */
    {
	extern void *fb_mono_base;
	extern int   fb_mono_rowbytes;
	extern int   fb_mono_xoff;
	extern int   fb_mono_yoff;
	static int probe_a_logged = 0;
	/* Guard: only fire during actual gameplay (not menu, not pre-game frames).
	 * Read bytes at dc_x=0 (byte cx0) and dc_x=14 (byte cx14) of center row.
	 * dc_x=14 => fb_x=(14<<2)+viewwindowx+fb_mono_xoff => byte (fb_x>>3). */
	if (!probe_a_logged && detailshift == 2 && fb_mono_base && !menuactive && gametic > 0) {
	    int cx0   = (viewwindowx + fb_mono_xoff) >> 3;          /* dc_x=0  byte */
	    int cx14  = ((14<<2) + viewwindowx + fb_mono_xoff) >> 3;/* dc_x=14 byte */
	    int wall_y  = viewwindowy + viewheight/2 + fb_mono_yoff;
	    int floor_y = viewwindowy + viewheight - 2 + fb_mono_yoff;
	    unsigned char b0w  = ((unsigned char*)fb_mono_base)[wall_y  * fb_mono_rowbytes + cx0];
	    unsigned char b14w = ((unsigned char*)fb_mono_base)[wall_y  * fb_mono_rowbytes + cx14];
	    unsigned char b0f  = ((unsigned char*)fb_mono_base)[floor_y * fb_mono_rowbytes + cx0];
	    doom_log("PROBE_A: cx0_wall=0x%02X cx14_wall=0x%02X cx0_floor=0x%02X (wy=%d fy=%d cx0=%d cx14=%d ma=%d gt=%ld)\r",
		     b0w, b14w, b0f, wall_y, floor_y, cx0, cx14, (int)menuactive, (long)gametic);
	    probe_a_logged = 1;
	}
    }
    /* Crash bisect: confirm render completed before IFU */
    if (menuactive) { doom_log("RPV_DONE ma=1\r"); doom_log_flush(); }
}
