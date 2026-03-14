# doom_se30 TODO

## Visual / UI
- [ ] Improve menu text lightness/contrast
- [ ] **2× pixel scale** — render at half resolution, scale up 2× in blit; benchmark carefully vs current QUAD mode
- [ ] **Loading screen timing** — splash screen currently shows before D_DoomMain; WAD load happens after dismiss, so bar shows immediately. Need to show progress during actual load.
- [ ] **Settings/config dialog** — DLOG/DITL to expose fog_scale, detail level, solid floor settings in-game (user will build DITL in ResEdit)

## Input / Responsiveness
- [x] ~~Menu keystroke delay~~ — Fixed 2026-03-13: cache 1-bit background on first menu frame; subsequent frames restore via memcpy instead of 8000 blit8_sbar_thresh calls

## Rendering / Fog
- [ ] **Sky never fogs** — sky should be immune to fog; currently part of sky shows solidfloor fill pattern when fog is active (sharp cutoff, scales with fog distance). Two approaches tried and reverted: (1) per-column top/bot range tracking in R_RenderSegLoop + fallback pass in R_DrawPlanes; (2) per-column boolean flag + re-render of sky visplane entries at end of R_DrawPlanes. Neither produced correct results. Root cause not yet identified — sky visplane entries appear correctly written but some columns still render as fog fill.

## Stability / Exit
- [ ] **Monitor for CHK errors on exit** (Basilisk II) — observed once after black background window work. longjmp/ExitToShell architecture seems correct; if it recurs investigate QuickDraw teardown order.

## Bugs
- [ ] **Invisibility powerup weapon misrender** — when player has invisibility, weapon sprite renders skewed and displaced to the left. All weapons affected. Old bug, not introduced recently. Fuzz path (`R_DrawFuzzColumn_Mono`) suspected.
- [x] ~~Sprites (barrels, non-enemy) culled much closer than fog distance~~ — Fixed 2026-03-13: QUAD mode rw_scale is <<detailshift vs raw xscale; fog comparison now uses (xscale << detailshift)
- [x] ~~Explosion sprites invisible at fog distance~~ — Fixed 2026-03-13: P_ExplodeMissile clears MF_MISSILE from thing->flags; secondary check on thing->info->flags identifies "born-as-missile" objects
- [x] ~~idclev cheat broken with doom2.wad~~ — Fixed 2026-03-13: epsd guard was rejecting episode 0 for commercial gamemode
