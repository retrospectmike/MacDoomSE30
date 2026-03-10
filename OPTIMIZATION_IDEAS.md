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

- [x] **R_ScaleFromGlobalAngle — reduce FixedDiv calls** — **Done 2026-03-10.** Inlined
  via `SCALE_FROM_ANGLE` macro; short-seg fast path skips scale2 when stop-start≤2.
  Result: scale cost -40% on high-seg frames, 20–41% skip rate, FPS peak 8.4 (new Snow
  record). `prof_r_scale_skip` counter tracks hits.

- [ ] **HUD rendering cost** — Profiled 2026-03-09: `st` (ST_Drawer) = 2–9 ticks/window
  typical 3–6, runs every gameplay frame. `hu` (HU_Drawer) = 0–5, usually 0–2, spikes
  with active messages. Together account for 60–90% of `hud` budget. Unaccounted 2–5
  ticks = border draws + NetUpdate overhead. ST_Drawer at 3–6 ticks/window is small vs
  render (30–65) but non-negligible at fast-scene frame rates. Main candidate: add dirty
  flags to skip ST_Drawer redraw when nothing changed (health/ammo/keys/face unchanged).
  ST_Ticker already tracks some dirty state but ST_Drawer may not fully skip on no-change.
  Menu M_Drawer cost is low (2 ticks/frame when open — not the bottleneck).

- [ ] **R_CheckBBox tighter angle rejection** — Currently does full clip array scan per
  BSP node. Could add a screen-space angle rejection early-out for nodes clearly
  off to one side (before the clip array walk).

---

## Bigger Projects / Longer Term

- [ ] **Pre-dithered WAD textures** — Offline-convert all textures to 1-bit, eliminating
  `COLMONO_GRAY` entirely (32-cycle double table-lookup per row). Biggest single
  remaining win but requires building the WAD converter tool. Would also eliminate
  the colormap lookup chain.

- [ ] **Sky/fog interaction** — Sky ceiling is special-cased and currently gets fogged
  out when fog_scale is set. Needs a separate fog path for sky sectors so the sky
  remains visible. See also TODO.md.

- [ ] **Smaller view window** — Could try a narrower viewport (fewer columns per seg)
  but QUAD mode is already small — diminishing returns and visual quality degrades
  quickly.

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
