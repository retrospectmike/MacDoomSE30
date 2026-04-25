# Doom SE/30 — Performance History

Chronological record of FPS and profiling data across optimization milestones.
Newest entries at top. Add new entries here after each significant change.

**Emulator key:**
- **Basilisk II** — Mac IIci emulation, faster than real HW, useful for iteration
- **Snow** — Closer to real SE/30 timing, more representative
- **Real SE/30** — 68030 @ 16 MHz, the actual target

---

## 2026-04-10 — SE/30 mono vs IIcx color parity confirmed
**Emulator: Snow Mac SE/30 (gestalt=9) + Snow Mac IIcx (gestalt=8), release build**
**Config: detailLevel=2 halfline=1 affinetex=1 solidfloor=1 solidfloor_gray=4**
**WAD: doom.wad registered, timedemo pl20ben.lmp | 71 samples (mono) / 72 samples (color)**

Side-by-side of both Snow configurations at identical settings. Both machines are 16 MHz 68030.

| Metric | Snow SE/30 mono | Snow IIcx color | Delta |
|--------|-----------------|-----------------|-------|
| machine | gestalt=9 | gestalt=8 | — |
| depth | 1-bit mono | 8-bit color | — |
| blit path | MOVE.L double-buf | CopyBits NuBus | — |
| **FPS mean** | **6.74** | **6.90** | −2% |
| FPS range | 2.0–14.4 | 2.2–11.6 | — |
| logic (mean) | 17.77 | 17.74 | ≈0 |
| render (mean) | 44.99 | 45.72 | +2% |
| — setup | 6.6 | 4.8 | |
| — bsp | 31.3 | 33.5 | |
| — planes | 0.2 | 0.3 | |
| — masked | 5.7 | 5.4 | |
| **blit (mean)** | **10.23** | **5.88** | **−43%** |
| hud (mean) | 6.27 | 6.17 | ≈0 |
| sound (mean) | 0.08 | 0.11 | ≈0 |
| cy/px (walls) | 54.0 | n/a | — |

**FPS and render are identical within noise** — same clock speed, same renderer, same scene.
Confirms the 2026-04-06 parity result with full profiling data from both machines.

**Blit is the only material difference**: mono MOVE.L double-buffer (10.23 ticks mean)
is costlier than color CopyBits (5.88 ticks). The mono blit variance is wide (1–26 ticks)
because `is_direct` frames skip most of the flip; full-screen transitions (wipes, menus)
push it to 26. Color always does a full CopyBits regardless of scene state.

**cy/px=54** (mono only, walls) — 54 cycles per wall pixel at 16 MHz = 3.4 µs/pixel for
the 1-bit packed-nibble column renderer. Color path does not track `prof_r_pixels`.

---

## 2026-04-10 — NuBus CopyBits fix + instrumentation removal (first clean color run)
**Emulator: Snow Mac IIcx (gestalt=8), release build (no doom_log overhead)**
**Config: detailLevel=2 halfline=1 affinetex=1 solidfloor=1 solidfloor_gray=4**
**WAD: doom.wad registered, timedemo pl20ben.lmp | 72 samples**

### Changes

- **NuBus crash fix** (`i_video_mac.c`): Direct `memcpy`/`MOVE.L` to `0xf9000a00`
  (NuBus VRAM) in 24-bit addressing mode strips bits 24–31 → writes to `0x000a00`
  (system heap) → crash at frame 24. Fixed by routing through `CopyBits` on
  non-SE/30 machines (`s_use_copybits = machine_type != kMacModelSE30`). CopyBits
  uses the ROM slot manager and is safe in any addressing mode.
- **Instrumentation removed** (`d_main.c`, `i_video_mac.c`): 25+ `doom_log_flush()`
  (FlushVol) calls per frame were adding ~110 ticks/window (~60% of all frame time)
  on Snow's ExtFS-backed filesystem. Removed all step-marker doom_logs; FPS profiling
  system retained.

### Results

| Metric | Prev color (w/ instrumentation) | **This run** | IIcx 1-bit (2026-04-06) |
|--------|----------------------------------|--------------|--------------------------|
| FPS mean | 1.50 | **6.90** | 6.77 |
| FPS range | 1.4–1.6 | **2.2–11.6** | — |
| render ticks (mean) | 49.0 | **45.72** | 44.8 |
| blit ticks (mean) | 37.0 | **5.88** | 7.1 |
| hud ticks (mean) | 114.4 | **6.17** | — |
| sound ticks (mean) | 32.1 | **0.11** | — |

render breakdown: setup=4.8 bsp=33.5 planes=0.3 masked=5.4

**4.6× FPS improvement** from removing instrumentation overhead. The prior
blit=37 and hud=114 were almost entirely `FlushVol` I/O inside the timed
sections, not actual render/blit work.

Color 8-bit (6.90 FPS) is statistically identical to 1-bit mono (6.77) — consistent
with the 2026-04-06 parity result. Render is the bottleneck at ~65% of frame time in
both modes.

sound ≈ 0 in this run (vs 32 ticks with instrumentation) confirms the sound
update timings in the previous color run were entirely instrumentation overhead.

---

## 2026-04-06 — Color Renderer Parity (halfline + solidfloor + affinetex + QUAD)
**Emulator: Snow Mac IIcx (gestalt=8), debug build**
**Config: detailLevel=2 halfline=1 affinetex=1 solidfloor=1 solidfloor_gray=4**
**WAD: doom1.wad E1M1, timedemo pl20ben.lmp**

Ported all mono renderer performance opts to the 8-bit color path and removed the block
that disabled them for color displays.

### Changes

**`r_draw.c`** — halfline support added to all color column renderers:
- `R_DrawColumn`, `R_DrawColumnLow`: skip odd screen rows, double `fracstep`, iterate every 2 rows
- `R_DrawColumnQuadColor`, `R_DrawColumnMushColor`: same pattern
- `R_DrawSpan`, `R_DrawSpanLow`, `R_DrawSpanQuadColor`, `R_DrawSpanMushColor`: early return on odd rows

**`r_main.c`** — scanline doubling for color: after all rendering, copies each even row to
the adjacent odd row in `screens[0]` (view area only), matching the mono double-buffer pass.

**`d_main.c`** — removed the block that zeroed `opt_halfline`, `opt_affine_texcol`,
`opt_solidfloor`, and clamped `detailLevel` to 1 on color displays. `opt_scale2x` and
`fog_scale` remain suppressed (no color path for either).

### Results

Three-way comparison on same machine (Snow IIcx) + SE/30 reference:

| Metric | Old color | **New color** | IIcx 1-bit | SE/30 1-bit |
|--------|-----------|---------------|------------|-------------|
| detailLevel | 1 (LOW) | **2 (QUAD)** | 2 (QUAD) | 2 (QUAD) |
| halfline | off | **on** | on | on |
| solidfloor | off | **on** | on | on |
| FPS mean | 3.50 | **6.75** | 6.77 | 5.46 |
| FPS median | 3.1 | **6.2** | 6.4 | ~5.4 |
| render ticks (mean) | 103.0 | **45.5** | 44.8 | 52.0 |
| blit ticks (mean) | 5.9 | **7.1** | 10.2 | ~10 |
| logic ticks (mean) | 16.5 | 17.7 | 17.7 | — |

**+93% FPS** over old disabled color path. New color is statistically identical to
1-bit mono on the same machine (6.75 vs 6.77 — within noise). Render cost is equal
at the same detail level (45.5 vs 44.8 ticks); blit is cheaper for color (plain
memcpy vs Bayer dither: 7.1 vs 10.2 ticks).

IIcx is 1.24× faster than SE/30 (6.75 vs 5.46); render ratio is 1.14× (52.0 vs
45.5). Clock speed advantage (25 vs 16 MHz = 1.56×) is not fully realised —
consistent with NuBus VRAM write penalty on IIcx vs on-board VRAM on SE/30.

**Verdict: color and mono are now at parity. All scene-complexity performance
characteristics are the same; only the blit path differs (color slightly cheaper).**

---

## 2026-04-05 — dc_iscale Exact Divide + Affine Midpoint Subdivision (r_segs.c)
**Emulator: Snow (debug build)**
**Config: detailLevel=2 halfline=1 solidfloor=1 solidfloor_gray=4**
**WAD: doom1.wad E1M1, timedemo pl20ben.lmp**

Two correctness fixes with net-positive or neutral performance impact.

### dc_iscale: exact per-column divide

`dc_iscale` was linearly interpolated across wall segments using a pre-computed step.
`1/scale` is not linear in screen x — the midpoint of the interpolated range has a bow
error proportional to segment width. Visible as fisheye bowing on angled walls and as
colorful noise in masked/transparent textures (wrong iscale → index advances past post
pixel data into garbage).

**Fix:** `dc_iscale = 0xffffffffu / (unsigned)l_scale` per column in both
`R_RenderSegLoop` (walls) and `R_RenderMaskedSegRange` (masked). Linear step vars removed.

| Metric | linear interp | exact divide | Delta |
|--------|---------------|--------------|-------|
| Mean FPS | 5.44 | **5.45** | **+0.2% (noise)** |
| Segloop ticks (mean) | ~20.7 | ~21.7 | +4.8% |

Segloop cost uptick is real but small — extra 32-bit divide per column. Wall renderer is
not the bottleneck; overall FPS unchanged.

### Affine texture column stepping: midpoint subdivision

`opt_affine_texcol` replaces the exact `finetangent`-based `texturecolumn` with a linear
step. `finetangent` is nonlinear in screen x → horizontal texture warp on angled walls and
wrong column on masked textures (wrong transparency strip).

**Fix:** `#define AFFINETEX_MIDPOINT 1` in r_segs.c. `R_StoreWallRange` now computes exact
`texturecolumn` at x0, xm=(x0+x1)/2, and x1 (2 extra FixedMuls per segment). The loop
resets `l_texcol`/`l_texstep` to the exact midpoint values when `x == l_texmid_x`. Error
reduces 4× (each half-segment is half-width → ¼ bow error).

Three-way comparison (all runs: same timedemo, debug build, affinetex ON except "OFF" col):

| Metric | old linear affine | **midpoint affine** | affine OFF |
|--------|-------------------|---------------------|------------|
| Mean FPS | 5.44 | **5.46** | 5.33 |
| Render ticks (mean) | 51.9 | 52.0 | 53.0 |
| BSP ticks (mean) | 35.8 | 35.6 | 37.0 |
| Segloop ticks (mean) | 22.2 | **21.7** | 22.9 |
| cy/px (mean) | 44.2 | **43.1** | 45.7 |

Midpoint affine is strictly better than old linear (+0.4% FPS, −2.3% segloop) and faster
than affine OFF (+2.0% FPS). The 2 extra FixedMuls per segment are amortized across all
columns in that segment; per-column overhead is negligible.

**Verdict: keep exact dc_iscale divide; keep midpoint affine as default (opt_affine_texcol=1).**

---

## 2026-03-31 — Zone Memory Usage Survey (doom1.wad)
**Emulator: Snow (debug build)**
**Zone heap: 8 MB (8192 KB)**

Measured `Z_FreeMemory()` after each level load across all 27 maps (3 episodes × 9 maps).

| Map | Free (KB) | Used (KB) | Used % |
|-----|-----------|-----------|--------|
| E1M1 | 7782 | 410 | 5.0% |
| E1M2 | 7637 | 555 | 6.8% |
| E1M3 | 7644 | 548 | 6.7% |
| E1M4 | 7705 | 487 | 5.9% |
| E1M5 | 7708 | 484 | 5.9% |
| E1M6 | 7571 | 621 | 7.6% |
| E1M7 | 7671 | 521 | 6.4% |
| E1M8 | 7743 | 449 | 5.5% |
| E2M1 | 7793 | 399 | 4.9% |
| E2M2 | 7540 | 652 | 8.0% |
| E2M3 | 7687 | 505 | 6.2% |
| E2M4 | 7650 | 542 | 6.6% |
| E2M5 | 7596 | 596 | 7.3% |
| E2M6 | 7654 | 538 | 6.6% |
| E2M7 | 7530 | 662 | 8.1% |
| E2M8 | 7815 | 377 | 4.6% |
| E2M9 | 7862 | 330 | 4.0% |
| E3M1 | 7844 | 348 | 4.2% |
| E3M2 | 7767 | 425 | 5.2% |
| E3M3 | 7654 | 538 | 6.6% |
| E3M4 | 7596 | 596 | 7.3% |
| E3M5 | 7589 | 603 | 7.4% |
| E3M6 | 7689 | 503 | 6.1% |
| E3M7 | 7696 | 496 | 6.1% |
| E3M8 | 7860 | 332 | 4.1% |
| E3M9 | 7760 | 432 | 5.3% |

**Peak: E2M7 at 662 KB (8.1% of 8 MB zone).** All maps well under 1 MB. A 4 MB zone would have ample headroom for doom1.wad.

---

## 2026-03-29 — Monster Sight Distance Culling (p_enemy.c)
**Emulator: Snow (debug build)**
**Config: detailLevel=2 halfline=1 affinetex=1 solidfloor=1 solidfloor_gray=4**
**WAD: Plutonia MAP20 (high monster density)**

Added `monster_sight_dist` (doom.cfg, default 1600 map units ≈ 50m): cheap `P_AproxDistance`
check in `P_LookForPlayers` before the expensive `P_CheckSight` BSP raycast. Monsters beyond
the threshold skip the sight trace entirely and stay idle. Configurable; 0 = vanilla (no limit).

Sub-profiling of `P_SetMobjState` action functions revealed A_Look was the #1 logic cost on
monster-dense maps — ~187 idle monsters per window each running P_CheckSight every tic via
`P_LookForPlayers`. Most of these traces were for monsters far beyond engagement range.

Comparison: playdemo baseline (before) vs manual gameplay (after). Demo desync prevents
identical playback (the optimization changes which monsters wake up), but combat intensity
(A_Chase call count ~195) is comparable across both runs.

| Metric | Before | After | Delta |
|--------|--------|-------|-------|
| Mean FPS (combat) | 1.93 | **2.42** | **+25%** |
| Min FPS | 1.3 | 1.5 | +0.2 |
| Max FPS | 2.6 | **3.9** | **+1.3** |
| ptick (mean) | 100 | **75** | **−25%** |
| A_Look ticks (mean) | 25 | **8** | **−68%** |
| A_Chase ticks (mean) | 20 | 17 | −15% |
| st total (mean) | 56 | **38** | **−32%** |

Note: A_Look call count unchanged (~187/window) — the P_AproxDistance early-out is so cheap
it doesn't register in tick-granularity profiling. The savings come entirely from skipping
the P_CheckSight BSP trace for distant monsters.

**Verdict: keep. Largest logic optimization to date on monster-dense maps.**

---

## 2026-03-28 — 68030 I-Cache Optimization (r_segs.c, r_draw.c, CMakeLists.txt)
**Emulator: Snow (debug build)**
**Config: detailLevel=2 halfline=1 affinetex=1 solidfloor=1 solidfloor_gray=4**

Systematic optimization to fit the two hottest inner loops — `R_RenderSegLoop` (column
dispatch) and `R_DrawColumnQuadLow_Mono` (pixel rendering) — into the 68030's 256-byte
instruction cache. Three techniques applied:

1. **Removed `-funroll-loops`** from r_segs.c and r_draw.c. GCC's 4× unrolling expanded
   loops well past 256 bytes. The source already has manual 2× unrolling where beneficial.
2. **Outlined cold paths** from the segloop: visplane marking (`R_MarkVisplanes`, noinline)
   and non-affine texture column computation (`R_CalcTexColNonAffine`, noinline). These
   are rarely/never taken with solidfloor=1 and affinetex=1.
3. **Cached globals as locals** in both functions. GCC reloads globals after every indirect
   call (`colfunc()`) or `char*` write (`*dst = ...`) due to aliasing rules. Copying
   loop-invariant globals (`rw_scalestep`, `rw_stopx`, `fog_scale`, `midtexture`,
   `walllights`, etc.) and stepped globals (`rw_scale`, `topfrac`, `bottomfrac`, `rw_x`)
   to locals lets GCC keep them in registers across the entire loop.
4. **Inlined `R_GetColumn`** into r_segs.c as `R_GetColumn_Fast` (static inline), eliminating
   ~42 cycles of JSR/RTS overhead per column per texture tier.

Inner loop sizes (disassembled):

| Loop | Before | After | I-cache fit? |
|------|--------|-------|-------------|
| R_RenderSegLoop (midtexture) | 542 B (unrolled) | **230 B** | **Yes** (26 B spare) |
| R_DrawColumnQuadLow_Mono (halfline) | 424 B (unrolled) | **82 B** | **Yes** (174 B spare) |

| Metric | Dot-product baseline | **I-cache optimized** | Delta |
|--------|---------------------|----------------------|-------|
| Frames | 52 | 70 | — |
| Mean FPS | 5.54 | **6.08** | **+9.7%** |
| Median FPS | — | **5.7** | — |
| Min FPS | 1.6 | 1.5 | — |
| Max FPS | 9.4 | **12.0** | **+2.6 (new Snow record)** |
| Render ticks (mean) | 41.6 | **38.0** | **−8.7%** |
| BSP ticks (mean) | 30.7 | **27.6** | **−10.1%** |
| Segloop ticks (mean) | ~19* | **15.3** | **−19%** |

*Segloop not separately tracked in baseline; estimated from intermediate build.

Key insight: on the 68030, the cost of a 6-byte absolute-address global load (`movel 0x...,%d0`)
is not just the memory access — it inflates loop code size by ~6 bytes per access. The
R_RenderSegLoop had 20 such accesses (120 bytes = 38% of the loop). Replacing them with
2-byte register ops (`addql`, `cmpl %sp@(N)`) simultaneously makes the code faster AND
smaller, creating a virtuous cycle with the I-cache.

**Verdict: keep. Largest single optimization since Phase 4 direct 1-bit renderers.**

---

## 2026-03-26 — R_PointToDist Dot-Product Elimination (r_segs.c)
**Emulator: Snow (debug build)**
**Config: detailLevel=2 halfline=1 affinetex=1 solidfloor=1 solidfloor_gray=4**

Replaced `R_PointToDist` (2× FixedDiv + 1× FixedMul ≈ 460 cycles) with direct
dot-product of `(viewx−v1x, viewy−v1y)` onto wall direction vector (2× FixedMul ≈ 88
cycles) for `rw_offset` computation in `R_StoreWallRange`. The `rw_distance` computation
was already converted to dot-product in Phase 2C. This change was previously bundled
with the iterative BSP rewrite (which regressed 10%) and reverted together. Retested
in isolation — the dot-product itself is a clear win.

| Metric | Previous build | Dot-product build | Delta |
|--------|---------------|-------------------|-------|
| Frames | 80 | 52 | — |
| Mean FPS | 5.23 | **5.54** | **+5.9%** |
| Min FPS | 1.5 | 1.6 | +0.1 |
| Max FPS | 8.4 | **9.4** | **+1.0 (new Snow high)** |
| Render ticks (mean) | 47.0 | **41.6** | **−11.5%** |
| BSP ticks (mean) | 35.6 | **30.7** | **−13.8%** |
| Segs/frame (mean) | 133.6 | 124.8 | (scene variation) |

Savings: ~370 cycles per textured seg × ~130 segs/frame ≈ 48K cycles/frame.
`R_PointToDist` now has zero callers (dead code, kept in r_main.c).
**Verdict: keep.**

---

## 2026-03-25 — Direct Framebuffer (directfb) — REGRESSION
**Emulator: Snow (debug build)**
**Config: detailLevel=2 halfline=1 affinetex=1 solidfloor=1 solidfloor_gray=4 directfb=1**

Tested skipping the double-buffer flip by rendering directly to video RAM (`opt_directfb=1`).
Eliminated the 8,000-byte per-frame memcpy (200 rows × 40 bytes, 10 unrolled MOVE.L/row).

| Metric | Double-buffer (baseline 03-13) | directfb (this run) | Delta |
|--------|-------------------------------|---------------------|-------|
| Frames | ~90 | 57 | — |
| Mean FPS | **6.6** | **5.3** | −1.3 (−20%) |
| Median FPS | **6.4** | **5.1** | −1.3 (−20%) |
| Min FPS | 3.8 | 1.5 | −2.3 |
| Max FPS | 10.0 | 9.1 | −0.9 |

**Visual:** full-black flash every frame from solidfloor's per-frame `memset` of the
view area (`r_main.c:894`) — previously invisible behind the double buffer.

**Root cause (under investigation):** The double buffer consolidates all video RAM
writes into one fast sequential burst (2,000 MOVE.L). Without it, video RAM is hit by
hundreds of scattered column/span renderer writes (BSET/BCLR read-modify-write) at
unpredictable addresses. Two hypotheses: (A) 68030 data cache locality benefit for
offscreen buffer writes in main RAM, lost when writing directly to video RAM; (B)
emulator dirty-page tracking overhead on scattered video RAM writes (would not exist on
real SE/30 where video RAM = main RAM). See OPTIMIZATION_IDEAS.md for full analysis.

**Verdict: rejected.** Double-buffer flip is not just flicker prevention — it's a
performance optimization that batches slow scattered writes into one fast sequential copy.

---

## Configuration Options Reference — 2026-03-14

All SE/30-specific options, their ranges, typical test values, and performance impact.

| Option | Description | New? | Default | Range | Keys | Perf Impact | Typical Test Values |
|---|---|---|---|---|---|---|---|
| `halfline` | Renders every other row and copies it down, halving vertical pixel work | NEW | 1 (ON) | 0/1 | — | **+30–40%** render speed (when ON) | 1 |
| `affinetex` | Affine texture mode: eliminates per-pixel perspective divide for wall textures. Textures may swim slightly on close walls but imperceptible at distance | NEW | 0 (OFF) | 0/1 | — | **+5–10%** on wall-heavy scenes (when ON) | 1 |
| `solidfloor` | Solid gray floor/ceiling instead of textured flats. Eliminates all span drawing | NEW | 0 (OFF) | 0/1 | — | **+15–25%** render speed (when ON) | 1 |
| `solidfloor_gray` | Gray shade used for solid floor/ceiling and distance fog fill | NEW | 0 | 0–4 | `Z` cycle | Negligible | 4 |
| `fog_scale` | Distance fog: walls/sprites beyond threshold rendered as solid gray. Cuts render cost proportional to fog density | NEW | 0 (OFF) | 0–65536 | `` ` `` / `\` | **Up to +40%** in open areas (when ON, high fog) | 0 (off) |
| `detailLevel` | Column pixel width. 2=QUAD (4px wide columns, 4× fewer pixels rendered). Looks blocky but dramatically faster | NEW | 2 (QUAD) | 0–2 | — | **+200–300%** at QUAD vs full-res | 2 |
| `screenblocks` | View window size. DO NOT INCLUDE IN DIALOG BOX. Exposed via standard in-game size slider | NOT NEW | 7 | 1–11 | `-` / `+` | Moderate per step | 7 |
| `scale2x` | 2× pixel scale. Renders at half resolution and doubles each pixel. Gives chunky retro look; costs ~24% FPS | NEW | 0 (OFF) | 0/1 | — | **−24%** FPS (when ON) | 0 |
| `no_lighting` | Disables per-column lighting/colormap lookup. Flat uniform brightness; slightly faster column renderer | NEW | 0 (OFF) | 0/1 | `L` | **+3–5%** (when ON) | 0 |
| **Gamma** | Brightness curve exponent. <1.0 brightens midtones; >1.0 darkens them. 0.52 is the tuned default for SE/30 monochrome | NEW | 52 (→0.52) | 5–300 (×0.01) | `O` − / `P` + | None | 52 |
| **Black point** | Input black-point for contrast mapping. Lower = more pixels treated as black | NEW | 55 | 0–245 | `[` − / `]` + | None | 55 |
| **White point** | Input white-point for contrast mapping. Higher = more pixels treated as white. Must stay ≥10 above black point | NEW | 160 | 10–255 | `;` − / `'` + | None | 160 |

---

## 2026-03-21 — P_CheckSight Cache + Bulk Sprite Init + Key Fixes
**Emulator: Snow (debug build)**
**WAD: TNT, tested on Plutonia MAP20 (high monster density)**

### Changes
- **P_CheckSight cache** (`p_sight.c`): 64-entry hash table caches BSP raycast results
  for 4 game tics (114ms). Same monster→target pair returns cached result without
  repeating the full `P_CrossBSPNode` walk. Reject table still checked every call.
- **Bulk sprite init** (`r_data.c`): `R_InitSpriteLumps` replaced 1381 individual
  `lseek`+`read` calls with a single 3.6 MB bulk read. Headers parsed from buffer.
  Reduces startup time on SCSI.
- **Key table fix** (`i_input_mac.c`): added `9` (0x19) and `0` (0x1D) to kKeyTable.
  `idclev20` etc. now work.
- **Zone usage logging** (`p_setup.c`): `ZONE after level load: free=N KB` logged
  after each level load. Measured: ~1 MB used out of 32 MB zone across all tested maps.

### Plutonia MAP20 Performance (monster-heavy worst case)

| Metric | Before (no sight cache) | After (sight cache) |
|--------|------------------------|---------------------|
| Mean FPS | ~3.0 | ~3.2 |
| Logic % of frame | **50–69%** | **43–61%** |
| Logic ticks (typical) | 80–130 | 50–100 |
| Render % of frame | ~25% | ~28–33% |

Logic still dominates on MAP20 due to monster density. The remaining logic cost is
`P_Move`/`P_TryMove` (collision detection) and `P_MobjThinker` (state machine, gravity)
which run every tic regardless of sight caching.

### Key Finding: Logic vs Render Bottleneck
On monster-dense maps (Plutonia MAP20), game logic (`P_Ticker`) consumes 50–69% of
frame time. Even with instant rendering, FPS would cap around 4–5 on these maps.
On lighter maps (TNT MAP01), logic is only 14–20% of frame time. The bottleneck
shifts from render to logic as monster count increases.

### Tried and Reverted (this session)
- **Iterative BSP traversal** (`r_bsp.c`): replaced recursive `R_RenderBSPNode` with
  explicit stack. Result: **10% regression** (mean 5.91 vs 6.6 baseline). Likely cause:
  larger code footprint thrashing 68030's 256-byte instruction cache. Reverted.
- **R_PointToDist elimination** (`r_segs.c`): replaced `SlopeDiv`+`FixedDiv` with
  direct dot-product formula (2× FixedMul). Bundled with BSP change; reverted together.

---

## 2026-03-14 — 2× Pixel-Scale Mode vs Non-2× (Basilisk II)
**Emulator: Basilisk II (debug build)**
**Config: detailLevel=2 halfline=1 affinetex=1 solidfloor=1 solidfloor_gray=4**

Head-to-head comparison. Both modes produce `scvw=256 vh=128` (RESV-confirmed) —
render pipeline is **identical**. Performance gap is entirely in the blit/expand path.

| Metric | Non-2x | 2x | Delta |
|--------|--------|----|-------|
| Mean FPS | **6.6** | **5.0** | −1.6 (−24%) |
| Min FPS | 3.8 | 3.5 | −0.3 |
| Max FPS | 10.3 | 7.2 | −3.1 |
| Sample frames | 57 | 66 | — |
| Blit units (typical) | 11–15 | 18–22 | **+7–9** |
| Blit units (palette change) | 16–27 | 29–37 | +10–12 |
| Render units | 24–62 | 23–60 | ≈0 |

**Root cause:** 2× expand+flip pipeline costs ~18ms/frame:
- `expand2x_blit`: ~16KB written (128 src rows × 2 dest rows × 64 bytes)
- 2× flip: 64 bytes × 288 rows = **18,432 bytes** vs non-2× 40 × 200 = **8,000 bytes** (2.3×)

The gap is proportionally worse in fast/open scenes (blit is larger fraction of frame time)
and proportionally smaller in heavy/complex scenes where render dominates.
Max FPS gap (10.3 vs 7.2) is where blit overhead dominates over an already-fast render.

Note: These are Basilisk II numbers. Snow/real SE/30 will scale proportionally lower for both
modes but the relative delta should remain ~1.5 FPS.

---

## 2026-03-13 — Release Build + Blit Narrowing + Polish Pass
**Emulator: Snow (release build, DOOM_RELEASE_BUILD=1)**
**Commit: (this session)**

Baseline performance snapshot taken from Snow emulator running the release build (no
Retro68 console window) after all polish work this session. Profiling run: ~90 frames.

| Metric | Value |
|--------|-------|
| FPS median | **6.4** |
| FPS mean | 6.6 |
| FPS peak | **10.0** |
| FPS floor | 3.8 |
| Top cost: segloop | ~38.5 ms/frame |
| Top cost: blit | ~38.5 ms/frame |
| Scale skip rate | 22.8% of segs |
| P_Ticker share of logic | ~82% |
| Fog effective cull rate | ~5% |

Blit and segloop now roughly tied as the frame's dominant costs. The blit narrowing
landed this session (40 bytes/row instead of 64) — expected ~+0.8 FPS gain not yet
captured in this log. Fog culling at ~5% reflects conservative fog_scale setting;
heavier fog significantly reduces segloop cost.

Key changes this session:
- Release build (no console): removes Retro68 console window overhead and code size
- Double-buffer flip narrowed to 40 bytes/row (game area only, was 64 bytes/row)
- Non-gameplay blit path: uniform `blit8_sbar_thresh` across full screen
- Splash screen with animated PICT cycle added (`i_main_mac.c`)
- Menu bar hide/restore around gameplay; black fullscreen background window
- Clean exit via longjmp/ExitToShell architecture (no artifacts on desktop)
- Sprite fog distance fixed for QUAD mode; explosion sprites always visible at enemy range
- idclev cheat fixed for doom2.wad

---

## 2026-03-10 — ST_Drawer Dirty-Skip (st_stuff.c)
**Emulator: Snow**
**Commit: (uncommitted)**

Added high-level snapshot in `ST_Drawer`: snapshots health, armorpoints, ammo[4],
maxammo[4], readyweapon, st_faceindex, cards bitmask, weaponowned bitmask. When all
match last-drawn values, skip `ST_drawWidgets` entirely. Core insight: `STlib_drawNum`
has no dirty tracking — all number widgets (health, armor, ammo ×8) were unconditionally
redrawn every frame even without changes. Snapshots reset in `ST_initData()` and updated
after `ST_doRefresh()`.

| Metric | Previous (scale inline) | This build |
|--------|-------------------------|------------|
| FPS peak | 8.4 | **9.3** (new all-time Snow record) |
| FPS typical | 4.5–6.5 | 4.5–7.5 (more 7+ frames) |
| st ticks/window typical | 3–6 | **0–2** |
| st=0 frames | ~0% | ~11% |
| st=3-6 frames | ~100% | ~28% (damage/combat only) |

Dirty skip fires on quiet/exploration frames. Still fires on combat frames due to face
index changing every ~17 tics + health/ammo changes during combat. Correct behavior.
**Verdict: keep.**

---

## 2026-03-10 — R_ScaleFromGlobalAngle Inline + Short-Seg Skip (r_segs.c, r_main.c)
**Emulator: Snow**
**Commit: (uncommitted)**

Two changes combined:
1. **Inlined `R_ScaleFromGlobalAngle`** into `R_StoreWallRange` via `SCALE_FROM_ANGLE` macro —
   eliminates two function-call overheads per seg (prologue/epilogue/branch on 68030).
2. **Short-seg fast path**: when `stop - start <= 2`, skip `scale2` computation entirely —
   saves 3 divisions (`FixedDiv(scale2)`, `0xffffffff/scale2`, `scalestep` integer div).
   `prof_r_scale_skip` counter added; 20–41% of segs/frame hit this path.

| Metric | Before (colormap skip build) | This build |
|--------|------------------------------|------------|
| FPS peak | 7.6 | **8.4** (new all-time Snow record) |
| FPS typical | 4.5–6.2 | 4.5–6.5 (more 7+ frames) |
| scale ticks/window | 2–13 | 1–8 |
| scale % of trav | 25–50% | 13–38% |
| skip rate (segs) | — | 20–41%/frame |
| cy/px typical | 48–85 | ~45–80 |

~40% reduction in scale cost on high-seg frames. FPS peak 8.4 = new Snow high.
**Verdict: keep.**

---

## 2026-03-09 — Colormap/Lighting Index Cache (r_segs.c)
**Emulator: Snow**
**Commit: (uncommitted)**

Added `last_light_index` cache in `R_RenderSegLoop`: skip `dc_colormap = walllights[index]`
reassignment whewn the lighting index hasn't changed from the previous column. `rw_scale` steps
slowly across a seg, so adjacent columns often share the same `walllights[index]`. ~14–20 cycles
saved per skipped column.

| Metric | Previous (nibble table, Snow) | This build (Snow, extended run) |
|--------|-------------------------------|----------------------------------|
| FPS min | 4.1 | 3.6 |
| FPS typical | 5.5–6.5 | 4.5–6.2 |
| FPS peak | 7.1 | **7.6** (new high) |
| cy/px range | 32–171 | **39–123** |
| cy/px typical | 40–100 | 48–85 |
| blit ticks | 7–22 | 7–28 |

Extended 80+ frame Snow run confirms improvement. cy/px top end dropped 28% (171→123).
Peak FPS 7.6 (new Snow high). Lower FPS floor (3.6) on heavy scenes with many sprites/segs.
MOVEM.L blit tested and rejected same session (Basilisk II dirty-page issue, marginal savings).
**Verdict: keep colormap skip.**

---

## 2026-03-08 — QUAD Nibble Precomputed Table (r_draw.c)
**Emulator: Snow**
**Commit: (uncommitted, deployed build)**

Replaced `QUAD_NIBBLE` macro (4× CMP + shift + OR ≈ 60 cycles/row) in
`R_DrawColumnQuadLow_Mono` with a precomputed 2KB lookup table
(`quad_nibble_hi[4][256]` / `quad_nibble_lo[4][256]`). One `MOVE.B` table
lookup ≈ 8 cycles replaces the four comparisons.

| Metric | Before (Basilisk II) | After (Snow) |
|--------|----------------------|--------------|
| FPS — light/open scenes | 5.6–6.8 | 6.8–7.1 |
| FPS — typical corridors | 4.5–6.1 | 5.5–6.5 |
| FPS — heavy/complex | 3.4–4.5 | 4.1–5.6 |
| cy/px range | 46–238 | 32–171 |
| cy/px typical | 80–200 | 40–100 |
| render ticks (typical) | 33–70 | 24–50 |

Note: before/after are different emulators (Basilisk II vs Snow), so the delta
understates the gain — Snow runs slower than Basilisk II, yet FPS is higher.

---

## 2026-03-08 — BSP Fog Culling + Seg Fast Path (r_bsp.c, r_segs.c)
**Emulator: Basilisk II**
**Commit: 31bfaa8**

Two fog-distance culling optimisations:
1. **BSP bbox fog cull** (`R_CheckBBox`): prune entire BSP subtrees whose
   nearest point exceeds the fog distance — skips R_StoreWallRange entirely.
   Logged as `bbox=N` culls/frame (0–56 observed).
2. **Seg-level fog fast path** (`R_RenderSegLoop`): when both scale endpoints
   of a seg are below `fog_scale`, skip all texture/colfunc work — update BSP
   clip arrays only. Logged as `fog_seg=N` (0–62 observed).

New profiling counters added: `fog_seg`, `bbox` in FPS log line.

| Metric | Before | After |
|--------|--------|-------|
| FPS — light scenes | 5.6–6.1 | 6.0–6.8 |
| FPS — heavy scenes | 3.4–4.1 | 3.8–4.5 |
| bbox culls/frame | — | 0–56 |
| fog_seg culls/frame | — | 0–62 |

Gain varies heavily with fog_scale setting and scene; open outdoor scenes
with moderate fog show the most improvement.

---

## 2026-03-07 — QUAD Low-Detail Mode (detailLevel=2)
**Emulator: Basilisk II + Real SE/30**
**Commit: 78acf02**

Introduced `detailLevel=2` (QUAD mode): each logical column is 4 screen pixels
wide, each row 2 screen pixels tall (with halfline). Added:
- `R_DrawColumnQuadLow_Mono` — nibble-write column renderer (4px wide)
- `R_DrawSpanQuadLow_Mono` — nibble-write span renderer
- Gamma curve + palette tuning for low-detail mode
- `fog_scale` runtime key adjustment (`` ` `` / `\`)
- Distance fog sprite culling in `r_things.c`

| Metric | Previous best (detailLevel=1) | QUAD mode (detailLevel=2) |
|--------|-------------------------------|---------------------------|
| FPS — Basilisk II | 2.2–3.5 typical | 5–7 typical |
| FPS — Real SE/30 | ~1–2 (borderline) | **5–7 FPS on E1M1** |
| Render pixels/frame | ~16K (half-low) | ~4K (quad-low) |
| Status | Unplayable on HW | **Playable on real SE/30** |

First build declared **playable on real hardware**.

---

## 2026-03-02 — Double-Buffering + Fog + Phase 5 Opts
**Emulator: Basilisk II**
**Commit: 8357b33**

Multiple improvements bundled:
- **Double-buffering** (`i_video_mac.c`): 22KB off-screen buffer, `memcpy`
  flip at end of `I_FinishUpdate` — eliminated mid-frame flicker on menus
  and background
- **halfline default ON** (`m_misc.c`): renders even rows only, copies to odd;
  no white flash with double-buffer
- **Distance fog system**: `fog_scale` global, config/runtime tunable, wall
  and sprite culling
- **Visplane mark skip** (`r_segs.c`): suppress floor/ceiling visplane writes
  when `opt_solidfloor=1` (saves array writes per column)
- **LOD skip**: skip 1-pixel-high column draws in halfline mode
- **`-funroll-loops`** restricted to `r_draw.c`, `r_segs.c`, `r_bsp.c` only
  (global flag bloated the 256-byte I-cache)

| Metric | Before | After |
|--------|--------|-------|
| FPS — typical gameplay | ~1–2 | 2.2–3.5 |
| FPS — door/wall-facing | ~2–4 | 5.0–6.5 |
| Menu/bg flicker | Yes | Fixed |
| Fog system | None | Functional |

---

## 2026-03-01 (evening) — ASM Pixel Macros + Flat Floor/Ceiling
**Emulator: Snow**
**Commit: 3ababb6**

- **68030 inline ASM pixel macros** (`COLMONO_GRAY`, `COLMONO_BIT` in
  `r_draw.c`): SWAP for `frac>>16` avoids multi-cycle ASR chain; BSET/BCLR
  with bit number instead of bitmask frees a register
- **Flat floor/ceiling** (`opt_solidfloor`): fill floors/ceilings with solid
  dither pattern instead of textured spans — eliminates span drawing entirely
- **Solid floor gray level** (`solidfloor_gray`): tunable fill darkness

| Metric | Before | After |
|--------|--------|-------|
| FPS — Snow emulator | <1–2 | **2–4 FPS** |
| Floor/ceiling cost | High (span drawing) | Near zero (fill) |
| Pixel inner loop | C fallback | 68030 ASM macros |

First build that ran at recognizable frame rates on Snow (close to real HW).

---

## 2026-03-01 (afternoon) — Direct 1-bit Renderers + Phase 3/4
**Emulator: Basilisk II**
**Commit: 8076b8d**

- **Phase 3 — `I_FinishUpdate` 8-pixel blit** (`i_video_mac.c`): pack 8
  grayscale pixels into 1 byte via Bayer dithering before writing to the
  1-bit framebuffer — 8× fewer framebuffer writes vs byte-per-pixel
- **Phase 4 — Direct 1-bit renderers** (`r_draw.c`): `R_DrawColumn_Mono`,
  `R_DrawColumnLow_Mono`, `R_DrawSpan_Mono` write directly to the 1-bit
  framebuffer, skipping the intermediate `screens[0]` buffer entirely.
  Variables `fb_mono_base`, `fb_mono_rowbytes`, `fb_mono_xoff`, `fb_mono_yoff`
  exposed for use in draw routines
- **`transcolfunc`** added for translated column draws (`r_things.c`)
- **`r_main.c`** clears `screens[0]` view area before render for HUD overlay

| Metric | Before | After |
|--------|--------|-------|
| FPS — Basilisk II | ~1–3 | Still borderline but improved |
| Framebuffer writes | 1 per pixel (8-bit) | 1 per 8 pixels (1-bit packed) |
| Rendering path | Buffer → convert → blit | Direct to 1-bit framebuffer |

Described in commit as "still borderline unplayable" but major structural
improvement enabling all subsequent optimisations.

---

## 2026-02-26 — Phase 1 & 2: Compiler + Fixed-Point Opts
**Emulator: Basilisk II**
**Commit: 9b5279c**

- **Phase 1**: `detailLevel=1`, `screenblocks=7` (reduced viewport),
  `-O3 -fomit-frame-pointer` build flags, frame timing added to `D_DoomLoop`
- **Phase 2**: `FixedMul` as inline macro (`m_fixed.h`) — avoids function call
  overhead; `FixedDiv2` uses `long long` arithmetic to remove FPU dependency
- **FPS profiling** added: per-frame breakdown of logic/render/blit/hud/sound

| Metric | Before | After |
|--------|--------|-------|
| FPS — Basilisk II | <1 (slideshow) | ~7 FPS (Basilisk II, high detail) |
| `FixedMul` | Function call | Inline macro |
| FPU usage | Yes (FixedDiv) | Eliminated |

Note: "7fps" quoted in commit message is Basilisk II at detailLevel=1 — faster
than real HW. Snow/real HW performance was significantly lower at this stage.

---

## 2026-02-25 — v0.1: First Boot on Real Hardware
**Emulator: Basilisk II + Real SE/30**
**Commit: e53da1a**

Game boots and runs. Critical bugs fixed to get here:
- **Endianness**: `-D__BIG_ENDIAN__` added to CMakeLists; `m_swap.c` guard
  fixed; WAD byte-swapping now correct
- **HELP2 lump**: fallback to `HELP` for shareware WAD
- **`alloca()` stack overflow** (`w_wad.c`): 20KB+ stack alloc for WAD lump
  table caused silent corruption on real SE/30 (8–24 KB default stack) →
  replaced with `malloc()`/`free()`
- **Log file**: `doom_log()` added, writes to shared folder for debugging

| Metric | State |
|--------|-------|
| Boots in Basilisk II | ✓ |
| Boots on real SE/30 | ✓ (after alloca fix) |
| FPS | Slideshow (<1 FPS) |
| Playable | No |
