# Doom SE/30 — Performance History

Chronological record of FPS and profiling data across optimization milestones.
Newest entries at top. Add new entries here after each significant change.

**Emulator key:**
- **Basilisk II** — Mac IIci emulation, faster than real HW, useful for iteration
- **Snow** — Closer to real SE/30 timing, more representative
- **Real SE/30** — 68030 @ 16 MHz, the actual target

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
