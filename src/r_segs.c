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
//	All the clipping: columns, horizontal spans, sky columns.
//
//-----------------------------------------------------------------------------


static const char
rcsid[] = "$Id: r_segs.c,v 1.3 1997/01/29 20:10:19 b1 Exp $";





#include <stdlib.h>

#include "i_system.h"

#include "doomdef.h"
#include "doomstat.h"

#include "r_local.h"
#include "r_sky.h"


// OPTIMIZE: closed two sided lines as single sided

// True if any of the segs textures might be visible.
boolean		segtextured;	

// False if the back side is the same plane.
boolean		markfloor;	
boolean		markceiling;

boolean		maskedtexture;
int		toptexture;
int		bottomtexture;
int		midtexture;


angle_t		rw_normalangle;
// angle to line origin
int		rw_angle1;	

//
// regular wall
//
int		rw_x;
int		rw_stopx;
angle_t		rw_centerangle;
fixed_t		rw_offset;
fixed_t		rw_distance;
fixed_t		rw_scale;
fixed_t		rw_scalestep;
/* Precomputed iscale for linear interpolation across wall columns.
 * Replaces per-column: dc_iscale = 0xffffffffu / rw_scale (~150 cycles each)
 * with: dc_iscale = rw_iscale; rw_iscale += rw_iscalestep  (~2 cycles each). */
static fixed_t  rw_iscale;
static fixed_t  rw_iscalestep;
/* Affine texture column stepping — replaces FixedMul(finetangent,rw_distance) per column
 * (~70 cycles each) with a single addition (~2 cycles each).
 * Enabled by opt_affine_texcol (-affinetex arg), default OFF. */
static fixed_t  rw_texcol;
static fixed_t  rw_texstep;
extern int opt_affine_texcol;
extern int opt_halfline;
extern int opt_solidfloor;
extern int fog_scale;

/* BSP sub-profiling (60Hz ticks, reset by d_main.c every 35 game tics).
 *   prof_r_segs     = number of R_StoreWallRange calls (visible wall segs rendered)
 *   prof_r_seg_loop = time spent inside R_RenderSegLoop (per-column drawing)
 * Derived in log: bsp - seg_loop = BSP traversal + per-seg setup overhead. */
long prof_r_segs     = 0;
long prof_r_seg_loop = 0;
fixed_t		rw_midtexturemid;
fixed_t		rw_toptexturemid;
fixed_t		rw_bottomtexturemid;

int		worldtop;
int		worldbottom;
int		worldhigh;
int		worldlow;

fixed_t		pixhigh;
fixed_t		pixlow;
fixed_t		pixhighstep;
fixed_t		pixlowstep;

fixed_t		topfrac;
fixed_t		topstep;

fixed_t		bottomfrac;
fixed_t		bottomstep;


lighttable_t**	walllights;

short*		maskedtexturecol;



//
// R_RenderMaskedSegRange
//
void
R_RenderMaskedSegRange
( drawseg_t*	ds,
  int		x1,
  int		x2 )
{
    unsigned	index;
    column_t*	col;
    int		lightnum;
    int		texnum;
    
    // Calculate light table.
    // Use different light tables
    //   for horizontal / vertical / diagonal. Diagonal?
    // OPTIMIZE: get rid of LIGHTSEGSHIFT globally
    curline = ds->curline;
    frontsector = curline->frontsector;
    backsector = curline->backsector;
    texnum = texturetranslation[curline->sidedef->midtexture];
	
    lightnum = (frontsector->lightlevel >> LIGHTSEGSHIFT)+extralight;

    if (curline->v1->y == curline->v2->y)
	lightnum--;
    else if (curline->v1->x == curline->v2->x)
	lightnum++;

    if (lightnum < 0)		
	walllights = scalelight[0];
    else if (lightnum >= LIGHTLEVELS)
	walllights = scalelight[LIGHTLEVELS-1];
    else
	walllights = scalelight[lightnum];

    maskedtexturecol = ds->maskedtexturecol;

    rw_scalestep = ds->scalestep;
    spryscale = ds->scale1 + (x1 - ds->x1)*rw_scalestep;
    mfloorclip = ds->sprbottomclip;
    mceilingclip = ds->sprtopclip;

    /* Precompute iscale linear interpolation for masked segment columns */
    {
	fixed_t mpr_iscale = 0xffffffffu / (unsigned)spryscale;
	fixed_t mpr_iscalestep;
	if (x2 > x1)
	{
	    fixed_t spryscale_end = ds->scale1 + (x2 - ds->x1)*rw_scalestep;
	    mpr_iscalestep = (0xffffffffu / (unsigned)spryscale_end - mpr_iscale) / (x2 - x1);
	}
	else
	    mpr_iscalestep = 0;
    
    // find positioning
    if (curline->linedef->flags & ML_DONTPEGBOTTOM)
    {
	dc_texturemid = frontsector->floorheight > backsector->floorheight
	    ? frontsector->floorheight : backsector->floorheight;
	dc_texturemid = dc_texturemid + textureheight[texnum] - viewz;
    }
    else
    {
	dc_texturemid =frontsector->ceilingheight<backsector->ceilingheight
	    ? frontsector->ceilingheight : backsector->ceilingheight;
	dc_texturemid = dc_texturemid - viewz;
    }
    dc_texturemid += curline->sidedef->rowoffset;
			
    if (fixedcolormap)
	dc_colormap = fixedcolormap;
    
    // draw the columns
    for (dc_x = x1 ; dc_x <= x2 ; dc_x++)
    {
	// calculate lighting
	if (maskedtexturecol[dc_x] != MAXSHORT)
	{
	    if (!fixedcolormap)
	    {
		index = spryscale>>LIGHTSCALESHIFT;

		if (index >=  MAXLIGHTSCALE )
		    index = MAXLIGHTSCALE-1;

		dc_colormap = walllights[index];
	    }

	    sprtopscreen = centeryfrac - FixedMul(dc_texturemid, spryscale);
	    /* Linear interpolation replaces per-column 32-bit divide */
	    dc_iscale = mpr_iscale;

	    // draw the texture
	    col = (column_t *)(
		(byte *)R_GetColumn(texnum,maskedtexturecol[dc_x]) -3);

	    R_DrawMaskedColumn (col);
	    maskedtexturecol[dc_x] = MAXSHORT;
	}
	mpr_iscale += mpr_iscalestep;
	spryscale += rw_scalestep;
    }
    } /* end mpr_iscale block */

}




//
// R_RenderSegLoop
// Draws zero, one, or two textures (and possibly a masked
//  texture) for walls.
// Can draw or mark the starting pixel of floor and ceiling
//  textures.
// CALLED: CORE LOOPING ROUTINE.
//
/* HEIGHTBITS=16 lets GCC emit SWAP+EXT.L (8 cycles) for >>HEIGHTBITS instead of
 * two ASR.L (12 cycles). Requires topfrac/bottomfrac/pixhigh/pixlow scaled ×16
 * in R_StoreWallRange (see below). Net saving: ~4 cycles/shift × 4 shifts/col. */
#define HEIGHTBITS		16
#define HEIGHTUNIT		(1<<HEIGHTBITS)

void R_RenderSegLoop (void)
{
    unsigned		index;
    int			yl;
    int			yh;
    int			mid;
    fixed_t		texturecolumn;
    int			top;
    int			bottom;

    texturecolumn = 0;				// shut up compiler warning

    /* C: LOD — minimum column height worth calling colfunc.
     * In halfline mode a 1-pixel column (yl==yh) renders ≤1 pixel (50% chance even row).
     * Skip it: save R_GetColumn + colfunc call overhead for these tiny slivers. */
    int lod_min_height = opt_halfline ? 1 : 0;

    /* E: Visplane mark optimisation.
     * ceilingplane->top[]/bottom[] and floorplane->top[]/bottom[] are only consumed
     * by R_DrawPlanes for flat (floor/ceiling) rendering.  When opt_solidfloor=1,
     * R_DrawPlanes skips all flat rendering — so writing those arrays is wasted work.
     * Exception: the sky ceiling is rendered from ceilingplane in R_DrawPlanes even
     * when opt_solidfloor=1, so we must still mark it for sky-ceiling segments.
     *
     * NOTE: ceilingclip[]/floorclip[] BSP clip arrays are NOT affected — they must
     * always be updated so subsequent segments clip correctly. Only the visplane
     * top[]/bottom[] writes are suppressed. */
    boolean loop_markceiling = markceiling &&
        (!opt_solidfloor || frontsector->ceilingpic == skyflatnum);
    boolean loop_markfloor = markfloor && !opt_solidfloor;

    for ( ; rw_x < rw_stopx ; rw_x++)
    {
	/* Fog: columns beyond fog_scale threshold are skipped — the solidfloor
	 * background shows through them, giving a free distance-fog effect.
	 * BSP clip arrays (ceilingclip/floorclip) are still updated so
	 * subsequent segments clip correctly. Only colfunc draws are skipped. */
	int in_fog = (fog_scale > 0 && rw_scale < fog_scale);

	// mark floor / ceiling areas
	yl = (topfrac+HEIGHTUNIT-1)>>HEIGHTBITS;

	// no space above wall?
	if (yl < ceilingclip[rw_x]+1)
	    yl = ceilingclip[rw_x]+1;

	/* E: write to visplane top[]/bottom[] only when needed */
	if (loop_markceiling)
	{
	    top = ceilingclip[rw_x]+1;
	    bottom = yl-1;

	    if (bottom >= floorclip[rw_x])
		bottom = floorclip[rw_x]-1;

	    if (top <= bottom)
	    {
		ceilingplane->top[rw_x] = top;
		ceilingplane->bottom[rw_x] = bottom;
	    }
	}

	yh = bottomfrac>>HEIGHTBITS;

	if (yh >= floorclip[rw_x])
	    yh = floorclip[rw_x]-1;

	/* E: write to floorplane top[]/bottom[] only when needed */
	if (loop_markfloor)
	{
	    top = yh+1;
	    bottom = floorclip[rw_x]-1;
	    if (top <= ceilingclip[rw_x])
		top = ceilingclip[rw_x]+1;
	    if (top <= bottom)
	    {
		floorplane->top[rw_x] = top;
		floorplane->bottom[rw_x] = bottom;
	    }
	}

	// texturecolumn and lighting are independent of wall tiers
	if (segtextured)
	{
	    /* Affine stepping (opt_affine_texcol) or original per-column FixedMul */
	    if (opt_affine_texcol) {
		texturecolumn = rw_texcol >> FRACBITS;
		rw_texcol += rw_texstep;
	    } else {
		angle_t a = (rw_centerangle + xtoviewangle[rw_x]) >> ANGLETOFINESHIFT;
		texturecolumn = (rw_offset - FixedMul(finetangent[a], rw_distance)) >> FRACBITS;
	    }
	    // calculate lighting
	    index = rw_scale>>LIGHTSCALESHIFT;

	    if (index >=  MAXLIGHTSCALE )
		index = MAXLIGHTSCALE-1;

	    dc_colormap = walllights[index];
	    dc_x = rw_x;
	    /* Linear interpolation replaces per-column 32-bit divide */
	    dc_iscale = rw_iscale;
	    rw_iscale += rw_iscalestep;
	}

	// draw the wall tiers
	if (midtexture)
	{
	    // single sided line
	    dc_yl = yl;
	    dc_yh = yh;
	    dc_texturemid = rw_midtexturemid;
	    /* C: LOD; fog: skip draw if too thin or beyond fog distance */
	    if (!in_fog && dc_yh >= dc_yl + lod_min_height)
	    {
		dc_source = R_GetColumn(midtexture,texturecolumn);
		colfunc();
	    }
	    ceilingclip[rw_x] = viewheight;
	    floorclip[rw_x] = -1;
	}
	else
	{
	    // two sided line
	    if (toptexture)
	    {
		// top wall
		mid = pixhigh>>HEIGHTBITS;
		pixhigh += pixhighstep;

		if (mid >= floorclip[rw_x])
		    mid = floorclip[rw_x]-1;

		if (mid >= yl)
		{
		    dc_yl = yl;
		    dc_yh = mid;
		    dc_texturemid = rw_toptexturemid;
		    /* C: LOD; fog */
		    if (!in_fog && mid >= yl + lod_min_height)
		    {
			dc_source = R_GetColumn(toptexture,texturecolumn);
			colfunc();
		    }
		    ceilingclip[rw_x] = mid;
		}
		else
		    ceilingclip[rw_x] = yl-1;
	    }
	    else
	    {
		// no top wall — BSP clip must still update
		if (markceiling)
		    ceilingclip[rw_x] = yl-1;
	    }

	    if (bottomtexture)
	    {
		// bottom wall
		mid = (pixlow+HEIGHTUNIT-1)>>HEIGHTBITS;
		pixlow += pixlowstep;

		// no space above wall?
		if (mid <= ceilingclip[rw_x])
		    mid = ceilingclip[rw_x]+1;

		if (mid <= yh)
		{
		    dc_yl = mid;
		    dc_yh = yh;
		    dc_texturemid = rw_bottomtexturemid;
		    /* C: LOD; fog */
		    if (!in_fog && yh >= mid + lod_min_height)
		    {
			dc_source = R_GetColumn(bottomtexture,
						texturecolumn);
			colfunc();
		    }
		    floorclip[rw_x] = mid;
		}
		else
		    floorclip[rw_x] = yh+1;
	    }
	    else
	    {
		// no bottom wall — BSP clip must still update
		if (markfloor)
		    floorclip[rw_x] = yh+1;
	    }

	    if (maskedtexture)
	    {
		// save texturecol
		//  for backdrawing of masked mid texture
		maskedtexturecol[rw_x] = texturecolumn;
	    }
	}

	rw_scale += rw_scalestep;
	topfrac += topstep;
	bottomfrac += bottomstep;
    }
}




//
// R_StoreWallRange
// A wall segment will be drawn
//  between start and stop pixels (inclusive).
//
void
R_StoreWallRange
( int	start,
  int	stop )
{
    fixed_t		hyp;
    fixed_t		sineval;
    angle_t		distangle, offsetangle;
    fixed_t		vtop;
    int			lightnum;

    // don't overflow and crash
    if (ds_p == &drawsegs[MAXDRAWSEGS])
	return;
    prof_r_segs++;
		
#ifdef RANGECHECK
    if (start >=viewwidth || start > stop)
	I_Error ("Bad R_RenderWallRange: %i to %i", start , stop);
#endif
    
    sidedef = curline->sidedef;
    linedef = curline->linedef;

    // mark the segment as visible for auto map
    linedef->flags |= ML_MAPPED;
    
    // calculate rw_distance for scale calculation
    rw_normalangle = curline->angle + ANG90;
    offsetangle = abs(rw_normalangle-rw_angle1);
    
    if (offsetangle > ANG90)
	offsetangle = ANG90;

    distangle = ANG90 - offsetangle;
    hyp = R_PointToDist (curline->v1->x, curline->v1->y);
    sineval = finesine[distangle>>ANGLETOFINESHIFT];
    rw_distance = FixedMul (hyp, sineval);
		
	
    ds_p->x1 = rw_x = start;
    ds_p->x2 = stop;
    ds_p->curline = curline;
    rw_stopx = stop+1;
    
    // calculate scale at both ends and step
    ds_p->scale1 = rw_scale =
	R_ScaleFromGlobalAngle (viewangle + xtoviewangle[start]);

    if (stop > start )
    {
	ds_p->scale2 = R_ScaleFromGlobalAngle (viewangle + xtoviewangle[stop]);
	ds_p->scalestep = rw_scalestep =
	    (ds_p->scale2 - rw_scale) / (stop-start);
    }
    else
    {
	// UNUSED: try to fix the stretched line bug
#if 0
	if (rw_distance < FRACUNIT/2)
	{
	    fixed_t		trx,try;
	    fixed_t		gxt,gyt;

	    trx = curline->v1->x - viewx;
	    try = curline->v1->y - viewy;

	    gxt = FixedMul(trx,viewcos);
	    gyt = -FixedMul(try,viewsin);
	    ds_p->scale1 = FixedDiv(projection, gxt-gyt)<<detailshift;
	}
#endif
	ds_p->scale2 = ds_p->scale1;
    }

    /* Precompute iscale for linear interpolation — avoids per-column 32-bit division.
     * rw_scale advances linearly, so 1/scale interpolates accurately for normal walls.
     * Two divisions here replaces N divisions in the inner loop (N = stop - start + 1). */
    rw_iscale = 0xffffffffu / (unsigned)rw_scale;
    if (stop > start)
    {
	fixed_t iscale2 = 0xffffffffu / (unsigned)ds_p->scale2;
	rw_iscalestep = (iscale2 - rw_iscale) / (stop - start);
    }
    else
	rw_iscalestep = 0;
    
    // calculate texture boundaries
    //  and decide if floor / ceiling marks are needed
    worldtop = frontsector->ceilingheight - viewz;
    worldbottom = frontsector->floorheight - viewz;
	
    midtexture = toptexture = bottomtexture = maskedtexture = 0;
    ds_p->maskedtexturecol = NULL;
	
    if (!backsector)
    {
	// single sided line
	midtexture = texturetranslation[sidedef->midtexture];
	// a single sided line is terminal, so it must mark ends
	markfloor = markceiling = true;
	if (linedef->flags & ML_DONTPEGBOTTOM)
	{
	    vtop = frontsector->floorheight +
		textureheight[sidedef->midtexture];
	    // bottom of texture at bottom
	    rw_midtexturemid = vtop - viewz;	
	}
	else
	{
	    // top of texture at top
	    rw_midtexturemid = worldtop;
	}
	rw_midtexturemid += sidedef->rowoffset;

	ds_p->silhouette = SIL_BOTH;
	ds_p->sprtopclip = screenheightarray;
	ds_p->sprbottomclip = negonearray;
	ds_p->bsilheight = MAXINT;
	ds_p->tsilheight = MININT;
    }
    else
    {
	// two sided line
	ds_p->sprtopclip = ds_p->sprbottomclip = NULL;
	ds_p->silhouette = 0;
	
	if (frontsector->floorheight > backsector->floorheight)
	{
	    ds_p->silhouette = SIL_BOTTOM;
	    ds_p->bsilheight = frontsector->floorheight;
	}
	else if (backsector->floorheight > viewz)
	{
	    ds_p->silhouette = SIL_BOTTOM;
	    ds_p->bsilheight = MAXINT;
	    // ds_p->sprbottomclip = negonearray;
	}
	
	if (frontsector->ceilingheight < backsector->ceilingheight)
	{
	    ds_p->silhouette |= SIL_TOP;
	    ds_p->tsilheight = frontsector->ceilingheight;
	}
	else if (backsector->ceilingheight < viewz)
	{
	    ds_p->silhouette |= SIL_TOP;
	    ds_p->tsilheight = MININT;
	    // ds_p->sprtopclip = screenheightarray;
	}
		
	if (backsector->ceilingheight <= frontsector->floorheight)
	{
	    ds_p->sprbottomclip = negonearray;
	    ds_p->bsilheight = MAXINT;
	    ds_p->silhouette |= SIL_BOTTOM;
	}
	
	if (backsector->floorheight >= frontsector->ceilingheight)
	{
	    ds_p->sprtopclip = screenheightarray;
	    ds_p->tsilheight = MININT;
	    ds_p->silhouette |= SIL_TOP;
	}
	
	worldhigh = backsector->ceilingheight - viewz;
	worldlow = backsector->floorheight - viewz;
		
	// hack to allow height changes in outdoor areas
	if (frontsector->ceilingpic == skyflatnum 
	    && backsector->ceilingpic == skyflatnum)
	{
	    worldtop = worldhigh;
	}
	
			
	if (worldlow != worldbottom 
	    || backsector->floorpic != frontsector->floorpic
	    || backsector->lightlevel != frontsector->lightlevel)
	{
	    markfloor = true;
	}
	else
	{
	    // same plane on both sides
	    markfloor = false;
	}
	
			
	if (worldhigh != worldtop 
	    || backsector->ceilingpic != frontsector->ceilingpic
	    || backsector->lightlevel != frontsector->lightlevel)
	{
	    markceiling = true;
	}
	else
	{
	    // same plane on both sides
	    markceiling = false;
	}
	
	if (backsector->ceilingheight <= frontsector->floorheight
	    || backsector->floorheight >= frontsector->ceilingheight)
	{
	    // closed door
	    markceiling = markfloor = true;
	}
	

	if (worldhigh < worldtop)
	{
	    // top texture
	    toptexture = texturetranslation[sidedef->toptexture];
	    if (linedef->flags & ML_DONTPEGTOP)
	    {
		// top of texture at top
		rw_toptexturemid = worldtop;
	    }
	    else
	    {
		vtop =
		    backsector->ceilingheight
		    + textureheight[sidedef->toptexture];
		
		// bottom of texture
		rw_toptexturemid = vtop - viewz;	
	    }
	}
	if (worldlow > worldbottom)
	{
	    // bottom texture
	    bottomtexture = texturetranslation[sidedef->bottomtexture];

	    if (linedef->flags & ML_DONTPEGBOTTOM )
	    {
		// bottom of texture at bottom
		// top of texture at top
		rw_bottomtexturemid = worldtop;
	    }
	    else	// top of texture at top
		rw_bottomtexturemid = worldlow;
	}
	rw_toptexturemid += sidedef->rowoffset;
	rw_bottomtexturemid += sidedef->rowoffset;
	
	// allocate space for masked texture tables
	if (sidedef->midtexture)
	{
	    // masked midtexture
	    maskedtexture = true;
	    ds_p->maskedtexturecol = maskedtexturecol = lastopening - rw_x;
	    lastopening += rw_stopx - rw_x;
	}
    }
    
    // calculate rw_offset (only needed for textured lines)
    segtextured = midtexture | toptexture | bottomtexture | maskedtexture;

    if (segtextured)
    {
	offsetangle = rw_normalangle-rw_angle1;
	
	if (offsetangle > ANG180)
	    offsetangle = -offsetangle;

	if (offsetangle > ANG90)
	    offsetangle = ANG90;

	sineval = finesine[offsetangle >>ANGLETOFINESHIFT];
	rw_offset = FixedMul (hyp, sineval);

	if (rw_normalangle-rw_angle1 < ANG180)
	    rw_offset = -rw_offset;

	rw_offset += sidedef->textureoffset + curline->offset;
	rw_centerangle = ANG90 + viewangle - rw_normalangle;
	
	// calculate light table
	//  use different light tables
	//  for horizontal / vertical / diagonal
	// OPTIMIZE: get rid of LIGHTSEGSHIFT globally
	if (!fixedcolormap)
	{
	    lightnum = (frontsector->lightlevel >> LIGHTSEGSHIFT)+extralight;

	    if (curline->v1->y == curline->v2->y)
		lightnum--;
	    else if (curline->v1->x == curline->v2->x)
		lightnum++;

	    if (lightnum < 0)		
		walllights = scalelight[0];
	    else if (lightnum >= LIGHTLEVELS)
		walllights = scalelight[LIGHTLEVELS-1];
	    else
		walllights = scalelight[lightnum];
	}

	/* Affine texture column stepping (opt_affine_texcol, -affinetex arg):
	 * Precompute per-wall start/step to replace per-column FixedMul (~80 cycles)
	 * with a single addition (~2 cycles). */
	if (opt_affine_texcol) {
	    angle_t a0 = (rw_centerangle + xtoviewangle[rw_x]) >> ANGLETOFINESHIFT;
	    rw_texcol = rw_offset - FixedMul(finetangent[a0], rw_distance);
	    if (rw_stopx > rw_x + 1) {
		angle_t a1 = (rw_centerangle + xtoviewangle[rw_stopx - 1]) >> ANGLETOFINESHIFT;
		fixed_t t1 = rw_offset - FixedMul(finetangent[a1], rw_distance);
		rw_texstep = (t1 - rw_texcol) / (rw_stopx - 1 - rw_x);
	    } else
		rw_texstep = 0;
	}
    }

    // if a floor / ceiling plane is on the wrong side
    //  of the view plane, it is definitely invisible
    //  and doesn't need to be marked.
    
  
    if (frontsector->floorheight >= viewz)
    {
	// above view plane
	markfloor = false;
    }
    
    if (frontsector->ceilingheight <= viewz 
	&& frontsector->ceilingpic != skyflatnum)
    {
	// below view plane
	markceiling = false;
    }

    
    // calculate incremental stepping values for texture edges
    worldtop >>= 4;
    worldbottom >>= 4;
	
    /* Scale ×16 so >>HEIGHTBITS (now 16) uses SWAP+EXT.L instead of two ASR.L. */
    topstep = -FixedMul (rw_scalestep, worldtop) << 4;
    topfrac = ((centeryfrac>>4) - FixedMul (worldtop, rw_scale)) << 4;

    bottomstep = -FixedMul (rw_scalestep,worldbottom) << 4;
    bottomfrac = ((centeryfrac>>4) - FixedMul (worldbottom, rw_scale)) << 4;
	
    if (backsector)
    {	
	worldhigh >>= 4;
	worldlow >>= 4;

	if (worldhigh < worldtop)
	{
	    pixhigh = ((centeryfrac>>4) - FixedMul (worldhigh, rw_scale)) << 4;
	    pixhighstep = -FixedMul (rw_scalestep,worldhigh) << 4;
	}

	if (worldlow > worldbottom)
	{
	    pixlow = ((centeryfrac>>4) - FixedMul (worldlow, rw_scale)) << 4;
	    pixlowstep = -FixedMul (rw_scalestep,worldlow) << 4;
	}
    }
    
    // render it
    if (markceiling)
	ceilingplane = R_CheckPlane (ceilingplane, rw_x, rw_stopx-1);
    
    if (markfloor)
	floorplane = R_CheckPlane (floorplane, rw_x, rw_stopx-1);

    { long _slt = I_GetMacTick();
    R_RenderSegLoop ();
    prof_r_seg_loop += I_GetMacTick() - _slt; }

    // save sprite clipping info
    if ( ((ds_p->silhouette & SIL_TOP) || maskedtexture)
	 && !ds_p->sprtopclip)
    {
	memcpy (lastopening, ceilingclip+start, 2*(rw_stopx-start));
	ds_p->sprtopclip = lastopening - start;
	lastopening += rw_stopx - start;
    }
    
    if ( ((ds_p->silhouette & SIL_BOTTOM) || maskedtexture)
	 && !ds_p->sprbottomclip)
    {
	memcpy (lastopening, floorclip+start, 2*(rw_stopx-start));
	ds_p->sprbottomclip = lastopening - start;
	lastopening += rw_stopx - start;	
    }

    if (maskedtexture && !(ds_p->silhouette&SIL_TOP))
    {
	ds_p->silhouette |= SIL_TOP;
	ds_p->tsilheight = MININT;
    }
    if (maskedtexture && !(ds_p->silhouette&SIL_BOTTOM))
    {
	ds_p->silhouette |= SIL_BOTTOM;
	ds_p->bsilheight = MAXINT;
    }
    ds_p++;
}

