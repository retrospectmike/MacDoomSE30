# Doom SE/30 — Optimization Ideas

Prioritized list of remaining optimization opportunities. Update as ideas are tried,
confirmed, or ruled out. Mark status with: `[ ]` untried, `[x]` done, `[-]` tried/rejected.

---

## High Impact / Tractable

- [x] **Colormap/lighting skip per column** — `rw_scale` steps slowly across a seg so
  adjacent columns often land on the same `walllights[index]`. Track last index and
  skip `dc_colormap` + `mono_cm` recompute when unchanged. ~20 cycles saved per
  unchanged column. Low risk, small change. (2026-03-08)

- [-] **Double-buffer blit (MOVEM.L assembly)** — Hand-rolled 68030 MOVEM.L loop
  for the 12,800-byte memcpy flip. Assembly was correct but Basilisk II doesn't
  trigger display refresh from MOVEM.L writes to video RAM (dirty-page tracking
  issue). Snow showed HUD fine but savings marginal (~1–2 ticks on 7–28 total).
  Rejected 2026-03-09.

- [ ] **Optimize P_Ticker** — Profiled 2026-03-09: `ptick` = 98–100% of `logic` in every
  frame. G_Ticker overhead (input, M/AM/HU_Ticker) is only 1–2 ticks — P_Ticker is the
  entire logic cost. Typical ptick: 7–21 ticks/window, max 23. At render-light scenes
  (fast FPS), logic can be 50–80% of render cost — actionable. Next step: sub-profile
  inside P_Ticker to identify whether AI think (P_MobjThinker), movement (P_XYMovement),
  or sector specials (P_UpdateSpecials) dominate. Candidate: add per-category timers to
  `p_tick.c` / `p_map.c`.

---

## Medium Impact

- [x] **Double-buffer flip narrowing** — Full-row memcpy (64 bytes/row) overwrote
  the black background window margins left/right of the 320-wide game area. Narrowed
  to 40 bytes/row (game area only, starting at `xoff>>3`). Eliminates white border
  artifacts in release builds. Expected ~+0.8 FPS from fewer memory writes. (2026-03-13)

- **Blit bimodality** — Profiled 2026-03-13: blit cost is bimodal — ~7 ms on frames
  where `is_direct=1` (gameplay, GS_LEVEL, direct render path) and ~38.5 ms on frames
  where the full `blit8_sbar_thresh` path fires (menus, intermission, wipe frames).
  The 38.5 ms blit cost is now tied with segloop as the top per-frame cost in heavy
  gameplay. Further reduction would require either a faster threshold blit or skipping
  the full-screen blit on unchanged non-game frames (dirty-rect tracking).

- [ ] **2× mode: direct expand to real_fb_base (skip double-buffer flip)** — Profiled
  2026-03-14: the entire 2× performance gap vs non-2× (−1.6 FPS mean, −24%) is the
  expand+flip pipeline. 2× flip copies 18,432 bytes/frame (64×288 rows) vs 8,000 for
  non-2×. If we expand directly into `real_fb_base` instead of `fb_offscreen_buf`, the
  18KB flip is eliminated entirely. Risk: tearing if the Mac's 70.7 Hz raster catches
  the expand mid-write. With halfline (only 64 source rows → 128 dest rows in the view),
  the expand window is short; likely acceptable on Basilisk II, may show artifacts on real
  SE/30 at 16 MHz. Worth testing in Basilisk II first — if clean, a significant win.


- [x] **R_ScaleFromGlobalAngle — reduce FixedDiv calls** — **Done 2026-03-10.** Inlined
  via `SCALE_FROM_ANGLE` macro; short-seg fast path skips scale2 when stop-start≤2.
  Result: scale cost -40% on high-seg frames, 20–41% skip rate, FPS peak 8.4 (new Snow
  record). `prof_r_scale_skip` counter tracks hits.

- [x] **HUD rendering cost / ST_Drawer dirty-skip** — **Done 2026-03-10.** Snapshot-based
  skip in `ST_Drawer`: 13 fields compared; skip `ST_drawWidgets` entirely when unchanged.
  Result: st typical 3–6 → 0–2, FPS peak 9.3 (new Snow record). Still fires on combat
  frames (face changes every ~17 tics, health/ammo changes during damage). Correct behavior.

- [-] **R_CheckBBox tighter angle rejection** — Profiled 2026-03-10 (Snow + real SE/30).
  R_CheckBBox costs ~0.05 ticks/window (<1% of trav, <0.1% of frame). 72% of calls reach
  solidsegs scan, 27% angle-rejected — but scan itself is ≤5 loop iterations in QUAD mode
  (64 columns). Any early-out saves ~60 cycles/call × 78-203 calls/window = negligible.
  Not worth implementing.

---

## Bigger Projects / Longer Term

- [ ] **Pre-dithered WAD textures** — Offline-convert all textures to 1-bit, eliminating
  `COLMONO_GRAY` entirely (32-cycle double table-lookup per row). Biggest single
  remaining win but requires building the WAD converter tool. Would also eliminate
  the colormap lookup chain.

- [ ] **Sky/fog interaction** — Sky ceiling is special-cased and currently gets fogged
  out when fog_scale is set. Needs a separate fog path for sky sectors so the sky
  remains visible. See also TODO.md.

---

## Already Done / Rejected

- [x] **FixedMul inline macro** — Replaced function call with macro (Phase 2, 2026-02-26)
- [x] **FixedDiv2 → long long** — Eliminated FPU dependency (Phase 2, 2026-02-26)
- [x] **Direct 1-bit framebuffer renderers** — R_DrawColumn_Mono etc., skip intermediate buffer (Phase 4, 2026-03-01)
- [x] **68030 ASM pixel macros** — COLMONO_GRAY / COLMONO_BIT with SWAP, BSET/BCLR (2026-03-01)
- [x] **Flat floor/ceiling (solidfloor)** — Eliminates span drawing entirely (2026-03-01)
- [x] **Double-buffering** — Eliminates mid-frame flicker (2026-03-02)
- [x] **halfline rendering** — Render even rows only; odd rows copied (2026-03-02)
- [x] **iscale linear interpolation** — Replaces per-column 32-bit divide with linear step (2026-03-02)
- [x] **Affine texture column stepping** — Replaces per-column FixedMul with addition (2026-03-02)
- [x] **Visplane mark skip** — Suppress floor/ceiling visplane writes when solidfloor=1 (2026-03-02)
- [x] **QUAD mode (detailLevel=2)** — 4px-wide columns, nibble framebuffer writes (2026-03-07)
- [x] **BSP bbox fog culling** — Prune entire BSP subtrees beyond fog distance (2026-03-08)
- [x] **Seg-level fog fast path** — Skip texture/colfunc for fully-fogged segs (2026-03-08)
- [x] **QUAD nibble precomputed table** — Replace QUAD_NIBBLE 4×CMP/shift/OR with 1 table lookup (2026-03-08)
- [x] **Colormap/lighting index cache** — Skip `dc_colormap` update when `walllights[index]` unchanged across adjacent columns (2026-03-08)
- [-] **Pre-dithered texture columns (Option D)** — Tried and removed 2026-03-02: -3.7% FPS (slower) and visually broken (texture-space Bayer becomes perspective-warped spirals in screen space)
