# Doom for Macintosh SE/30 — Master Project Plan

## Table of Contents
1. [Project Overview](#1-project-overview)
2. [Hardware & Software Target](#2-hardware--software-target)
3. [Starting Point: Fresh Port vs Mac Doom Port](#3-starting-point-fresh-port-vs-mac-doom-port)
4. [Development Toolchain](#4-development-toolchain)
5. [Emulator Strategy](#5-emulator-strategy)
6. [Build-Test-Iterate Workflow](#6-build-test-iterate-workflow)
7. [Resource Fork & File Management](#7-resource-fork--file-management)
8. [WAD Asset Pipeline (Pre-Dithered 1-Bit)](#8-wad-asset-pipeline-pre-dithered-1-bit)
9. [Rendering Strategy](#9-rendering-strategy)
10. [Optimization Plan](#10-optimization-plan)
11. [Memory Budget (64 MB Baseline)](#11-memory-budget-64-mb-baseline)
12. [Sound Strategy](#12-sound-strategy)
13. [Input Handling](#13-input-handling)
14. [Phased Implementation Plan](#14-phased-implementation-plan)
15. [Risk Assessment & Mitigations](#15-risk-assessment--mitigations)
16. [Future: Mac Plus / 68000 Port Considerations](#16-future-mac-plus--68000-port-considerations)
17. [Open Questions](#17-open-questions)

---

## 1. Project Overview

**Goal**: A playable port of Doom on a Macintosh SE/30 at 15-30 FPS.

**The core challenge**: The SE/30's 68030 at 16 MHz is roughly **4-7x slower** than the 486DX2-66 that Doom was designed for, and roughly **half** the speed of even the 386DX-33 bare minimum. The closest reference point is the **Atari Falcon** (same 68030 @ 16 MHz), where existing Doom ports achieved only 5-15 FPS at heavily reduced resolutions. We need to exceed that with smart optimization.

**The SE/30's advantages over the Falcon**:
- **64 MB RAM** — massive room for pre-computed tables, full WAD in memory, pre-processed assets
- 1-bit display = only ~22 KB framebuffer (vs 320x200x8 = 64 KB on Falcon/DOS)
- SD-to-SCSI adapter = fast disk I/O for WAD loading
- Built-in 68882 FPU (though we likely won't use it for rendering)
- We can pre-process WAD assets to 1-bit, eliminating real-time dithering cost

**Design philosophy**: Keep it **Doom**, not "Doom-esque." Same WAD format (modified), same levels, same gameplay. Visual fidelity is adapted (pre-dithered 1-bit monochrome, potentially lower-resolution textures), but the game logic, level design, and feel remain authentic. Players should recognize every level.

---

## 2. Hardware & Software Target

### Primary Target Hardware
| Component | Spec |
|-----------|------|
| CPU | Motorola 68030 @ 16 MHz |
| FPU | Motorola 68882 @ 16 MHz (built-in) |
| RAM | **64 MB** (fully populated SE/30) |
| Display | 512 x 342, 1-bit monochrome (built-in CRT) |
| Framebuffer | ~22 KB, memory-mapped, 1 bit/pixel (1=black, 0=white) |
| Storage | SCSI via SD card adapter (fast reads) |
| Sound | Apple Sound Chip (ASC), 8-bit mono |
| I-cache | 256 bytes |
| D-cache | 256 bytes |

### Why 64 MB Instead of 8 MB

The 8 MB configuration is very tight once System 7 overhead is subtracted. With 64 MB we can:
- Load the **entire DOOM.WAD** (shareware ~4 MB, full ~11 MB, Doom II ~14 MB) into RAM
- Store **pre-dithered 1-bit textures** alongside originals, avoiding any runtime conversion
- Use **massive lookup tables** to trade memory for CPU cycles
- Keep **all level data, sounds, and music** resident simultaneously
- Have headroom for generous **texture caches** and **pre-computed columns**
- Maintain an 8 MB "lean mode" as a stretch goal if desired

### System Software: **System 7.1** with MODE32

**Why System 7.1:**

| Factor | System 6 | System 7.1 | Winner |
|--------|----------|------------|--------|
| RAM overhead | ~300 KB | ~1.5 MB | System 6 |
| 32-bit addressing | No (24-bit, max 8 MB) | Yes (with MODE32) | **System 7** |
| Max addressable RAM | 8 MB | 128 MB | **System 7** |
| Needed for 64 MB | No | **Yes** | **System 7** |
| WaitNextEvent | Optional (MultiFinder) | Always available | System 7 |
| Retro68 compatibility | Supported | Supported | Tie |
| AI agent familiarity | Poor | Better documented | System 7 |
| Basilisk II support | Good | Best | System 7 |

**System 7.1 + MODE32 is required** to address the full 64 MB of RAM. The ~1.5 MB system overhead is negligible with 64 MB total. Code will be written 32-bit clean throughout.

---

## 3. Starting Point: Fresh Port vs Mac Doom Port

### Option A: Start from linuxdoom-1.10 (id Software GPL release)
**Source**: `https://github.com/id-Software/DOOM`

**Pros:**
- Clean, well-understood codebase (~30K lines of C)
- Clear platform abstraction layer (`i_video.c`, `i_sound.c`, `i_system.c`, `i_net.c`)
- Extensively documented by the community (Doom Wiki, "Doom Black Book", etc.)
- No Mac legacy cruft — we write only the platform layer we need
- All rendering code is portable C with fixed-point math
- Many source ports exist as references (Chocolate Doom, etc.)

**Cons:**
- Must write the entire Mac platform layer from scratch
- Must handle WAD loading, screen blitting, input, and sound for classic Mac

### Option B: Start from the Mac Doom port (Lion Entertainment, ~1995)

**Pros:**
- Already has Mac Toolbox integration
- Proven to work on Mac hardware

**Cons:**
- Targeted **PowerPC** primarily — may have PPC-specific assumptions
- Used **8-bit color** — fundamentally different from our 1-bit target
- May use Color QuickDraw APIs not available on SE/30's monochrome display
- Code quality unknown — Lion Entertainment ports had mixed reputations
- Less community documentation than the id release

### **Recommendation: Option A — Start from linuxdoom-1.10**

The linuxdoom source is the better starting point because:
1. The platform abstraction layer (`i_*.c`) is exactly where we need to do our work
2. The rendering core is clean, portable C that we can optimize incrementally
3. We need to heavily modify the rendering pipeline anyway (pre-dithered 1-bit)
4. Starting clean avoids inheriting PPC assumptions or Color QuickDraw dependencies
5. Far better community documentation and reference implementations

We should still **study the Mac Doom port** for reference on Mac-specific issues but not use it as our code base.

---

## 4. Development Toolchain

### Cross-Compiler: **Retro68**
- GCC-based cross-compiler targeting m68k-apple-macos
- Runs on modern macOS, produces classic Mac executables with proper resource forks
- Uses CMake build system
- Includes Rez (resource compiler) and Multiversal Interfaces (Mac Toolbox headers)
- Source: `https://github.com/autc04/Retro68`

### Project Structure
```
doom-se30/
├── CMakeLists.txt              # Retro68 CMake project
├── src/
│   ├── d_main.c                # Doom main (from linuxdoom)
│   ├── r_*.c                   # Renderer (modified for 1-bit)
│   ├── p_*.c                   # Game logic (mostly unmodified)
│   ├── w_wad.c                 # WAD loading (modified for Mac FS)
│   ├── i_video_mac.c           # NEW: Mac video (1-bit framebuffer)
│   ├── i_sound_mac.c           # NEW: Mac sound (ASC)
│   ├── i_system_mac.c          # NEW: Mac system (memory, timing)
│   ├── i_input_mac.c           # NEW: Mac input (keyboard/mouse)
│   └── r_draw_mono.c           # NEW: 1-bit optimized drawers
├── src/asm/
│   ├── r_draw_68k.s            # 68030 asm inner loops
│   └── blit_68k.s              # 68030 asm framebuffer blit
├── tools/
│   ├── wad_convert.py          # WAD pre-processing tool (runs on modern Mac)
│   └── dither_textures.py      # Texture dithering + downscaling tool
├── rsrc/
│   └── doom.r                  # Mac resources (menus, SIZE, BNDL, etc.)
├── wads/
│   └── (original + converted WADs)
├── scripts/
│   ├── build.sh                # Build + deploy to emulator
│   └── convert_wad.sh          # Run WAD conversion pipeline
└── docs/
    └── PLAN.md                 # This file
```

### Debugging Tools
- **MacsBug**: Low-level debugger inside the emulated/real Mac
- **printf-to-serial**: Route debug output through serial port (Basilisk II maps to host file/terminal)
- **Log files**: Write debug logs to shared folder for analysis on host
- **Compiler warnings**: GCC `-Wall -Wextra`

---

## 5. Emulator Strategy

### Primary Development Emulator: **Basilisk II**

**Why Basilisk II:**
- Emulates Mac II-class hardware (68030)
- **Shared folder support (ExtFS)** — critical for rapid iteration
- **Serial port emulation** — captures debug output to host
- System 6 through Mac OS 8.1 support
- Command-line configurable

**Configuration for SE/30-like emulation:**
```
rom /path/to/mac_iici.rom
disk /path/to/system7.img
extfs /path/to/shared/
ramsize 67108864               # 64 MB RAM
cpu 3                          # 68030
fpu true                       # 68882
screen mono/512/342            # SE/30 display: 512x342 mono
```

**Key limitation**: Basilisk II emulates a Mac IIci, not SE/30 exactly. Timing won't be cycle-accurate. Fine for development; real hardware testing catches timing issues.

### Secondary Emulator: **Mini vMac (SE/30 variation)**

**Why also Mini vMac:**
- Can be built specifically as an **SE/30 emulator** (requires SE/30 ROM)
- More accurate emulation of compact Mac hardware
- Good for final validation before real hardware

**Limitation**: No shared folder — file transfer via drag-and-drop or disk images. Less convenient for rapid iteration, but more accurate.

### Workflow
1. **Daily development**: Basilisk II (shared folder = instant deployment)
2. **Periodic validation**: Mini vMac SE/30 variant (more accurate)
3. **Final testing**: Real SE/30 hardware

---

## 6. Build-Test-Iterate Workflow

### The Development Loop

```
┌──────────────────────────────────────────────────────┐
│  Modern Mac (macOS Tahoe)                            │
│                                                      │
│  1. Claude Code edits source in doom-se30/src/       │
│  2. Run: ./scripts/build.sh                          │
│     → Retro68 cross-compiles to Mac binary           │
│     → Output: DoomSE30.bin (MacBinary format)        │
│  3. Auto-copies to Basilisk II shared folder         │
│                                                      │
│  ┌────────────────────────────────────────────────┐  │
│  │  Basilisk II (emulated Mac)                    │  │
│  │                                                │  │
│  │  4. Shared folder volume auto-updates          │  │
│  │  5. Double-click DoomSE30 to run               │  │
│  │  6. Debug output → serial → host file          │  │
│  └────────────────────────────────────────────────┘  │
│                                                      │
│  7. Read debug output, iterate                       │
└──────────────────────────────────────────────────────┘
```

### Real Hardware Testing
- **SD card sneakernet**: Copy built binary to SD card → SCSI-SD adapter
- **Serial debugging**: SE/30 serial → USB-serial adapter → modern Mac, capture with `screen`/`minicom`
- **Log file exchange**: Write perf logs to disk on SE/30, read back via SD card

---

## 7. Resource Fork & File Management

### The Solution: Separation of Concerns

**On the modern Mac (Claude Code's environment):**
- All **source code** (.c, .h, .s) — data-fork-only, no issues
- **Resource definitions** as `.r` files (Rez source) — plain text, git-friendly
- **Retro68 build system** compiles `.r` into proper resource forks in output binary
- **WAD conversion tools** run here (Python scripts on modern Mac)
- We **never** handle raw resource forks on the modern side

**On the emulated/real Mac:**
- Built binary (with both forks) lives here
- Converted WAD files (data-fork-only) live here
- Game saves and config files live here

**File transfer:**
- **MacBinary (.bin)**: Retro68 output format, preserves both forks
- **HFS disk images**: Created with `hfsutils` for Mini vMac or SD card transfer
- **WAD files**: Pure data-fork, transfer cleanly anywhere

### Key Principle
> All creative work happens on the modern side as plain text. Resource forks are build artifacts, never hand-edited.

---

## 8. WAD Asset Pipeline (Pre-Dithered 1-Bit)

### The Big Optimization: Pre-Process, Don't Real-Time Convert

Instead of converting 8-bit color textures to 1-bit at runtime (which costs precious CPU cycles every frame), we **pre-convert all WAD assets offline** on the modern Mac. This is one of the most impactful optimizations in the entire project.

### What Gets Pre-Processed

| Asset Type | Original Format | Converted Format | Notes |
|-----------|----------------|-----------------|-------|
| Wall textures | 8-bit indexed color, 64-128px wide | **1-bit dithered**, optionally downscaled | Column-major for fast column drawing |
| Flat textures | 8-bit, 64x64 | **1-bit dithered**, 64x64 or 32x32 | Row-major for span drawing |
| Sprites | 8-bit with transparency | **1-bit dithered + 1-bit mask** | Column-major, sparse format |
| Status bar | 8-bit color | **1-bit pre-rendered** | Pre-composed full status bar graphic |
| Fonts/HUD | 8-bit | **1-bit** | Simple threshold, no dithering needed |
| COLORMAP | 256-entry × 34 levels | **Grayscale mapping table** | For dynamic lighting |
| PLAYPAL | 256 RGB entries | **256 grayscale values** | One-time conversion |

### The WAD Conversion Tool (`tools/wad_convert.py`)

Runs on the modern Mac as a pre-build step. Reads the original DOOM.WAD and outputs a modified WAD (e.g., `DOOM_SE30.WAD`) with:

1. **Textures dithered to 1-bit** using high-quality offline dithering:
   - Can use **Atkinson dithering** (the "Mac classic" look) since it's offline — no speed constraint
   - Or **Floyd-Steinberg** for highest quality
   - Or **Bayer ordered** for consistency with any runtime fallback dithering
   - User/developer choice per conversion run — we can experiment with what looks best

2. **Optional texture downscaling**:
   - Half-resolution textures (32px wide walls instead of 64px) for faster rendering
   - Full-resolution textures can also be stored if RAM allows (64 MB = plenty of room)
   - Could store **both** resolutions in the WAD for runtime detail-level switching

3. **Column-major 1-bit storage**:
   - Wall textures stored as columns of bytes, 8 pixels per byte
   - Optimized for Doom's column-based wall renderer
   - Direct blit to framebuffer without per-pixel conversion

4. **Sprite conversion**:
   - Each sprite column becomes two bitstreams: pixel data (1-bit) and transparency mask (1-bit)
   - Compact and fast to render (AND mask, OR pixels)

5. **Pre-computed lighting**:
   - Instead of 34 COLORMAP levels × 256 entries, we generate a smaller set of **grayscale attenuation levels**
   - For 1-bit output with ordered dithering, we might only need 8-16 effective brightness levels
   - Store pre-dithered versions of textures at different light levels (memory-intensive but eliminates runtime lighting math)
   - With 64 MB RAM, storing 8 light levels × all textures is feasible

### Lighting Strategy Options

**Option A: Pre-dithered light levels (maximum speed, maximum RAM)**
- Store each texture at 8 brightness levels, pre-dithered to 1-bit
- At runtime: select the correct pre-lit texture based on distance → direct blit
- Memory cost: 8× texture storage, but with 64 MB RAM this is ~30-40 MB total for all textures — fits!
- CPU cost: essentially zero per-pixel lighting cost
- This is the **luxury option** enabled by 64 MB RAM

**Option B: Runtime brightness modulation (less RAM, more CPU)**
- Store textures as 1-bit at full brightness
- At runtime: apply brightness by selectively "turning off" pixels based on a dither pattern and distance
- More CPU work per pixel but much less RAM

**Option C: Grayscale intermediate with runtime dithering (most flexible)**
- Store textures as 4-bit or 8-bit grayscale (not 1-bit)
- Apply lighting as grayscale multiplication
- Dither to 1-bit as final step (ordered dithering with Bayer LUT)
- Most flexible for quality but costs CPU for the dither pass

**Recommendation**: Start with **Option C** (grayscale + runtime dither) for initial development — it's the most flexible and easiest to debug. Then optimize to **Option A** (pre-dithered light levels) once we need the performance and have profiling data showing the dither pass is a bottleneck. With 64 MB, we can afford Option A.

### WAD Conversion Pipeline
```
Original DOOM.WAD (11 MB, 8-bit color)
        │
        ▼
wad_convert.py (runs on modern Mac)
  ├── Read all textures, flats, sprites
  ├── Convert palette to grayscale
  ├── Apply dithering (Atkinson/Floyd-Steinberg/Bayer — configurable)
  ├── Optionally downscale textures
  ├── Repack in modified WAD format
  ├── Store pre-lit texture variants (if Option A)
  └── Preserve all non-graphic lumps (levels, sounds, music) as-is
        │
        ▼
DOOM_SE30.WAD (~15-40 MB depending on options — fits in 64 MB RAM)
```

### Keeping It "Doom"

The pre-dithered WAD approach preserves Doom's identity because:
- Same level geometry, same enemy placements, same item locations
- Same textures, just rendered in 1-bit — players will recognize every wall
- The dithering style (especially Atkinson) gives it a distinctly "classic Mac" look while remaining clearly Doom
- Game logic is completely unmodified
- We're not simplifying or replacing any content — just converting its visual representation

---

## 9. Rendering Strategy

### The Rendering Pipeline (Modified for SE/30)

```
Standard Doom:    BSP → Walls/Flats/Sprites → 320x200 8-bit buffer → screen

SE/30 Doom:       BSP → Walls/Flats/Sprites → direct 1-bit output to framebuffer
                  (using pre-dithered textures from converted WAD)
```

With pre-dithered textures, we can potentially **skip the intermediate buffer entirely** and draw directly to the 1-bit framebuffer. This eliminates an entire memory copy step.

### Resolution Strategy

**Primary render resolution: 256 x 171** (exactly half of 512x342)
- 43,776 pixels to render (vs 64,000 for original Doom = 68%)
- Each rendered pixel maps to a 2x2 block of screen pixels
- Or: render at 256x171 and upscale 2x (simple byte doubling for 1-bit)

**Detail modes (user-selectable):**
| Mode | Render Resolution | Pixels | Expected FPS | Notes |
|------|------------------|--------|-------------|-------|
| High | 512 x 342 | 175,104 | ~5-8 | Native res, for screenshots |
| Medium | 256 x 171 | 43,776 | ~15-25 | **Primary target** |
| Low | 128 x 86 | 11,008 | ~30+ | Chunky but fast, 4x scale |

### Wall Rendering with Pre-Dithered Textures

With 1-bit pre-dithered textures stored column-major:
- Each texture column is a stream of bytes (8 pixels per byte)
- The column drawer reads bytes from the texture and writes them to the framebuffer
- No color conversion, no dithering, no multiplication per pixel for texture sampling
- Lighting: either select a pre-lit texture variant (Option A) or apply a brightness mask

**Inner loop (conceptual, before assembly optimization):**
```c
// Pre-dithered texture column draw — extremely simple
void R_DrawColumn_1bit(void) {
    byte *dest = framebuffer + (dc_x / 8) + dc_yl * SCREENWIDTH_BYTES;
    byte bitmask = 0x80 >> (dc_x & 7);
    fixed_t frac = dc_texturemid + (dc_yl - centery) * dc_iscale;

    for (int y = dc_yl; y <= dc_yh; y++) {
        // Get 1-bit texel from pre-dithered texture
        int texel_y = (frac >> FRACBITS) & 127;
        int texel_bit = source_column[texel_y / 8] & (0x80 >> (texel_y & 7));

        if (texel_bit)
            *dest |= bitmask;   // set pixel (black)
        else
            *dest &= ~bitmask;  // clear pixel (white)

        dest += SCREENWIDTH_BYTES;
        frac += dc_iscale;
    }
}
```

This is dramatically simpler (and faster) than the original 8-bit column drawer, which had to do texture lookup + COLORMAP lighting lookup + write byte. We're doing texture bit-test + conditional bit-set.

### Floor/Ceiling Rendering

**Option 1: Flat-filled (fastest)**
- Each sector's floor/ceiling is a solid shade: either black, white, or a dither pattern (25%, 50%, 75%)
- Trivially fast — just fill horizontal runs with a constant byte pattern
- Looks acceptable in 1-bit; many 1-bit Mac games used flat fills

**Option 2: Textured with pre-dithered flats (slower but authentic)**
- Pre-dithered flat textures (64x64 or 32x32, 1-bit)
- Perspective-correct span drawing using pre-dithered source
- Significantly more CPU than flat fills

**Recommendation**: Implement both, default to flat-filled, let user enable textured floors if FPS allows.

### Viewport Size

Like original Doom, offer **adjustable viewport**:
- Full: 256x171 render area → 512x342 after 2x scale (no status bar overlap)
- Reduced: 200x140 or smaller → thick border, with status bar below
- Status bar: pre-rendered 1-bit bitmap, drawn at bottom of screen

### Sprite Rendering

Pre-dithered sprites with transparency masks:
- Each sprite column = data bits + mask bits
- Rendering: `screen = (screen AND mask) OR data`
- Two memory reads + two bitwise ops per byte = fast

---

## 10. Optimization Plan

### Priority Order (highest impact first)

#### Tier 1: Architecture-Level (Pre-Build / Design Decisions)
These are "free" performance wins decided at design time.

1. **Pre-dithered WAD assets** — eliminate ALL runtime dithering/color conversion
2. **Render at 256x171** — 68% of original pixel count, clean 2x scaling
3. **1-bit framebuffer** — 8 pixels per byte, 8x fewer memory writes than 8-bit
4. **64 MB RAM** — entire WAD in memory, massive LUTs, pre-lit textures
5. **Flat-filled floors/ceilings** as default (optional textured mode)
6. **Pre-computed lighting** — pre-lit texture variants eliminate per-pixel lighting math

#### Tier 2: Inner Loop Assembly (The Critical Path)

7. **Column drawer in 68030 assembly** — the #1 hotspot (wall rendering)
   - Use `(An)+` post-increment for sequential reads
   - Keep fixed-point accumulator in D-regs, use `SWAP` for integer extraction
   - Bit manipulation for 1-bit framebuffer (BSET/BCLR or shift+mask)
   - Partial loop unrolling (4 pixels/iteration, must fit in 256-byte I-cache)

8. **2x scale blit** in 68030 assembly
   - Copy each rendered byte to two positions (horizontal doubling)
   - Copy each rendered row twice (vertical doubling)
   - Use `MOVEM` for fast multi-register memory copies

9. **Span drawer in 68030 assembly** (if textured floors enabled)
   - Perspective-correct texture mapping with reciprocal LUTs
   - Process 8 pixels at a time → 1 output byte

#### Tier 3: Algorithmic Optimizations

10. **Minimize MULS instructions** — 28-44 cycles each on 68030. Use LUTs, shifts+adds
11. **Reciprocal division tables** — avoid DIVU (38-56 cycles) in inner loops
12. **Tighter BSP traversal** with early-out on full screen coverage
13. **Sprite draw optimization** — batch column draws, minimize mask operations
14. **Visplane reduction** — merge compatible visplanes aggressively

#### Tier 4: Memory-Speed Tradeoffs (Leverage 64 MB)

15. **Pre-lit texture variants** — 8 brightness levels × all textures, select at runtime
16. **Pre-expanded column data** — textures stored in column-draw-ready format
17. **Composite textures pre-built** — multi-patch textures pre-composed in RAM
18. **Full WAD resident in RAM** — zero disk I/O during gameplay
19. **Giant reciprocal/trig LUTs** — trade KB of tables for multiply/divide instructions

#### Tier 5: Quality vs. Performance Knobs

20. **Detail level** (high/medium/low resolution)
21. **Floor rendering** (flat fills vs textured)
22. **Viewport size** adjustment
23. **Max visible sprites** limit
24. **Music on/off** (significant CPU savings — see Sound section)
25. **Frame skipping** (render every other frame)
26. **Adaptive detail** (auto-reduce detail when FPS drops below target)

### The 68882 FPU

**Verdict: Don't use it for rendering.** The 68882's FMUL (~50-70 cycles) is slower than integer MULS (~28-44 cycles). Fixed-point 16.16 is the right approach. This also helps with the future 68000 port (no FPU).

---

## 11. Memory Budget (64 MB Baseline)

```
System 7.1 + MODE32 overhead:    ~2 MB
Application partition:           ~62 MB

Within the application partition:
  Doom executable code:          ~200 KB
  Stack:                         ~64 KB

  WAD data (full DOOM.WAD):      ~11 MB (loaded entirely into RAM)
  Pre-dithered textures:         ~5-8 MB (1-bit versions of all textures)
  Pre-lit texture variants:      ~30-40 MB (8 light levels × all textures)
                                 OR ~5-8 MB (no pre-lit, runtime dither)

  Lookup tables:                 ~1 MB (trig, reciprocals, dither, etc.)
  Composite texture cache:       ~2 MB (pre-built multi-patch textures)
  BSP/gameplay structures:       ~1 MB (nodes, segs, sectors, things, etc.)
  Render buffer (256x171):       ~44 KB (intermediate grayscale, if needed)
  Framebuffer (system):          ~22 KB (memory-mapped)
  Sound buffers:                 ~256 KB
  Music data:                    ~1 MB (if enabled)
  Misc / headroom:               ~2 MB

  TOTAL (with pre-lit textures): ~53-57 MB — FITS in 62 MB ✓
  TOTAL (without pre-lit):       ~23-27 MB — FITS easily ✓
```

With 64 MB, we have **no memory pressure whatsoever**. We can afford every memory-for-speed tradeoff available. This is a huge advantage.

### The 8 MB Stretch Goal

If desired later, an 8 MB mode would require:
- Streaming WAD data from disk (only current level in RAM)
- No pre-lit texture variants (runtime brightness only)
- Smaller lookup tables
- ~5.5 MB application partition after System 7 overhead
- Tight but achievable for the shareware WAD

---

## 12. Sound Strategy

### SE/30 Audio Hardware
- **Apple Sound Chip (ASC)**: 8-bit, mono, 4 oscillators
- Sampled sound output via DMA through Sound Manager

### Two Independent Systems: SFX and Music

**Sound Effects (SFX):**
- Doom's SFX are 8-bit mono PCM — perfect match for the ASC
- Software mixer combines multiple simultaneous sounds
- CPU cost: moderate (mixing N channels of 11 kHz 8-bit audio)
- **Priority: HIGH** — SFX are essential for gameplay feel (door sounds, gunshots, enemy alerts)
- Implement early in Phase 4

**Music:**
- Doom uses MUS format (simplified MIDI) for music
- Requires a **software synthesizer** to convert MIDI notes to PCM audio
- CPU cost: **significant** — real-time MIDI synthesis is expensive
- **Priority: LOW** — music is nice-to-have but not essential for "playable Doom"

### Music Strategy

**Default: Music OFF.** This frees up significant CPU for rendering.

**Optional music modes (user-selectable):**
1. **No music** (default) — maximum FPS
2. **Simple music** — minimal polyphony (2-4 voices), basic waveforms, low quality but low CPU
3. **Full music** — higher polyphony, better synthesis, noticeable FPS impact
4. **Pre-rendered music** — convert MUS tracks to PCM offline, store in WAD as raw audio. Zero runtime synthesis cost, but ~10-30 MB of additional WAD data. With 64 MB RAM, this is feasible!

**Recommendation**: Implement SFX first. Add music later as option 1 (off) and option 4 (pre-rendered PCM from converted WAD). This avoids the entire software synth problem.

### Sound CPU Budget
- Estimated SFX mixing cost: ~5-10% of frame time
- Estimated full MIDI synthesis: ~15-30% of frame time (this is why music-off is default)
- SFX + no music is the sweet spot for playable frame rates

---

## 13. Input Handling

### Keyboard & Mouse
- SE/30 uses ADB keyboard and mouse
- Classic Mac Toolbox Event Manager handles input
- Doom needs: move, strafe, turn, fire, use, weapon select, run, menu

### Implementation
- `WaitNextEvent` for main event loop
- `GetKeys` for direct keyboard state polling (lower latency for gameplay)
- Mac mouse is single-button — fire mapped to mouse button, use/open to keyboard
- Standard arrow keys / WASD for movement
- Straightforward Toolbox code, not a performance concern

---

## 14. Phased Implementation Plan

### Phase 0: Environment Setup (Week 1)
- [ ] Install and build **Retro68** on modern macOS
- [ ] Set up **Basilisk II** with System 7.1, MODE32, 64 MB RAM, shared folder, serial output
- [ ] Configure Basilisk II for 512x342 monochrome display
- [ ] Build and run a "Hello World" Mac app via Retro68 → Basilisk II
- [ ] Set up **build-deploy script** (build → shared folder)
- [ ] Verify direct framebuffer writes (draw test pattern on 512x342 display)
- [ ] Optionally: build Mini vMac SE/30 variant for validation
- [ ] Verify serial debug output works (printf from Mac app → host terminal)

### Phase 1: Doom Compiles and Boots (Weeks 2-3)
- [ ] Import linuxdoom-1.10 source into project
- [ ] Strip Linux-specific code (`i_video.c`, `i_sound.c`, `i_net.c`, `i_system.c`)
- [ ] Write minimal **i_system_mac.c**: memory allocation (NewPtr/64MB), timing (TickCount)
- [ ] Write minimal **i_video_mac.c**: allocate 320x200 grayscale buffer, simple threshold blit to 1-bit screen
- [ ] Write minimal **i_input_mac.c**: keyboard input via GetKeys
- [ ] Stub out **i_sound_mac.c**: no-op (silent)
- [ ] Get Doom to **compile** for 68k with Retro68 — fix all errors
- [ ] Get Doom to **launch and show the title screen**
- [ ] Get Doom to **load a level and render a single frame**

### Phase 2: WAD Pipeline + Visual Quality (Weeks 4-6)
- [ ] Build **wad_convert.py** tool: reads DOOM.WAD, converts textures/sprites/flats to grayscale and 1-bit
- [ ] Implement **Atkinson dithering** in the converter for that classic Mac aesthetic
- [ ] Also implement **Bayer ordered dithering** option in converter
- [ ] Generate first **DOOM_SE30.WAD** with pre-dithered 1-bit assets
- [ ] Modify renderer to use 1-bit textures from converted WAD
- [ ] Implement **proper ordered dithering** for any runtime conversion still needed (lighting)
- [ ] Implement **2x scaling** (256x171 → 512x342)
- [ ] Implement **flat-filled floors/ceilings**
- [ ] Get basic **gameplay working**: walk, shoot, doors, enemies
- [ ] Measure FPS — establish **baseline performance**
- [ ] Profile: identify top 5 hotspots
- [ ] Visual tuning: adjust dithering parameters for best 1-bit appearance

### Phase 3: Performance Optimization (Weeks 7-12)
- [ ] Write **68030 assembly column drawer** (1-bit optimized)
- [ ] Write **68030 assembly 2x scale blit**
- [ ] Implement **pre-lit texture variants** (8 light levels in WAD)
- [ ] Optimize fixed-point math (LUTs for multiply, reciprocal tables for divide)
- [ ] Implement **adjustable viewport size**
- [ ] Add **detail level settings** (high/medium/low)
- [ ] Profile and iterate until **15+ FPS** on medium detail
- [ ] Test on **Mini vMac SE/30 variant**
- [ ] Test on **real SE/30 hardware**
- [ ] Iterate on assembly hot loops based on real hardware profiling

### Phase 4: Sound & Polish (Weeks 13-16)
- [ ] Implement **sound effects** (software mixer → Sound Manager)
- [ ] Add **status bar** rendering (pre-dithered 1-bit bitmap from WAD)
- [ ] Implement **menu system** (Mac-adapted)
- [ ] Add **save/load game** support
- [ ] Add **music option**: pre-rendered PCM tracks in converted WAD (if CPU/RAM allow)
- [ ] Implement **music on/off toggle**
- [ ] Performance tuning with sound enabled
- [ ] Test with **Doom II WAD** (convert and test)

### Phase 5: Refinement (Ongoing)
- [ ] **Adaptive detail**: auto-adjust based on frame rate
- [ ] **Textured floors** option (optional mode, with 68030 asm span drawer)
- [ ] **Multiple WAD support** (Doom, Doom II, custom WADs)
- [ ] **Configuration persistence** (save settings to prefs file)
- [ ] Explore **pre-rendered music** in converted WAD
- [ ] Final push for consistent **20-30 FPS**
- [ ] Stretch: **8 MB lean mode** (WAD streaming, no pre-lit textures)

---

## 15. Risk Assessment & Mitigations

### Risk 1: Performance Target Not Achievable (HIGH RISK)
**Mitigation**: Multiple fallback levels:
- Low detail mode (128x86, 4x scale) should hit 30+ FPS
- Flat floors save ~30-40% render time
- Pre-dithered textures eliminate runtime conversion entirely
- Pre-lit textures eliminate runtime lighting
- 64 MB RAM enables every memory-for-speed tradeoff
- Worst case: still a playable Doom experience with reduced fidelity

### Risk 2: Retro68 Toolchain Issues (MEDIUM RISK)
**Mitigation**:
- Actively maintained, widely used
- Fallback: compile inside emulator with MPW/CodeWarrior
- Critical code is hand-written assembly anyway

### Risk 3: Emulator vs. Real Hardware Differences (MEDIUM RISK)
**Mitigation**:
- Test on real hardware early and often
- Use Mini vMac SE/30 variant as intermediate validation
- Don't rely on emulator timing accuracy

### Risk 4: WAD Conversion Quality (LOW-MEDIUM RISK)
**Concern**: Pre-dithered 1-bit textures may not look good enough.
**Mitigation**:
- Multiple dithering algorithms available (Atkinson, Floyd-Steinberg, Bayer)
- Tunable parameters (contrast, brightness, threshold)
- Can hand-adjust specific problem textures
- Full original WAD data preserved for re-conversion with different settings

### Risk 5: Pre-Lit Textures Blow RAM Budget (LOW RISK)
**Mitigation**: With 64 MB, even 8 light levels × all textures fits (~30-40 MB). If somehow too large, reduce to 4 light levels or use runtime lighting for less-common textures.

---

## 16. Future: Mac Plus / 68000 Port Considerations

**Very low priority**, but architectural decisions that keep the door open:

| Feature | 68000 (Mac Plus, 8 MHz) | 68030 (SE/30, 16 MHz) |
|---------|--------------------------|------------------------|
| MIPS | ~1-2 | ~5-6 |
| Multiply | 70 cycles | 28-44 cycles |
| Shifts | 2 cycles/bit | Barrel shifter (constant) |
| Caches | None | 256B I + 256B D |
| FPU | None | 68882 |
| Max RAM | 4 MB | 128 MB |
| Screen | 512x342 1-bit | Same |

Mac Plus would be ~3-5x slower. If SE/30 achieves 15 FPS → Mac Plus ~3-5 FPS. Would need:
- 128x86 or lower render resolution
- Maximum flat-fill everything
- Possibly Wolfenstein-style raycaster instead of BSP

**Design for portability**: Keep 68030 assembly in separate files, don't use FPU, keep core engine in portable C.

---

## 17. Open Questions

To resolve as we progress:

1. **Retro68 inline assembly**: Does GCC support inline `asm()` for 68k, or separate `.s` files only? (Phase 0)
2. **Direct framebuffer access**: Best method — direct memory writes or QuickDraw `CopyBits`? (Phase 0)
3. **Basilisk II mono mode**: Does it accurately emulate 512x342 monochrome? (Phase 0)
4. **Timing precision**: `TickCount` is 1/60s. Is `Microseconds()` available on System 7.1? (Phase 0)
5. **WAD file I/O**: Mac File Manager APIs vs C `fopen`/`fread` via Retro68 newlib? (Phase 1)
6. **Optimal dithering style**: Atkinson (Mac classic) vs Bayer (consistent) vs custom? Visual testing in Phase 2
7. **Pre-lit texture count**: How many light levels give good results without excessive RAM? (Phase 3)
8. **Sound mixing CPU cost**: How much frame budget does SFX mixing consume? (Phase 4)
9. **Real SE/30 cycle timing**: Does emulator match real hardware performance? (Phase 3)

---

## Summary: The Critical Path

```
Phase 0: Toolchain + emulator working, hello world on screen
    ↓
Phase 1: Doom compiles for 68k Mac, shows a frame
    ↓
Phase 2: WAD converter built, pre-dithered rendering, baseline FPS measured
    ↓
Phase 3: Assembly optimization → 15+ FPS achieved ← THE HARD PART
    ↓
Phase 4: Sound, menus, polish → complete playable game
    ↓
Phase 5: Push for 25-30 FPS, full game support, refinement
```

### Why This Will Work

1. **Pre-dithered 1-bit textures** eliminate the most expensive per-pixel operation (color conversion + dithering)
2. **64 MB RAM** lets us trade memory for speed at every opportunity
3. **256x171 render resolution** reduces pixel count to 68% of original Doom
4. **Flat-filled floors** eliminate the most expensive render subsystem
5. **1-bit framebuffer** is only 22 KB — memory bandwidth is minimal
6. **68030 assembly inner loops** with careful I-cache management
7. **Music-off default** preserves CPU for rendering
8. **Multiple detail levels** ensure playability even if top FPS target is hard to reach

**Let's build this.**
