# doom_se30 TODO

## Pre-1.0

### Visual / UI
- [x] ~~**Loading screen**~~ — bold centered text on bg_window before D_DoomMain; done.
- [x] ~~**Menu text contrast**~~ — blit8_menu with luminance threshold (menu_thresh=88); done.
- [x] ~~Settings/config dialog~~ — DLOG/DITL 129 with checkboxes, popups, edit fields; saves via doom.cfg
- [x] ~~doom_dither.cfg consolidated~~ — dither params now in doom.cfg via M_LoadDefaults/M_SaveDefaults
- [x] ~~App icon~~ — BNDL/FREF/ICN# with creator code DMSE

## Post-1.0

### Sound
- [ ] **Activate sound** - Definitely make it optional as FPS hit will likely be there;  Track its impact on performance.

### Performance / Hardware
- [ ] **Ensure dcache enabled on 68040 systems** — Quadra 650 timedemo (2026-04-05) showed `CACR=0x80008000` (icache ON, dcache OFF). dcache off measurably hurts column renderer texture fetches. Need to enable dcache in `I_Init` on 68040; SE/30 (68030) has no dcache so this is 040-only. Check `SwapDataCache` logic — it may be intentionally disabled for compatibility.
- [x] ~~**Mac IIci / Quadra crash**~~ — Root cause: `NewPtr` 24-bit truncation in `I_ZoneBase`. Without Mode32/32-bit addressing, `NewPtr(48MB)` etc. truncate size to 24 bits → `NewPtr(0)` → returns valid non-NULL zero-byte block → Z_Init treats it as a 48MB zone → walks into system memory → corrupts interrupt dispatch chain → crash. Fix: `MaxMem()` guard before allocation loop; skips any size exceeding largest contiguous free block. Fixed 2026-04-05. The Gestalt `_HWPriv` guard added during investigation is also correct and kept.

### Rendering
- [x] ~~**2× pixel scale**~~ — Implemented 2026-03-14.
- [ ] **Sky fog** (nice-to-have) — sky should be immune to fog; currently shows solidfloor fill at fog distance. Two approaches tried and reverted. Root cause unknown. Could slip to post-1.0.
- [ ] **Keep optimizing** for more FPS esp to bring the low end up.  See docs OPTIMIZATION_IDEAS.md and PERFORMANCE_HISTORY.md for extensive records.
- [ ] **Fix sky clipping** rendering bugs when OPTION "solid fill ceiling/floor" texture rendering is set to OFF
- [x] ~~**Check transparent wall texture rendering bug** in E1M1~~ — Fixed 2026-03-31: cached `spryscale` local in `R_RenderMaskedSegRange` wasn't written back to global before `R_DrawMaskedColumn` call

### Options GUI
- [ ] **Expose tunable options in-game** — monster_throttle_dist, fog_scale, solidfloor_gray, halfline, etc. Currently only settable via doom.cfg or hotkeys.

### Custom WADs
- [x] ~~**Single PWAD file picker**~~ — splash dialog button (item 5) opens StandardGetFile, selected filename shown in item 6, passed as `-file` to engine. PWAD must be in the same folder as the app.
- [ ] **Support selecting and loading multiple PWAD files** — currently limited to one PWAD. Extend to allow picking multiple files (loop StandardGetFile, display list, expand mac_argv).

### Save/Load
- [ ] **Load game crashes** — menu Load Game causes a crash. Save game may or may not work.

### HUD
- [ ] **HUD area flashes black** — when view size is maxed out (full screen, no HUD), the status bar region flashes to black and back during gameplay. Timed with HUD redraw (periodic updates + triggered changes like firing a gun). Holding fire makes the region permanently black.

### Stability / Exit
- [x] ~~**Missing WAD detection**~~ — shows message on bg_window + waits for click/key, then clean exit; done.
- [ ] **Monitor for CHK errors, other crashes on exit** (Basilisk II and Snow) — observed once; likely transient

## Bugs Fixed
- [x] ~~Invisibility powerup weapon misrender~~ — Fixed 2026-03-15: fuzz path corrected
- [x] ~~Sprites culled much closer than fog distance~~ — Fixed 2026-03-13
- [x] ~~Explosion sprites invisible at fog distance~~ — Fixed 2026-03-13
- [x] ~~idclev cheat broken with doom2.wad~~ — Fixed 2026-03-13
- [x] ~~Menu keystroke delay~~ — Fixed 2026-03-13

## Detail Levels
- [x] ~~MUSH mode (detailLevel=3)~~ — Implemented but visually unusable (~1 FPS gain, 28-column incoherence). Code retained, not recommended.
