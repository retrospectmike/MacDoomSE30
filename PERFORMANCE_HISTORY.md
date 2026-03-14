# Doom SE/30 — Performance History

Chronological record of FPS and profiling data across optimization milestones.
Newest entries at top. Add new entries here after each significant change.

**Emulator key:**
- **Basilisk II** — Mac IIci emulation, faster than real HW, useful for iteration
- **Snow** — Closer to real SE/30 timing, more representative
- **Real SE/30** — 68030 @ 16 MHz, the actual target

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
