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

- [x] **P_CheckSight cache** — 64-entry hash table in `p_sight.c` caches BSP raycast
  results for 4 game tics (114ms). Eliminates repeated `P_CrossBSPNode` walks for
  the same monster→target pair. Result on Plutonia MAP20: logic dropped from 50–69%
  to 43–61% of frame time, ~7% FPS gain. Monsters react at most 4 tics late to
  player visibility changes — barely perceptible. (2026-03-21)

- [ ] **Optimize P_Ticker further** — Profiled 2026-03-21 on Plutonia MAP20: even with
  sight cache, logic is 43–61% of frame time on monster-dense maps. Remaining cost is
  `P_Move`/`P_TryMove` (blockmap collision per monster per tic) and `P_MobjThinker`
  (state machine, gravity). Candidate: throttle `P_Move` for distant monsters, or
  reduce collision check frequency. Would change gameplay behavior.

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

- [-] **Direct framebuffer rendering (skip double-buffer flip)** — Tested 2026-03-25
  for the non-2× path (`opt_directfb`). Eliminated the 8,000-byte flip (40×200, 10
  unrolled MOVE.L/row) by rendering directly to video RAM.

  **Result: 20% REGRESSION** (mean 5.3 vs 6.6, median 5.1 vs 6.4, min 1.5 vs 3.8).
  Also produced visible full-black flash every frame from solidfloor's per-frame
  `memset` of the view area (line 894, `r_main.c`) — previously invisible behind the
  double buffer.

  **Key finding — why the double buffer is FASTER than direct rendering:**
  The double buffer batches all video RAM writes into one tight sequential burst
  (2,000 MOVE.L in an unrolled loop). Without it, video RAM gets hit by hundreds of
  scattered column renderer calls (BSET/BCLR = read-modify-write, 2 bus cycles each),
  solidfloor fills, and span writes — all at unpredictable addresses.

  Two hypotheses for the slowdown (not yet confirmed which dominates):

  **(A) 68030 data cache effect.** If System 7.5 enables the data cache (CACR bit 8),
  writes to the offscreen buffer in main RAM benefit from cache-line locality — nearby
  reads (e.g. RMW for bit operations) hit cache (~4 cycles) instead of external bus
  (~20+ cycles). The sequential flip to video RAM is write-through regardless but is
  maximally bus-efficient. Scattered direct writes to video RAM get zero cache benefit
  on reads. **Status: unconfirmed — need to read CACR at startup to verify data cache
  is actually enabled.**

  **(B) Emulator dirty-page tracking artifact.** On real SE/30 hardware, video RAM IS
  main RAM — same DRAM, same bus. Basilisk II / Snow track writes to the video RAM
  address range to know which screen regions to redraw on the host. Hundreds of
  scattered writes = hundreds of dirty-page checks on the host side. One sequential
  flip = one contiguous dirty region. This overhead is emulator-only and would not
  exist on real hardware. **Status: unconfirmed — need to test directfb on real SE/30.**

  **To confirm:** (1) Read CACR register at startup and log data cache state.
  (2) Test directfb on real SE/30. If directfb is same speed on real HW but slower
  in emulator, hypothesis B is the cause.

  The 2× path (18,432-byte flip) was not tested. The larger flip cost may still make
  direct rendering worthwhile for 2×, but the same scattered-write penalty applies.
  Unlikely to be a net win without addressing the solidfloor pre-clear.


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

- [x] **68030 I-cache optimization (segloop + colfunc)** — **Done 2026-03-28.** Fit
  `R_RenderSegLoop` (542→230 B) and `R_DrawColumnQuadLow_Mono` (424→82 B) into the
  256-byte I-cache via: removed `-funroll-loops` from r_segs.c and r_draw.c, outlined
  cold paths (visplane marking, non-affine texcol), cached globals as locals to eliminate
  6-byte absolute-address loads (20 per loop iteration → 6), inlined `R_GetColumn`.
  Result: mean +9.7%, peak 12.0 FPS (new Snow record), segloop −19%, render −8.7%.
  Applicable pattern: any inner loop >256 B with global reloads after indirect calls
  or `char*` writes benefits from local caching.

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
- [-] **Iterative BSP traversal** — Replaced recursive `R_RenderBSPNode` with explicit
  stack + early-out solidsegs check. Result: **10% regression** (5.91 vs 6.6 mean FPS).
  Likely cause: larger code footprint thrashing 68030's 256-byte instruction cache.
  Recursive version's tiny code body fits in cache; iterative version with two stack
  arrays and inner pop loop does not. Reverted 2026-03-21.
- [x] **R_PointToDist elimination (rw_offset)** — **Done 2026-03-26.** Replaced
  `R_PointToDist` (2× FixedDiv ~280 cycles) + `FixedMul` (~44 cycles) with direct
  dot-product of (view−v1) onto wall direction (2× FixedMul ~88 cycles) for `rw_offset`
  in `R_StoreWallRange`. `rw_distance` was already dot-product (Phase 2C).
  Previously bundled with iterative BSP and reverted (2026-03-21); retested in isolation.
  Result: render −11.5%, BSP −13.8%, peak 9.4 FPS (new Snow high). `R_PointToDist`
  now has zero callers (dead code, kept in place).
- [x] **Bulk sprite init I/O** — `R_InitSpriteLumps` replaced 1381 individual `lseek`+`read`
  calls with single 3.6 MB bulk read into temp buffer, headers parsed in-memory.
  Reduces startup time on SCSI. (2026-03-21)
- [x] **68030 I-cache: segloop + colfunc** — Fit R_RenderSegLoop (542→230 B) and
  R_DrawColumnQuadLow_Mono (424→82 B) into 256-byte I-cache. Removed -funroll-loops,
  outlined cold paths, cached globals as locals, inlined R_GetColumn. +9.7% mean FPS,
  peak 12.0. (2026-03-28)
