# doom_se30 TODO

## Visual / UI
- [ ] Improve menu text lightness/contrast
- [ ] **2× pixel scale** — render at half resolution, scale up 2× in blit; benchmark carefully vs current QUAD mode
- [ ] **Loading screen timing** — splash screen currently shows before D_DoomMain; WAD load happens after dismiss, so bar shows immediately. Need to show progress during actual load.
- [ ] **Settings/config dialog** — DLOG/DITL to expose fog_scale, detail level, solid floor settings in-game (user will build DITL in ResEdit)

## Input / Responsiveness
- [ ] Menu keystroke delay — reduce latency between keypress and menu response

## Rendering / Fog
- [ ] Sky visibility with fog — sky can be fogged out; improve sky/fog interaction so sky remains visible

## Stability / Exit
- [ ] **Monitor for CHK errors on exit** (Basilisk II) — observed once after black background window work. longjmp/ExitToShell architecture seems correct; if it recurs investigate QuickDraw teardown order.

## Bugs
- [x] ~~Sprites (barrels, non-enemy) culled much closer than fog distance~~ — Fixed 2026-03-13: QUAD mode rw_scale is <<detailshift vs raw xscale; fog comparison now uses (xscale << detailshift)
- [x] ~~Explosion sprites invisible at fog distance~~ — Fixed 2026-03-13: P_ExplodeMissile clears MF_MISSILE from thing->flags; secondary check on thing->info->flags identifies "born-as-missile" objects
- [x] ~~idclev cheat broken with doom2.wad~~ — Fixed 2026-03-13: epsd guard was rejecting episode 0 for commercial gamemode
