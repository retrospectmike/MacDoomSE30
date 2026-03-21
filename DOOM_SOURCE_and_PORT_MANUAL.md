# DOOM SE/30 — Technical Notes
## Architecture, Internals, and SE/30 Port Optimizations

*A technical reference for developers working with this codebase.*

---

## Table of Contents

### Part I — DOOM Source Code Architecture
- Chapter 1: Overview & File Map
- Chapter 2: Engine Core
- Chapter 3: The Renderer (Deep Dive)
- Chapter 4: Physics & Game Logic
- Chapter 5: UI & HUD
- Chapter 6: Sound System
- Chapter 7: Platform Layer (Mac-Specific)

### Part II — SE/30 Port Optimizations
- Opt 1: Compiler Flags (`-O3 -fomit-frame-pointer`)
- Opt 2: `-funroll-loops` Selective Application
- Opt 3: `FixedMul` Inlined Macro
- Opt 4: `FixedDiv2` — FPU Removal
- Opt 5: Direct 1-bit Rendering
- Opt 6: `I_FinishUpdate` 8-pixels-per-byte Blit
- Opt 7: Double-Buffering
- Opt 8: Half-Line Rendering (`opt_halfline`)
- Opt 9: Affine Texture Stepping (`opt_affine_texcol`)
- Opt 10: `iscale` Linear Interpolation
- Opt 11: Detail Level System (HIGH / LOW / QUAD)
- Opt 12: `opt_solidfloor` — Skip Flat Texture Rendering
- Opt 13: Distance Fog System (`fog_scale`)
- Opt 14: 2× Pixel Scale Mode (`opt_scale2x`)
- Opt 15 *(failed)*: MUSH Detail Mode
- Opt 16 *(not pursued)*: Pre-Processing WAD on Modern Machine
- Opt 17 *(failed)*: Pre-Dithered Texture Columns (Option D)

### Figures
- Figure 1a: Architectural Layer Stack
- Figure 1b: One-Frame Data Flow
- Figure 2: Startup Sequence
- Figure 3: D_DoomLoop Frame Timeline
- Figure 4: Game State Machine
- Figure 5: Rendering Pipeline
- Figure 6: BSP Traversal
- Figure 7: Wall Rendering Column Math
- Figure 8: I_FinishUpdate Blit State Machine
- Figure 9: Zone Allocator Block Layout
- Figure 10: WAD File Structure
- Figure 11: Optimization Impact Summary
- Figure 12: Direct 1-bit Render Path vs Original
- Figure 13: Bayer Dithering — How It Works
- Figure 14: Half-Line Rendering
- Figure 15: Fog Culling Decision Tree
- Figure 16: Double-Buffer Flip

---

# Part I — DOOM Source Code Architecture

---

## Chapter 1: Overview & File Map

### 1.1 What This Port Is

This codebase is a port of id Software's 1993 DOOM to the Apple Macintosh SE/30, compiled with the Retro68 GCC-based cross-compiler targeting the Motorola 68030 CPU. The game runs under Classic Mac OS 7.x with a 512×342 1-bit (monochrome) display. The port uses no third-party libraries — only the Mac Toolbox (Classic Mac OS system calls) and the standard C library as provided by Retro68.

The primary engineering challenge is not correctness but *performance*: the SE/30 runs at 16 MHz with no hardware sprite acceleration, no GPU, no 2D blitter beyond the CPU itself, and a display that requires all color information to be converted to 1-bit black-and-white. The original DOOM was designed for 386/486 PCs running at 33–66 MHz with fast VGA hardware. Getting it to run at a playable frame rate on the SE/30 required a substantial number of targeted optimizations, described in Part II of this document.

### 1.2 Target Hardware Constraints

| Constraint | Value | Impact |
|---|---|---|
| CPU | Motorola 68030 @ 16 MHz | ~5 MIPS effective throughput |
| FPU | Motorola 68882 (present) | Avoided in hot paths — context switch overhead outweighs benefit |
| Instruction cache | 256 bytes | Aggressive loop unrolling causes cache thrash |
| RAM | 8 MB typical | Zone allocator must work within this; no virtual memory |
| Display | 512×342, 1-bit monochrome | All rendering must convert 8-bit palette → 1-bit Bayer-dithered |
| Display rowbytes | 64 bytes | 512px / 8 bits = 64 bytes per row |
| Game view offset | xoff=96px (12 bytes), yoff=71px | View centered in 512×342 within 320×200 game area |
| Storage | HFS (SCSI or floppy-emulated) | Sequential reads ~1 MB/s; explains 10-15s WAD load time |
| OS | Classic Mac OS 7.x | Cooperative multitasking; no preemption; Toolbox event loop required |
| Compiler | Retro68 GCC (m68k-apple-macos-gcc) | Big-endian; no libc math; limited C99 |

### 1.3 Build System

The build is driven by CMake with the Retro68 toolchain file:

```
cmake -S src/ -B build/src/ -DCMAKE_TOOLCHAIN_FILE=.../retro68.toolchain.cmake
cmake --build build/src/
```

`scripts/build.sh src` handles debug builds; `scripts/build.sh src release` handles release builds (sets `DOOM_RELEASE_BUILD=1`, which disables all `doom_log()` calls and the Retro68 console window).

Key CMakeLists.txt decisions:
- **`-m68030 -O3 -fomit-frame-pointer`** globally (see Opt 1)
- **`-funroll-loops`** only on `r_draw.c`, `r_segs.c`, `r_bsp.c` (see Opt 2)
- **`d_main.c`** compiled at `-O2 -fno-omit-frame-pointer` due to a GCC 68k stack frame bug with `-O3` and large local variable sets (see Opt 1)
- `add_application(DoomSE30 CREATOR "DMSE" ...)` — Retro68 macro that wraps the output binary into a Classic Mac application with resource fork, type `APPL`, creator `DMSE`

### 1.4 Source File Map

All source files live in `src/`. The table below lists every compiled file, its approximate line count, its role, and its primary callers.

#### Engine Core

| File | Lines | Role | Primary caller(s) |
|---|---|---|---|
| `d_main.c` | 1404 | Game startup, main loop, frame timing, profiling | `main()` in i_main_mac.c |
| `g_game.c` | 1689 | Input→ticcmd, game state machine, demo, level transitions | `d_main.c` (D_DoomLoop) |
| `d_net.c` | ~200 | Network stub (no actual networking) | `d_main.c` |
| `doomdef.c` | ~50 | Global definitions | all |
| `doomstat.c` | ~100 | Global game state variables | all |
| `d_items.c` | ~60 | Weapon/ammo item tables | `p_inter.c` |

#### WAD & Memory

| File | Lines | Role | Primary caller(s) |
|---|---|---|---|
| `w_wad.c` | 606 | WAD file parsing, lump directory, lazy lump cache | `d_main.c`, `r_data.c`, all resource users |
| `z_zone.c` | 468 | Custom zone memory allocator, tag-based purging | all (every Z_Malloc call) |

#### Renderer

| File | Lines | Role | Primary caller(s) |
|---|---|---|---|
| `r_main.c` | 1100 | Renderer init, view setup, render pipeline entry | `d_main.c` (D_Display) |
| `r_bsp.c` | ~600 | BSP tree traversal, wall segment clipping | `r_main.c` (R_RenderPlayerView) |
| `r_segs.c` | ~500 | Wall segment rendering, column dispatch | `r_bsp.c` (R_StoreWallRange) |
| `r_draw.c` | ~1200 | Pixel-level column/span drawers (8-bit and 1-bit) | via function pointers (`colfunc`, `spanfunc`) |
| `r_plane.c` | ~450 | Floor/ceiling visplane management and rendering | `r_main.c` (R_DrawPlanes) |
| `r_things.c` | ~600 | Sprite projection, depth sort, masked rendering | `r_main.c` (R_DrawMasked) |
| `r_sky.c` | 63 | Sky texture constants and init | `r_main.c`, `r_segs.c`, `r_plane.c` |
| `r_data.c` | ~800 | Texture/flat/sprite data loading, colormap init | `r_main.c` (R_Init) |

#### Physics & Game Logic

| File | Lines | Role | Primary caller(s) |
|---|---|---|---|
| `p_setup.c` | ~700 | Level lump loading and structure init | `g_game.c` (G_DoLoadLevel) |
| `p_tick.c` | ~100 | Per-tic dispatcher (P_Ticker) | `g_game.c` |
| `p_map.c` | ~900 | Collision, movement, line traversal, shooting | `p_enemy.c`, `p_user.c`, `p_mobj.c` |
| `p_maputl.c` | ~400 | Blockmap iteration, path traversal utilities | `p_map.c`, `p_enemy.c` |
| `p_mobj.c` | ~600 | Game object (mobj) lifecycle, state machine | `p_inter.c`, `p_enemy.c`, `p_setup.c` |
| `p_enemy.c` | ~900 | Monster AI, targeting, action callbacks | `p_tick.c` (via thinker list) |
| `p_inter.c` | ~500 | Pickup, damage, kill logic | `p_map.c`, `p_mobj.c` |
| `p_user.c` | ~300 | Player movement and control | `p_tick.c` |
| `p_pspr.c` | ~400 | Player weapon sprite state machine | `p_tick.c` |
| `p_spec.c` | ~800 | Line/sector special effects (doors, floors, etc.) | `p_map.c`, `p_tick.c` |
| `p_ceilng.c` | ~200 | Ceiling movement | `p_spec.c` |
| `p_doors.c` | ~300 | Door movement | `p_spec.c` |
| `p_floor.c` | ~300 | Floor movement | `p_spec.c` |
| `p_lights.c` | ~200 | Lighting effects | `p_spec.c` |
| `p_plats.c` | ~250 | Platform movement | `p_spec.c` |
| `p_switch.c` | ~200 | Switch activation | `p_spec.c` |
| `p_telept.c` | ~150 | Teleporter logic | `p_spec.c` |
| `p_saveg.c` | ~400 | Save/load game serialization | `g_game.c` |
| `p_sight.c` | ~250 | Line-of-sight check | `p_enemy.c`, `p_map.c` |

#### UI, HUD & Menus

| File | Lines | Role | Primary caller(s) |
|---|---|---|---|
| `m_menu.c` | ~1200 | Menu system (main/options/load/save) | `d_main.c` (M_Responder, M_Drawer) |
| `m_misc.c` | ~400 | Config load/save, file I/O, screenshot | `i_main_mac.c`, `I_Quit` |
| `m_argv.c` | ~80 | Command-line argument parser | `d_main.c` |
| `m_bbox.c` | ~60 | Bounding box utilities | `r_bsp.c`, `p_maputl.c` |
| `m_cheat.c` | ~150 | Cheat code sequence matcher | `st_stuff.c` |
| `m_fixed.c` | ~60 | Fixed-point math (FixedMul/FixedDiv) | everywhere |
| `m_random.c` | ~60 | Deterministic RNG | `p_enemy.c`, `p_inter.c` |
| `m_swap.c` | ~60 | Byte-order conversion (LONG/SHORT macros) | `w_wad.c`, `p_setup.c` |
| `hu_lib.c` | ~400 | HUD widget primitives | `hu_stuff.c` |
| `hu_stuff.c` | ~600 | HUD: messages, level title, chat | `d_main.c` (D_Display) |
| `st_lib.c` | ~300 | Status bar widget primitives | `st_stuff.c` |
| `st_stuff.c` | ~900 | Status bar: health, ammo, face, cheats | `d_main.c` (D_Display) |
| `am_map.c` | ~800 | Automap rendering | `d_main.c` (D_Display) |
| `f_finale.c` | ~300 | End-of-game/episode finale | `g_game.c` |
| `f_wipe.c` | ~200 | Screen wipe transitions | `d_main.c` (D_Display) |
| `wi_stuff.c` | ~700 | Intermission screen | `g_game.c` |
| `v_video.c` | ~300 | Patch drawing into screens[0] | `m_menu.c`, `hu_stuff.c`, `st_stuff.c` |

#### Sound

| File | Lines | Role | Primary caller(s) |
|---|---|---|---|
| `s_sound.c` | ~500 | Sound channel management, attenuation | `d_main.c`, `p_inter.c`, `p_enemy.c` |
| `sounds.c` | ~200 | Sound effect and music tables | `s_sound.c` |
| `i_sound_mac.c` | ~100 | Sound hardware stub (deactivated) | `s_sound.c` |

#### Platform Layer

| File | Lines | Role | Primary caller(s) |
|---|---|---|---|
| `i_main_mac.c` | ~500 | Mac entry point, splash, settings dialog, exit arch | OS (main entry) |
| `i_video_mac.c` | ~1000 | Framebuffer, dithering, blitting, double-buffer | `d_main.c` (I_FinishUpdate) |
| `i_input_mac.c` | ~300 | Keyboard/mouse polling, hotkeys | `i_system_mac.c` (I_StartTic) |
| `i_system_mac.c` | ~400 | Timing, memory, logging, error/quit | `d_main.c`, `w_wad.c`, `z_zone.c` |
| `i_net_mac.c` | ~50 | Network stub | `d_net.c` |

#### Data / Tables

| File | Lines | Role | Primary caller(s) |
|---|---|---|---|
| `info.c` | ~400 | `mobjinfo[]` — all object type definitions | `p_mobj.c` |
| `tables.c` | ~200 | `finesine[]`, `finecosine[]`, `tantoangle[]` lookup tables | `r_main.c`, `r_segs.c` |
| `dstrings.c` | ~100 | String constants (level names, messages) | `hu_stuff.c`, `d_main.c` |

### 1.5 Architecture

#### Figure 1a — Architectural Layer Stack

Each tier depends only on the tier(s) below it. Data and control flow upward; hardware access flows downward.

```
  ┌──────────────────────────────────────────────────────────────────────┐
  │  UI / HUD / Menus / Intermission / Automap / Finale                  │
  │  hu_stuff.c · st_stuff.c · m_menu.c · wi_stuff.c · am_map.c         │
  │  All draw into screens[0] (8-bit); read player state from g_game.c   │
  ├──────────────────────────────────────────────────────────────────────┤
  │  Renderer                    │  Physics & AI                         │
  │  r_main / r_bsp / r_segs     │  p_map / p_enemy / p_inter           │
  │  r_draw / r_plane / r_things │  p_mobj / p_spec / p_sight           │
  │  Reads WAD textures/sprites  │  Reads level geometry from p_setup   │
  │  Writes to fb_mono_base      │  Mutates mobj_t / sector_t state     │
  ├──────────────────────────────────────────────────────────────────────┤
  │  Engine Core                                                          │
  │  d_main.c — orchestrates the frame loop, calls all subsystems        │
  │  g_game.c — game state machine, input→ticcmd, level transitions      │
  │  w_wad.c  — WAD lump directory and lazy cache (feeds renderer/phys)  │
  │  z_zone.c — all heap allocation; every subsystem calls Z_Malloc      │
  ├──────────────────────────────────────────────────────────────────────┤
  │  Platform Abstraction Layer (Mac-specific)                            │
  │  i_main_mac.c  — entry point, splash, settings dialog, exit arch     │
  │  i_video_mac.c — framebuffer, dithering, blit, double-buffer         │
  │  i_input_mac.c — keyboard/mouse polling via GetKeys()                │
  │  i_system_mac.c — timing (TickCount), memory, logging, I_Quit        │
  ├──────────────────────────────────────────────────────────────────────┤
  │  Mac OS 7 Toolbox  │  HFS (SCSI/floppy)  │  68030 hardware           │
  │  QuickDraw / WM    │  WAD file on disk   │  512×342 1-bit framebuf   │
  └──────────────────────────────────────────────────────────────────────┘
```

#### Figure 1b — One-Frame Data Flow

What happens between two consecutive calls to `I_FinishUpdate`. The game runs at 35 Hz logic / ~6-10 Hz render on the SE/30.

```
  KEYBOARD / MOUSE
       │
       │  GetKeys() poll — i_input_mac.c
       ▼
  event_t queue  (d_main.c: D_PostEvent)
       │
       │  G_BuildTiccmd() — g_game.c
       ▼
  ticcmd_t  ────────────────────────────────────────────────────────┐
  (forward/side/angle/buttons)                                      │
       │                                                            │
       │  TryRunTics() → G_Ticker() → P_Ticker()                   │
       ▼                                                            │
  Game world state mutated:                                         │
  · mobj_t positions / health / states  (p_map, p_enemy, p_inter)  │
  · sector_t floor/ceiling heights      (p_spec)                   │
  · player_t health / weapons / powers  (p_user, p_inter)          │
       │                                                            │
       │  D_Display() → R_RenderPlayerView()                        │
       ▼                                                            │
  BSP traversal  (r_bsp.c)                                         │
  · R_RenderBSPNode — walk tree front-to-back                      │
  · R_AddLine / R_StoreWallRange — clip & queue wall segs          │
       │                                                            │
       ├──▶  Wall columns  (r_segs.c → colfunc in r_draw.c)        │
       │     Write directly to fb_mono_base (1-bit, Bayer dither)  │
       │                                                            │
       ├──▶  Floor/ceiling spans  (r_plane.c → spanfunc)           │
       │     Solid fill (opt_solidfloor) or textured spans         │
       │                                                            │
       └──▶  Sprites  (r_things.c → colfunc)                       │
             Depth-sorted, masked column draw                       │
       │                                                            │
       │  HU_Drawer / ST_Drawer — overlay HUD onto screens[0]      │
       ▼                                                            │
  I_FinishUpdate()  (i_video_mac.c)                                │
  · Blit status bar / border from screens[0] → fb_offscreen_buf   │
  · Overlay HUD text onto 1-bit view area                          │
  · memcpy fb_offscreen_buf → real 512×342 framebuffer (flip)  ◀──┘
       │
       ▼
  1-BIT DISPLAY  (512×342 monochrome CRT)
```

---

---

## Chapter 2: Engine Core

The engine core is the spine of the codebase. It owns the frame loop, game state, resource loading, and memory. Everything else — the renderer, physics, UI — is called *from* here. Understanding these four files gives you the mental model to navigate the rest.

---

### 2.1 `d_main.c` — Startup and Frame Loop

`d_main.c` is the first C code that runs after the Mac platform layer hands off control, and it never returns until the game exits. It has two jobs: **initialization** and **the frame loop**.

#### Startup Sequence

**Figure 2** traces the startup call chain from `D_DoomMain` through to the moment the first frame begins.

```
  D_DoomMain()
       │
       ├─ FindResponseFile()         parse @responsefile command-line arg
       │
       ├─ IdentifyVersion()          scan for doom1.wad / doom.wad / doom2.wad
       │   ├─ access(doom2wad)  ──▶  found → gamemode = commercial, D_AddFile()
       │   ├─ access(doomwad)   ──▶  found → gamemode = registered, D_AddFile()
       │   ├─ access(doom1wad)  ──▶  found → gamemode = shareware, D_AddFile()
       │   └─ none found        ──▶  I_NoWadAlert() → I_Quit()   [exits cleanly]
       │
       ├─ M_LoadDefaults()           read doom.cfg → all opt_ flags, fog_scale, etc.
       │
       ├─ Z_Init()                   set up zone memory allocator (i_system → I_ZoneBase)
       ├─ W_InitMultipleFiles()      open WAD, parse lump directory  ← 10-15s here
       ├─ V_Init()                   allocate screens[0..4] buffers
       ├─ R_Init()                   load textures/sprites, build lookup tables
       ├─ P_Init()                   init physics (line specials, switch tables)
       ├─ I_Init()                   init sound (stub on SE/30)
       ├─ S_Init()                   set up sound channels
       ├─ HU_Init()                  load HUD font patches
       ├─ ST_Init()                  load status bar patches
       ├─ AM_Init()                  init automap
       ├─ M_Init()                   init menu system
       │
       └─ D_DoomLoop()               ← never returns
```

`IdentifyVersion` checks WAD files in priority order: doom2f.wad → doom2.wad → plutonia.wad → tnt.wad → doomu.wad → doom.wad → doom1.wad. The first match wins and sets `gamemode` (commercial / retail / registered / shareware). If none match and no PWADs were specified, `wadfiles[0]` remains NULL and `I_NoWadAlert()` displays a message on the background window before exiting cleanly.

#### The Frame Loop — `D_DoomLoop`

`D_DoomLoop` is an infinite loop that never exits. It has three phases per iteration:

```c
void D_DoomLoop(void)
{
    while (1)
    {
        TryRunTics();       // advance game logic (may run 0, 1, or several tics)
        S_UpdateSounds();   // update sound channel positions/volumes
        D_Display();        // render and blit one frame
    }
}
```

**Figure 3** shows the timing relationship between these phases and where the profiling buckets sit.

```
  Wall clock time →

  ├── TryRunTics ──────────┤├── S_UpdateSounds ─┤├─────── D_Display ──────────────────────┤
  │  prof_logic            ││  prof_sound        ││  prof_disp                             │
  │                        ││                    ││                                        │
  │  G_Ticker()            ││  per-channel        ││  ┌─ R_RenderPlayerView ───────────┐   │
  │   P_Ticker()           ││  distance/vol/pan   ││  │  prof_r_bsp   prof_r_segs      │   │
  │    p_map / p_enemy     ││  recalculation      ││  │  prof_r_planes prof_r_masked   │   │
  │    p_mobj / p_spec     ││                     ││  └──────────────────────────────┘   │
  │                        ││                     ││  ┌─ HU/ST/AM Drawers ─────────────┐  │
  │  [may skip if ahead]   ││                     ││  └──────────────────────────────┘   │
  │  [may run 2+ if behind]││                     ││  ┌─ I_FinishUpdate (blit) ────────┐  │
  │                        ││                     ││  │  prof_blit                     │  │
  │                        ││                     ││  └──────────────────────────────┘   │
  └────────────────────────┘└────────────────────┘└────────────────────────────────────┘

  Every 35 tics (~1 second): FPS logged to doom_log
```

`TryRunTics` is DOOM's mechanism for decoupling game logic (35 Hz fixed) from render rate (as fast as possible). It compares `gametic` against the real-time clock and runs zero or more logic tics to catch up. If the machine is fast, it runs exactly one tic per render. On the SE/30, renders often take longer than 1/35s, so `TryRunTics` typically runs zero tics (logic already ahead) — the game runs at the render rate, not at 35 Hz.

#### `D_Display` — Render Routing

`D_Display` routes the render based on `gamestate`:

```c
switch (gamestate)
{
    case GS_LEVEL:
        R_RenderPlayerView();   // BSP → walls → floors → sprites
        HU_Drawer();            // messages, level title
        ST_Drawer();            // status bar
        if (automapactive) AM_Drawer();
        break;
    case GS_INTERMISSION:  WI_Drawer(); break;
    case GS_FINALE:        F_Drawer();  break;
    case GS_DEMOSCREEN:    D_PageDrawer(); break;
}
// wipe handling, menu overlay
if (menuactive) M_Drawer();
I_FinishUpdate();   // blit to physical framebuffer
```

#### Event Queue

Input arrives asynchronously from `i_input_mac.c` via `D_PostEvent(event_t *ev)`, which inserts into a circular ring buffer (`events[MAXEVENTS]`, head/tail indices). Each frame, `D_ProcessEvents` drains the queue and dispatches to `M_Responder` (menus get first crack) then `G_Responder` (game input).

#### Profiling

`d_main.c` maintains a set of `long` accumulators (`prof_logic`, `prof_render`, `prof_blit`, `prof_sound`, etc.) driven by `I_GetMacTick()` (raw `TickCount()`). Every 35 tics it logs a profiling summary to `doom_log.txt`:

```
FPS=7.2  logic=12% render=71% blit=9% sound=4% disp=4%
  r_bsp=28% r_segs=31% r_planes=5% r_masked=7%
```

This per-subsystem breakdown was the primary tool used to identify where optimizations would have the most impact (see Part II).

---

### 2.2 `g_game.c` — Game State Machine

`g_game.c` sits between the frame loop and the simulation. It owns **what game state the player is in** and **how keyboard/mouse input becomes game actions**.

#### Input → Ticcmd

Every logic tic, `G_BuildTiccmd(ticcmd_t *cmd)` converts raw input state into a `ticcmd_t` — a compact, serializable record of what the player is doing this tic:

```c
typedef struct {
    char    forwardmove;   // -127..127: forward/back speed
    char    sidemove;      // -127..127: strafe speed
    short   angleturn;     // delta angle (not absolute)
    short   consistancy;   // for network validation
    byte    chatchar;      // chat key (multiplayer)
    byte    buttons;       // BT_ATTACK | BT_USE | BT_CHANGE | BT_SPECIAL
} ticcmd_t;
```

The ticcmd is the only data that crosses the boundary between input and simulation. In the original DOOM, ticcmds were also the network packet format — each player broadcasts their ticcmd and waits to receive all others before advancing a tic. The SE/30 port has no networking, but the architecture is preserved (and makes demo playback work for free: a demo is just a recorded sequence of ticcmds).

#### Game State Machine

`g_game.c` maintains `gamestate` (what is being shown) and `gameaction` (what should happen next). The action is set by various triggers (menu selection, level completion, death) and executed at the top of `G_Ticker`:

**Figure 4** shows the state and transition graph.

```
                    ┌─────────────────────────────────────┐
                    │         GS_DEMOSCREEN                │
                    │  title / credit / demo playback      │
                    └──────────┬───────────────────────────┘
                               │ ga_newgame / ga_playdemo
                               ▼
  ┌──────────────┐    ga_loadlevel    ┌─────────────────────────────┐
  │ GS_INTERMISSION│ ◀──────────────── │          GS_LEVEL           │
  │  wi_stuff.c  │                    │  gameplay, automap, HUD     │
  └──────┬───────┘                    └──────────────┬──────────────┘
         │ ga_worlddone                              │
         │                           ga_completed ──┘ (map exit)
         ▼                           ga_victory  ──── (E?M8 boss kill)
  ┌──────────────┐
  │  GS_FINALE   │ (end-of-episode text / cast)
  │  f_finale.c  │
  └──────┬───────┘
         │ ga_newgame / back to title
         ▼
                    GS_DEMOSCREEN

  Other transitions (from any state):
    ga_loadgame  ──▶ GS_LEVEL  (load save file)
    ga_savegame  ──▶ (stays in GS_LEVEL, writes file)
    ga_screenshot──▶ (stays in current state, writes PCX)
```

#### Demo System

DOOM's demo system is elegant: `G_RecordDemo` allocates a buffer and sets `demorecording = true`. From that point, every ticcmd built by `G_BuildTiccmd` is also written byte-by-byte to the buffer via `G_WriteDemoTiccmd`. On playback, `G_ReadDemoTiccmd` feeds pre-recorded ticcmds into the simulation instead of reading from hardware — the game runs identically. A demo file is a header (version, skill, episode, map) followed by a stream of ticcmds terminated by `DEMOMARKER (0x80)`.

---

### 2.3 `w_wad.c` — WAD Resource System

WAD ("Where's All the Data?") is DOOM's resource archive format. Every texture, sprite, sound effect, music track, level geometry lump, and palette lives in the WAD file. The WAD system is the foundation everything else is built on — nothing renders or plays without it.

#### WAD File Format

**Figure 10 — WAD File Structure:**

```
  Offset 0                WAD File on Disk
  ┌─────────────────────────────────────────────────────────┐
  │  Header (12 bytes)                                      │
  │  ┌────────────┬──────────────┬────────────────────────┐ │
  │  │ "IWAD"     │ numlumps     │ infotableofs           │ │
  │  │ 4 bytes    │ int32 LE     │ int32 LE (offset→dir)  │ │
  │  └────────────┴──────────────┴────────────────────────┘ │
  ├─────────────────────────────────────────────────────────┤
  │  Lump Data (variable)                                   │
  │  ┌──────────┬────────────┬──────────────────────────┐  │
  │  │ PLAYPAL  │ COLORMAP   │ E1M1 geometry ... doom1   │  │
  │  │ 256 RGB  │ 34 tables  │ VERTEXES LINEDEFS etc.    │  │
  │  └──────────┴────────────┴──────────────────────────┘  │
  ├─────────────────────────────────────────────────────────┤
  │  Lump Directory  (at infotableofs, numlumps × 16 bytes) │
  │  ┌──────────────┬────────────┬──────────────────────┐  │
  │  │ filepos      │ size       │ name[8]               │  │
  │  │ int32 LE     │ int32 LE   │ ASCII, NUL-padded     │  │
  │  └──────────────┴────────────┴──────────────────────┘  │
  │  × numlumps entries  (doom1.wad: 1266 entries = 20 KB) │
  └─────────────────────────────────────────────────────────┘

  In memory after W_AddFile():
  lumpinfo[]  array of lumpinfo_t:  { handle, position, size, name[8] }
  lumpcache[] array of void*:       NULL until first access, then Z_Malloc'd
```

All integer fields in the WAD are **little-endian**. Since the 68030 is big-endian, every integer read from a WAD lump must be byte-swapped. The `LONG(x)` and `SHORT(x)` macros in `m_swap.h` handle this: on big-endian builds they perform the swap; on little-endian (PC) they're no-ops. A critical early bug in this port involved `m_swap.c` having its `#ifdef` backwards — the swap was only applied on little-endian, causing `numlumps` to read as ~4 billion and crashing `realloc`.

#### Lazy Loading — Why It Works

`W_AddFile` reads **only the directory** into `lumpinfo[]`. The actual lump data stays on disk. When anything first requests a lump via `W_CacheLumpNum(lump, tag)`:

```c
void* W_CacheLumpNum(int lump, int tag)
{
    if (!lumpcache[lump])
    {
        // First access: allocate and read from disk
        ptr = Z_Malloc(lumpinfo[lump].size, tag, &lumpcache[lump]);
        W_ReadLump(lump, lumpcache[lump]);
    }
    else
        Z_ChangeTag(lumpcache[lump], tag);   // already cached; update purge priority

    return lumpcache[lump];
}
```

The `tag` parameter controls the zone purge priority:
- `PU_STATIC` — never purged (used for essential data like palettes, colormaps)
- `PU_LEVEL` — purged when a new level loads (`Z_FreeTags(PU_LEVEL, PU_PURGELEVEL)`)
- `PU_CACHE` — purged whenever Z_Malloc needs space (used for textures and sprites)

This means infrequently-seen textures may be loaded, evicted under memory pressure, and reloaded — all transparently. The zone allocator handles eviction automatically.

#### Why the WAD Takes 10-15 Seconds to Load

> **Sidebar: Why Front-Loading Is Intentional**
>
> The startup delay is not a bug. Here is what actually happens during those 10-15 seconds:
>
> 1. **`W_InitMultipleFiles`** reads the 20 KB lump directory off HFS (slow SCSI-emulated storage, ~1 MB/s sequential).
> 2. **`R_InitData`** (called from `R_Init`) composites all multi-patch textures. DOOM textures are not stored as flat bitmaps — they are defined as a list of named patches (sprites) positioned at offsets within a bounding box. `R_InitData` reads each `TEXTURE1`/`TEXTURE2` lump, allocates a `texture_t` struct for each texture, and builds the `texturecolumnlump[]` / `texturecolumnofs[]` lookup arrays used by the column drawers at runtime. doom1.wad has 125 textures; doom2.wad has 277.
> 3. **`R_InitSprites`** parses all sprite rotation tables.
> 4. **`ST_Init` / `HU_Init`** cache font and status bar patches into zone memory.
>
> The alternative — lazy-loading textures during gameplay — would cause hitching every time an unseen texture first appeared on screen. On a machine running at 6-8 FPS with a slow disk, that hitch could be several seconds. Front-loading trades startup time for a smooth (by SE/30 standards) in-game experience.
>
> A natural follow-up question is whether the WAD could be pre-processed on a modern machine and shipped as a ready-to-load binary blob. This was considered and rejected — see Opt 16 in Part II.

---

### 2.4 `z_zone.c` — Zone Memory Allocator

Classic Mac OS 7 has `NewPtr` / `NewHandle` for heap allocation, but DOOM does not use them for game data. Instead, all game memory comes from a single large block obtained once at startup, managed by DOOM's own allocator: the **zone**.

The motivation: DOOM needs to be able to evict cached textures automatically when memory is tight, and to free entire categories of memory (level data) in a single call at level transition. The standard C heap provides neither. The zone allocator provides both via *tagged blocks*.

#### Memory Layout

**Figure 9 — Zone Allocator Block Layout:**

```
  I_ZoneBase() returns one contiguous block from NewPtr (tries 48MB → 2MB fallback)

  mainzone
  ┌─────────────────────────────────────────────────────────────────────┐
  │ memzone_t header                                                    │
  │ { size, blocklist (sentinel), rover }                               │
  ├──────────┬──────────────┬──────────┬──────────┬────────────────────┤
  │ memblock │ memblock     │ memblock │ memblock │ memblock           │
  │ STATIC   │ LEVEL        │ FREE     │ CACHE    │ FREE               │
  │ palette  │ level geom   │ (gap)    │ texture  │ (available)        │
  │ tag=1    │ tag=2        │ user=0   │ tag=100  │ user=0             │
  └──────────┴──────────────┴──────────┴──────────┴────────────────────┘
       ▲                                    ▲
       │                                    │
   PU_STATIC=1                         PU_CACHE=100
   never evicted                       evicted first

  Each memblock_t:
  ┌──────────┬────────────┬─────────┬───────────┬────────┬────────┐
  │ size     │ user**     │ tag     │ id        │ *next  │ *prev  │
  │ (incl    │ (ptr to    │ purge   │ 0x1d4a11  │        │        │
  │  header) │  user ptr) │ priority│ sentinel  │        │        │
  └──────────┴────────────┴─────────┴───────────┴────────┴────────┘
```

#### How Allocation Works

`Z_Malloc(size, tag, user)` scans forward from a `rover` pointer through the block list. For each block:
- If it's free (`user == NULL`): use it if large enough
- If it's purgeable (`tag >= PU_PURGELEVEL`): evict it (null the user's pointer, mark free), then use it if large enough
- If it's static or level (`tag < PU_PURGELEVEL`): skip it

When a sufficiently large space is found, the block is split if the leftover exceeds `MINFRAGMENT` (64 bytes). The `user` parameter is a pointer-to-pointer: the allocator writes the block's address into `*user` and saves `user` in the block header. This allows `Z_Free` to automatically null the caller's pointer, and allows the allocator to update pointers if blocks were ever moved (they aren't in this implementation, but the architecture supports it).

#### Tag Hierarchy

| Tag constant | Value | Meaning |
|---|---|---|
| `PU_STATIC` | 1 | Never purged. Used for: palette, colormaps, lookup tables |
| `PU_SOUND` | 2 | Never purged. Sound data |
| `PU_MUSIC` | 3 | Never purged. Music data |
| `PU_LEVEL` | 50 | Freed on level transition (`Z_FreeTags(PU_LEVEL, PU_PURGELEVEL-1)`) |
| `PU_LEVSPEC` | 51 | Level specials (thinkers). Freed with level |
| `PU_CACHE` | 100 | Purgeable at any time. Used for: textures, sprites, lumps |

`Z_FreeTags(lowtag, hightag)` walks the entire block list and frees every block whose tag falls in the range. Called at level transition to reclaim all `PU_LEVEL` and `PU_LEVSPEC` blocks in one pass.

#### `Z_CheckHeap`

During debug builds, `Z_CheckHeap()` validates the entire block list: checks that `next->prev == current`, that no two consecutive free blocks exist (they should have been merged), and that no block's `id` field has been overwritten (would indicate a buffer overrun). It is called from `D_DoomLoop` periodically and logs results to `doom_log.txt`.

---

---

## Chapter 3: The Renderer (Deep Dive)

DOOM's renderer is a software rasterizer built on a **Binary Space Partition (BSP)** tree. It has no Z-buffer, no GPU, and no floating-point math in the hot path. Every pixel on screen is computed by the CPU using integer fixed-point arithmetic, and on the SE/30 every pixel must also be converted to 1-bit monochrome.

Understanding the renderer is the key to understanding the SE/30 optimizations. Most of the performance work in Part II targets exactly the code described in this chapter.

**Figure 5 — Rendering Pipeline:**

```
  R_RenderPlayerView()
       │
       ├─ R_SetupFrame()
       │   Set viewx/y/z/angle/sin/cos from player position
       │   Select colormap (fullbright or distance-based lighting)
       │
       ├─ R_ClearClipSegs()      solidsegs[] = [-∞ .. +∞] (nothing clipped yet)
       ├─ R_ClearDrawSegs()      drawsegs pointer reset
       ├─ R_ClearPlanes()        visplanes reset, floorclip/ceilingclip = screen edges
       ├─ R_ClearSprites()       vissprite list reset
       │
       ├─ R_RenderBSPNode()  ◀── PHASE 1: BSP traversal
       │   Walk BSP tree front-to-back
       │   For each visible wall seg → R_StoreWallRange()
       │     For each column of each wall → colfunc()  [writes directly to 1-bit FB]
       │     Mark floor/ceiling visplanes
       │     Project sprites in subsectors → vissprites[]
       │
       ├─ R_DrawPlanes()     ◀── PHASE 2: Floors & ceilings
       │   For each visplane → R_MakeSpans() → spanfunc()
       │   (skipped entirely when opt_solidfloor=1)
       │
       └─ R_DrawMasked()    ◀── PHASE 3: Sprites & masked walls
           Depth-sort vissprites[] (back to front)
           For each sprite → R_DrawVisSprite() → colfunc()
           For each masked wall → R_RenderMaskedSegRange() → colfunc()
```

The three phases are strictly ordered: BSP first (establishes occlusion and clip arrays), then floors/ceilings (need clip arrays from BSP), then sprites/masked (need depth info from BSP).

---

### 3.1 Coordinate Systems and Fixed-Point Math

#### The World

DOOM's world coordinate system is a 2D top-down map with a vertical (Z) axis for floor/ceiling heights. Coordinates are measured in map units. One map unit ≈ 3 cm in the game's fiction, but the actual scale is arbitrary — what matters is that player height is 56 units, a standard wall is 128 units tall, and doorways are 72 units.

All positions, distances, and angles are **fixed-point integers** — there is no `float` or `double` anywhere in the hot render path.

#### `fixed_t` — 16.16 Fixed Point

```c
typedef int fixed_t;
#define FRACBITS  16
#define FRACUNIT  (1 << FRACBITS)   // = 65536 = "1.0"
```

A `fixed_t` is a 32-bit signed integer where the top 16 bits are the integer part and the bottom 16 bits are the fractional part. `FRACUNIT` (65536) represents 1.0. A wall at distance 2.5 map units would be stored as `2.5 * 65536 = 163840`.

Multiplication of two `fixed_t` values requires a 64-bit intermediate result to avoid overflow:
```c
#define FixedMul(a,b)  ((fixed_t)(((long long)(a) * (b)) >> FRACBITS))
```
This is the single most-called operation in the renderer. See Opt 3 in Part II for why it's a macro.

Division:
```c
fixed_t FixedDiv2(fixed_t a, fixed_t b) {
    return (fixed_t)(((long long)a << FRACBITS) / b);
}
```

#### Angles — `angle_t`

Angles are unsigned 32-bit integers covering the full circle: `0` = east, `ANG90` = north, `ANG180` = west, `ANG270` = south. This wraps naturally with integer overflow. The fine angle system divides the circle into `FINEANGLES` (8192) steps, stored in the `finesine[]` and `finecosine[]` tables in `tables.c`. To convert an `angle_t` to a fine angle index: `angle >> ANGLETOFINESHIFT` (= `angle >> 19`).

The advantage of integer angles: wraparound is free, comparison is fast, and the trig tables are pre-computed integers (no `sin()` / `cos()` calls at runtime).

---

### 3.2 `r_main.c` — View Setup and Render Coordination

`r_main.c` owns two things: the **one-time initialization** of all rendering tables, and the **per-frame view setup** before BSP traversal begins.

#### Projection

DOOM uses a simple perspective projection. The relationship between world distance `dist` and screen scale `scale` is:

```
  scale = projection / dist

  where: projection = (viewwidth/2) / tan(FOV/2)
                    = centerxfrac / TANFOV
```

`projection` is computed once at startup and stored as a `fixed_t`. `scale` (called `rw_scale` per wall segment) tells the renderer how tall a world-unit-high wall appears on screen at that distance. A wall 1 unit tall at scale `s` occupies `s` pixels vertically.

The inverse — `iscale = 1.0 / scale` — is used in column drawers as the texture DDA step: for each pixel down the column, advance `iscale` into the texture vertically.

#### Texture Mapping Tables

At startup, `R_InitTextureMapping` builds two critical arrays:

- **`viewangletox[angle]`** — for a ray at `angle` from the player, which screen column does it hit? Range 0..viewwidth.
- **`xtoviewangle[x]`** — inverse: for screen column `x`, what world angle does that ray correspond to? Used during wall rendering to compute texture offset.

These eliminate per-column trig during rendering. Instead of computing `atan2` for each column, the renderer just looks up `xtoviewangle[x]`.

#### Light Tables

`R_InitLightTables` precomputes two arrays used for distance-based lighting:

- **`zlight[LIGHTLEVELS][MAXLIGHTZ]`** — indexed by `[sector light level][distance]`, gives the colormap index to use for that sector/distance combination. Brighter sectors stay bright further away; dark sectors go black quickly.
- **`scalelight[LIGHTLEVELS][MAXLIGHTSCALE]`** — same idea but indexed by wall scale (used for walls, which use scale rather than raw distance).

A colormap is a 256-byte table remapping palette indices: colormap 0 = fullbright (identity), colormap 31 = almost black (darkest). The renderer picks a colormap per wall/sprite based on these tables and passes it to the column drawer as `dc_colormap`.

#### Function Pointer Dispatch

The renderer never calls column and span drawing functions directly. Instead, it calls through global function pointers:

```c
void (*colfunc)(void);       // standard wall/sprite column
void (*basecolfunc)(void);   // saved copy of standard colfunc
void (*fuzzcolfunc)(void);   // invisibility effect
void (*transcolfunc)(void);  // color-translated (player sprites)
void (*spanfunc)(void);      // floor/ceiling spans
```

`R_ExecuteSetViewSize` sets these pointers based on `detailshift` and whether the game is in direct-render mode:

```
detailshift=0 (HIGH):   colfunc = R_DrawColumn_Mono
detailshift=1 (LOW):    colfunc = R_DrawColumnLow_Mono
detailshift=2 (QUAD):   colfunc = R_DrawColumnQuadLow_Mono
detailshift=3 (MUSH):   colfunc = R_DrawColumnMushLow_Mono
menuactive/wipe:        colfunc = R_DrawColumn  (8-bit fallback)
```

This dispatch is the mechanism by which the entire rendering system switches between quality levels and between the direct 1-bit path (gameplay) and the 8-bit intermediate path (menus/wipes/intermission) — without any `if` statements in the hot column-draw loop.

---

### 3.3 `r_bsp.c` — BSP Tree Traversal

#### What a BSP Tree Is

DOOM's levels are preprocessed offline by a BSP builder tool. The result is a binary tree stored in the WAD where:
- **Interior nodes** (`node_t`) each contain a partition line and two child references (left/right subtrees), plus bounding boxes for each subtree
- **Leaf nodes** (`subsector_t`) contain a list of wall segments (`seg_t`) forming a convex polygon of the level

The key property: for any viewpoint, traversing the BSP tree front-to-back (always visit the child the viewpoint is on first) visits geometry in painter's-algorithm order. The renderer can draw walls as it encounters them, using a clip list to stop drawing when the screen is fully covered. No sorting, no Z-buffer.

**Figure 6 — BSP Traversal:**

```
  BSP tree root
       │
  R_RenderBSPNode(root)
       │
       ├─ Which side of the partition line is the viewpoint on?
       │   R_PointOnSide(viewx, viewy, node)
       │
       ├─ Recurse: FRONT child first  ◀── closer geometry drawn first
       │
       ├─ Is the BACK child's bounding box visible at all?
       │   R_CheckBBox(node->bbox[backside])
       │   └─ clips bbox against view frustum + solidsegs clip list
       │      If fully clipped → prune entire subtree (fog culling also here)
       │
       └─ Recurse: BACK child
            │
            └─ If leaf (NF_SUBSECTOR bit set):
                 R_Subsector(subsector)
                 For each seg in subsector:
                   R_AddLine(seg)
                     ├─ Clip seg to view frustum [-clipangle .. +clipangle]
                     ├─ If solid wall: R_ClipSolidWallSegment → R_StoreWallRange
                     └─ If window:     R_ClipPassWallSegment  → R_StoreWallRange

  solidsegs[] — the clip list:
  ┌──────┬──────┬──────┬──────┬──────┐
  │-∞..─1│      │25..67│      │200..∞│   ← ranges of screen columns fully occluded
  └──────┘      └──────┘      └──────┘
  As solid walls are drawn, their column ranges are merged into solidsegs[].
  Once solidsegs covers [-∞..∞] the entire screen is filled → traversal can stop.
```

#### `R_CheckBBox` — Subtree Pruning

Before recursing into a BSP subtree, `R_CheckBBox` tests the subtree's axis-aligned bounding box against the view frustum. It uses a precomputed `checkcoord[4][4]` table (based on which side of horizontal/vertical the box sits relative to the viewpoint) to select which two corners of the box to test. If both corners are outside the frustum, the entire subtree is culled — no further recursion, no wall rendering. On large open levels this prunes a substantial fraction of the tree.

---

### 3.4 `r_segs.c` — Wall Segment Rendering

`r_segs.c` contains the hottest code in the codebase. `R_RenderSegLoop` — the innermost wall-rendering loop — is where the majority of frame time is spent.

#### `R_StoreWallRange` — Wall Setup

When the BSP traversal finds a visible wall segment, `R_StoreWallRange(start, stop)` computes everything needed to render that segment's columns. Key computations:

**Scale at each endpoint:**
```
  rw_scale     = projection / (cos(rw_normalangle - viewangle) * rw_distance)
  rw_scalestep = (scale_right - scale_left) / (stop - start)
```
Scale is computed at both endpoints and stepped linearly across the seg (see Opt 10 — `iscale` linear interpolation).

**Texture vertical positioning:**
```
  rw_midtexturemid = frontsector->floorheight + midtexture->height
                   - viewz + sidedef->rowoffset
```
This gives the texture coordinate at the top of the screen column for the wall's center texture.

**Wall tiers:** A wall segment can have up to three texture tiers:
- **Upper texture** — the wall above a window opening (between a higher front ceiling and lower back ceiling)
- **Middle texture** — a solid wall, or a transparent texture over a window
- **Lower texture** — the wall below a window opening

Each tier is rendered as a separate range of columns by `R_RenderSegLoop`.

#### `R_RenderSegLoop` — The Inner Loop

This is the innermost loop of the entire renderer, called once per visible wall column per frame.

```c
while (rw_x < rw_stopx)
{
    // 1. Compute iscale (texture DDA step) — linear interp (see Opt 10)
    dc_iscale = rw_iscale;
    rw_iscale += rw_iscalestep;

    // 2. Texture column (which vertical strip of the texture to draw)
    //    Affine step (Opt 9) or per-column FixedMul
    texturecolumn = rw_offset - FixedMul(xtoviewangle[rw_x], rw_distance);

    // 3. Lighting — pick colormap based on scale (distance)
    index = rw_scale >> LIGHTSCALESHIFT;
    dc_colormap = walllights[index];

    // 4. Clip top/bottom to floor/ceiling
    dc_yl = MAX(ceilingclip[rw_x] + 1, yh_from_scale);
    dc_yh = MIN(floorclip[rw_x]  - 1, yl_from_scale);

    // 5. Update clip arrays for floors/ceilings
    ceilingclip[rw_x] = ...;
    floorclip[rw_x]   = ...;

    // 6. Mark visplanes
    if (markceiling) ceilingplane = R_CheckPlane(ceilingplane, rw_x, rw_x);
    if (markfloor)   floorplane   = R_CheckPlane(floorplane,   rw_x, rw_x);

    // 7. Draw the column (or skip if fogged — see Opt 13)
    if (!in_fog) {
        dc_source = texture_column_pointer;
        colfunc();    // → R_DrawColumnQuadLow_Mono or similar
    }

    rw_x++;
    rw_scale += rw_scalestep;
}
```

**Figure 7 — Wall Column Math:**

```
  Top-down world view:              Screen view:

  Viewpoint (V)                     Screen column x
       │                                 │
       │  rw_distance                    │  dc_yl (top clip)
       │◀────────────────▶│              │
       │                  Wall seg       │  texture pixels
       │  rw_normalangle ↗               │  dc_iscale = 1/scale
       │                                 │  (step per pixel down)
       │                                 │
  scale = projection / rw_distance       │  dc_yh (bottom clip)
  iscale = rw_distance / projection      │

  Texture vertical DDA:
  For each pixel at screen row y (from dc_yl to dc_yh):
    texrow = dc_texturemid + (y - centery) * dc_iscale
    pixel  = dc_source[texrow >> FRACBITS]   ← palette index
    gray   = grayscale_pal[colormap[pixel]]  ← 0-255 luminance
    1bit   = (gray > bayer4x4[y&3][x&3])    ← Bayer threshold
    write bit to fb_mono_base
```

The `dc_iscale` value is the amount to advance in texture space per screen pixel. At close range, `iscale` is small (many screen pixels per texel — magnified). At far range, `iscale` is large (many texels per screen pixel — minified). This is the perspective-correct texture mapping DDA.

---

### 3.5 `r_plane.c` — Floors and Ceilings

Floors and ceilings are rendered completely differently from walls. Where walls are rendered column-by-column during BSP traversal, floors and ceilings are rendered **after** the BSP pass, span-by-span (horizontally).

#### Visplanes

During BSP traversal, each floor and ceiling surface is registered as a `visplane_t`. A visplane represents one contiguous floor or ceiling surface with a single height, texture, and light level. Multiple separate floor areas at the same height/texture/light share one visplane (their column ranges are merged).

```c
typedef struct {
    fixed_t  height;        // world height of this surface
    int      picnum;        // flat (texture) index
    int      lightlevel;    // sector light level
    int      minx, maxx;    // screen column range
    byte     top[SCREENWIDTH];    // top clip per column (from ceilingclip[])
    byte     bottom[SCREENWIDTH]; // bottom clip per column (from floorclip[])
} visplane_t;
```

`R_FindPlane` looks up an existing visplane matching height/pic/lightlevel, or allocates a new one if none exists. `R_CheckPlane` validates that the column range being added is contiguous — if there's a gap, the visplane is split.

#### Span Rendering

`R_DrawPlanes` enumerates all visplanes and for each one calls `R_MakeSpans`, which decomposes the visplane's per-column top/bottom clips into horizontal spans. For each span, `R_MapPlane` computes the world-space texture coordinates:

```
  For a horizontal span at screen row y, from x1 to x2:

  planeheight = abs(height - viewz)
  distance    = planeheight * yslope[y]       ← pre-computed table
  ds_xstep    = distance * basexscale         ← world units per screen pixel
  ds_ystep    = distance * baseyscale
  ds_xfrac    = viewx + distance * viewcos - (x1-centerx) * ds_xstep
  ds_yfrac    = viewy - distance * viewsin - (x1-centerx) * ds_ystep
```

Then `spanfunc()` (→ `R_DrawSpan_Mono` or `R_DrawSpanQuadLow_Mono`) steps `ds_xfrac` and `ds_yfrac` by `ds_xstep`/`ds_ystep` for each pixel, indexing into the flat texture.

#### `opt_solidfloor` — The Big Win

When `opt_solidfloor=1`, `R_DrawPlanes` skips all of the above for non-sky surfaces. Instead of computing texture coordinates and calling `spanfunc`, the floor/ceiling areas are filled with a solid gray value by `I_FinishUpdate`. This eliminates the entire visplane management and span-rendering cost — a very significant saving (see Opt 12).

The sky ceiling is still rendered normally even with `opt_solidfloor=1` — sky detection uses `picnum == skyflatnum` and is exempted from the skip.

---

### 3.6 `r_things.c` — Sprites

Sprites (monsters, pickups, explosions, decorations) are game objects (`mobj_t`) projected from 3D world coordinates to 2D screen coordinates, then rendered as masked vertical strips.

#### Projection — `R_ProjectSprite`

For each `mobj_t` in the subsectors visited during BSP traversal, `R_ProjectSprite` is called:

1. **Transform** the object's world position relative to the viewpoint
2. **Perspective divide** to get screen X and scale
3. **Select frame/rotation**: the sprite definition (`spritedef_t`) for this object type contains rotation variants (up to 8 orientations). The renderer picks the variant whose angle best matches the viewing direction, optionally flipping horizontally if the symmetric variant is used
4. **Fog cull**: if `xscale < fog_scale`, skip decorative objects (anything not `MF_COUNTKILL | MF_SPECIAL | MF_MISSILE`). Enemies, pickups, and projectiles are always drawn regardless of fog distance
5. **Allocate `vissprite_t`**: record screen x1/x2, scale, texture pointer, colormap, translation table (for colored player sprites), and whether to flip

#### Depth Sorting

After BSP traversal, `vissprites[]` contains all visible sprites in BSP (approximately front-to-back) order. `R_DrawMasked` sorts them back-to-front by scale (larger scale = closer) using a simple insertion sort. Sprites are then drawn in that order — painter's algorithm.

This is approximate. Two sprites at nearly the same distance whose screen projections overlap may render in the wrong order (a known DOOM quirk, especially visible with stacked corpses). A true per-pixel Z-buffer would fix it but is far too expensive on the SE/30.

#### `R_DrawVisSprite`

For each sprite column from `x1` to `x2`:
1. Compute the texture column index from the sprite's `patch_t` data
2. Look up the column pointer in the patch (post-based format — see §3.7)
3. Set `dc_yl`/`dc_yh` from the sprite's screen-space vertical extent, clipped to `mfloorclip[]`/`mceilingclip[]` (per-column clip arrays set by walls)
4. Call `colfunc()` (or `transcolfunc()` for translated player sprites)

The floor/ceiling clip arrays ensure sprites don't bleed through walls — a sprite standing behind a wall is correctly occluded because the wall's solid clip range covers those columns.

---

### 3.7 `r_draw.c` — Pixel Writers

`r_draw.c` contains the actual pixel-level drawing functions. Everything above this layer works in world coordinates, texture space, or screen-space integers. This layer writes bits to memory.

#### Post-Based Sprite/Texture Format

Sprites and masked textures are stored in the WAD as `patch_t` structures using a **post-based column format**. Each column is a sequence of *posts*: runs of non-transparent pixels separated by gaps (transparent rows). This allows sparse sprites (where most pixels are transparent) to skip transparent sections entirely:

```
  Column data layout:
  ┌─────────────────────────────────────────────────┐
  │ topdelta (byte) — y offset of this post         │
  │ length   (byte) — pixel count in this post      │
  │ unused   (byte) — padding                       │
  │ pixel[0..length-1] — palette indices            │
  │ unused   (byte) — padding                       │
  │ ... next post ...                               │
  │ 0xFF — end of column sentinel                   │
  └─────────────────────────────────────────────────┘
```

`R_DrawMaskedColumn` iterates posts, clipping each to `dc_yl`/`dc_yh`, and calls `colfunc()` for each visible run.

#### Column Drawer — Direct 1-bit Path

The SE/30-specific column drawers (`R_DrawColumn_Mono`, `R_DrawColumnQuadLow_Mono`, etc.) write directly to the 1-bit framebuffer. The core loop for the standard 1-pixel-wide drawer:

```c
void R_DrawColumn_Mono(void)
{
    int      count  = dc_yh - dc_yl + 1;
    fixed_t  frac   = dc_texturemid + (dc_yl - centery) * dc_iscale;
    byte    *dest   = fb_mono_base
                    + (dc_yl + fb_mono_yoff) * fb_mono_rowbytes
                    + ((dc_x + fb_mono_xoff) >> 3);
    int      bitpos = 7 - ((dc_x + fb_mono_xoff) & 7);   // bit within byte

    while (count--)
    {
        byte  src  = dc_source[frac >> FRACBITS];         // texture palette index
        byte  gray = grayscale_pal[dc_colormap[src]];     // 0-255 luminance
        int   row  = (dc_yl + written) & 3;
        int   col  = dc_x & 3;

        if (gray > bayer4x4[row][col])
            *dest &= ~(1 << bitpos);   // white pixel (0 = lit in Mac 1-bit)
        else
            *dest |=  (1 << bitpos);   // black pixel

        frac += dc_iscale;
        dest += fb_mono_rowbytes;
    }
}
```

Note the Mac 1-bit convention: **0 = white (lit), 1 = black (dark)**. This is inverted from the intuitive expectation. Every bit operation in the blit code accounts for this.

#### QUAD Column Drawer

`R_DrawColumnQuadLow_Mono` draws 4 pixels wide, writing one nibble per row:

```c
// 4 pixels wide — one nibble per row, nibble-aligned
byte nibble_mask = (dc_x & 4) ? 0x0F : 0xF0;   // which nibble to write

while (count--)
{
    byte src  = dc_source[frac >> FRACBITS];
    byte gray = grayscale_pal[dc_colormap[src]];
    byte pat  = (gray > bayer4x4[row & 3][0]) ? 0x00 : 0xFF; // all 4 pixels same

    *dest = (*dest & ~nibble_mask) | (pat & nibble_mask);
    frac += dc_iscale;
    dest += fb_mono_rowbytes;
}
```

Writing a full nibble (4 bits) at once avoids the read-modify-write cost of single-bit manipulation, which is why QUAD mode is significantly faster than LOW mode despite only halving the horizontal resolution again.

#### Bayer Dithering

All direct 1-bit drawers use Bayer ordered dithering to convert 8-bit grayscale to 1-bit. The Bayer matrix is a 4×4 threshold grid:

**Figure 13 — Bayer Dithering:**

```
  Bayer 4×4 threshold matrix (normalized 0-255):

  ┌──────┬──────┬──────┬──────┐
  │   0  │ 136  │  34  │ 170  │
  ├──────┼──────┼──────┼──────┤
  │ 204  │  68  │ 238  │ 102  │
  ├──────┼──────┼──────┼──────┤
  │  51  │ 187  │  17  │ 153  │
  ├──────┼──────┼──────┼──────┤
  │ 255  │ 119  │ 221  │  85  │
  └──────┴──────┴──────┴──────┘

  For each pixel at screen position (x, y):
    gray      = grayscale_pal[colormap[palette_index]]   ← 0-255
    threshold = bayer4x4[y & 3][x & 3]                  ← from table above
    result    = (gray > threshold) ? WHITE : BLACK

  Example — 50% gray (gray=128):
  ┌─┬─┬─┬─┐     ┌─┬─┬─┬─┐
  │W│B│B│W│     │ │█│█│ │   128 > 0   ✓ → white
  ├─┼─┼─┼─┤     ├─┼─┼─┼─┤   128 > 136 ✗ → black
  │B│W│B│W│  =  │█│ │█│ │   128 > 204 ✗ → black
  ├─┼─┼─┼─┤     ├─┼─┼─┼─┤   128 > 68  ✓ → white  ... and so on
  │B│W│W│W│     │█│ │ │ │
  ├─┼─┼─┼─┤     ├─┼─┼─┼─┤
  │B│W│B│W│     │█│ │█│ │
  └─┴─┴─┴─┘     └─┴─┴─┴─┘
  Approx 50% of pixels lit — visually appears as mid-gray
```

> **Sidebar: Why Bayer Dithering?**
>
> Several dithering algorithms were considered:
>
> **Floyd-Steinberg error diffusion**: Produces the highest visual quality — it propagates quantization error to neighboring pixels, creating smooth gradients. Ruled out because: (a) it requires a full-width error buffer (320 bytes) that must be read and written every row, adding cache pressure; (b) it requires serpentine (left-right-left) scan order, meaning rows cannot be rendered independently — incompatible with column-based rendering; (c) it is inherently sequential and cannot be parallelized across columns.
>
> **Random / noise dithering**: Adds random noise at each pixel. Cheap to compute but produces temporal flickering — since the random pattern changes every frame, static surfaces appear to "boil." Unacceptable at 6-8 FPS where flicker is very visible.
>
> **Simple threshold (no dithering)**: The cheapest option — just `gray > 128`. Produces harsh posterization with no intermediate tones. Unacceptable visually.
>
> **Bayer ordered dithering** wins on all practical criteria: O(1) per pixel (two table lookups + one comparison), deterministic (same input → same output, no flicker), independent per pixel (any column can be rendered in any order), 4×4 matrix fits in 16 bytes (stays in the 68030's 256-byte instruction cache), and produces visually acceptable quality for the 512×342 display.

#### The `grayscale_pal` and `mono_colormaps` Lookup Chain

The full lookup chain from palette index to screen bit:

```
  palette_index (0-255)
       │  dc_colormap[] — 256-byte distance-lighting remap
       ▼
  lit_index (0-255)
       │  grayscale_pal[] — palette index → 0-255 luminance
       │  (incorporates BT.601 RGB weights + gamma correction)
       ▼
  gray (0-255)
       │  > bayer4x4[y&3][x&3]
       ▼
  1-bit pixel

  Optimization: mono_colormaps[] merges dc_colormap[] + grayscale_pal[]
  into a single 34×256 table, collapsing the first two lookups into one.
```

`I_BuildMonoColormaps()` (called on every palette change) constructs `mono_colormaps` by composing each of DOOM's 34 colormaps with `grayscale_pal`. The column drawers then use `mono_colormaps[colormap_index * 256 + palette_index]` directly, saving one table-lookup per pixel.

#### Fuzz Column (Invisibility Effect)

`R_DrawFuzzColumn_Mono` implements the partial-invisibility powerup. In the original 8-bit DOOM, this reads a neighboring pixel from the framebuffer and remaps it through colormap 6 (darkening). In the 1-bit monochrome version, the effect is implemented as a stipple: a fixed pattern of pixels is XOR'd with the underlying framebuffer content, creating a "see-through" flickering appearance. The fuzz pattern cycles through `fuzzoffset[]` each tic to animate.

---

---

## Chapter 4: Physics & Game Logic

The physics and game logic layer sits between the game state machine (`g_game.c`) and the renderer. It owns the simulation: where objects are, how they move, what happens when they collide, and how monsters decide what to do. This layer runs at a fixed 35 Hz regardless of render rate.

The code is split across many `p_*.c` files, each owning a narrow slice of responsibility. The central data structure throughout is `mobj_t` — the game object — and the central spatial structure is the **blockmap**.

---

### 4.1 Core Data Structures

Before diving into individual files, two data structures appear everywhere and must be understood first.

#### `mobj_t` — Mobile Object

Every entity in the game world — players, monsters, projectiles, pickups, decorations, corpses — is an `mobj_t`:

```c
typedef struct mobj_s {
    // World position
    fixed_t     x, y, z;
    fixed_t     floorz, ceilingz;    // floor/ceiling at current position
    fixed_t     radius, height;      // collision cylinder

    // Motion
    fixed_t     momx, momy, momz;    // velocity (fixed_t units per tic)
    angle_t     angle;               // facing direction

    // State machine
    state_t    *state;               // current animation/behavior state
    int         tics;                // tics remaining in this state
    mobjtype_t  type;                // what kind of object (MT_PLAYER, MT_ZOMBIE, ...)
    mobjinfo_t *info;                // pointer into mobjinfo[] table

    // Game logic
    mobj_t     *target;              // current attack target (monsters)
    mobj_t     *tracer;              // homing missile target
    int         health;
    int         flags;               // MF_SOLID, MF_SHOOTABLE, MF_COUNTKILL, ...

    // Linked lists
    struct mobj_s *snext, *sprev;    // sector thing list
    struct mobj_s *bnext, *bprev;    // blockmap thing list
    subsector_t   *subsector;        // which BSP leaf this object is in

    // Rendering
    spritenum_t   sprite;
    int           frame;
    fixed_t       floorclip;        // sprite clipping for water/slime
} mobj_t;
```

The `state` / `tics` pair implements a simple state machine. Each state points to a sprite frame, a duration in tics, an optional action function (called once per tic while in that state), and the next state. When `tics` reaches zero the object transitions to `state->nextstate`. This drives all animation and behavior — a zombie walking, shooting, dying, and becoming a corpse is just a sequence of state transitions defined in `info.c`.

#### The Blockmap

The blockmap is a spatial hash that divides the level into a grid of 128×128 unit blocks. Each cell contains a linked list of `mobj_t` objects currently occupying it and a list of `line_t` linedefs intersecting it.

```
  Level footprint divided into 128-unit grid cells:

  ┌────┬────┬────┬────┐
  │    │    │█   │    │   Each cell: linked list of lines + linked list of things
  ├────┼────┼────┼────┤
  │    │  V │    │    │   V = viewpoint, █ = monster
  ├────┼────┼────┼────┤
  │    │    │    │    │
  └────┴────┴────┴────┘

  Broad-phase collision: only check things/lines in cells the mover overlaps.
  For a radius-R object moving from A to B:
    Check all blocks within bbox(A,B) expanded by R.
```

Without the blockmap, every movement check would need to test against all ~400 linedefs and all active mobjs in the level. With the blockmap, only a handful of cells need to be examined. `P_BlockThingsIterator` and `P_BlockLinesIterator` (in `p_maputl.c`) provide the iteration interface used throughout `p_map.c`.

---

### 4.2 `p_setup.c` — Level Loading

`P_SetupLevel` is called by `G_DoLoadLevel` each time a map is entered. It reads a fixed set of lumps from the WAD in order, builds the level data structures, and spawns all map objects.

#### Lump Loading Order

Each MAP lump is identified by name (e.g., `E1M1` or `MAP01`) followed by a fixed sequence of sub-lumps. The loader reads them by offset from the marker:

```
  WAD lump sequence for each level:

  E1M1          ← marker (zero-length)
  THINGS        ← spawn positions, types, flags, angles  (10 bytes each)
  LINEDEFS      ← wall lines: vertex refs, flags, special, tag, sidedef refs
  SIDEDEFS      ← texture names + offsets, sector ref  (30 bytes each)
  VERTEXES      ← x/y coordinates (4 bytes each, little-endian)
  SEGS          ← BSP wall segments: vertex refs, angle, linedef ref, side, offset
  SSECTORS      ← BSP leaf nodes: seg count + first seg index
  NODES         ← BSP interior nodes: partition line, child refs, bboxes
  SECTORS       ← floor/ceiling height + texture, light level, type, tag
  REJECT        ← NxN bit matrix: sector-to-sector visibility pre-computation
  BLOCKMAP      ← spatial hash header + per-cell line lists
```

All integer fields are little-endian in the WAD. `P_LoadVertexes` converts 16-bit coordinates to `fixed_t` by shifting left `FRACBITS`. `P_LoadSectors`, `P_LoadSidedefs`, and `P_LoadLinedefs` cross-reference each other — a linedef refers to sidedefs by index, sidedefs refer to sectors by index.

#### Endianness Note

`p_setup.c` uses `SHORT(x)` (from `m_swap.h`) on every integer read from a WAD lump. On big-endian builds (the SE/30), `SHORT` byte-swaps its argument. On little-endian (PC), it's a no-op. The BSP node bounding boxes use `SHORT` too — important because the node format stores bbox coordinates as 16-bit signed shorts.

#### The Reject Matrix

The reject matrix is a pre-computed `numsectors × numsectors` bit array. Each bit `[i][j]` answers "can sector i ever see sector j?" If the bit is set, no LOS check is needed — the sectors are always mutually invisible. `P_CheckSight` consults this matrix first; if the reject bit is set it returns false immediately without any ray-casting. On levels with many sectors this eliminates a large fraction of LOS checks.

```c
// Reject matrix lookup:
int pnum   = (s1->sectornum * numsectors) + s2->sectornum;
int bytenum = pnum >> 3;
int bitnum  = 1 << (pnum & 7);
if (rejectmatrix[bytenum] & bitnum)
    return false;   // guaranteed no LOS — skip ray cast
```

#### Thing Spawning

After all geometry is loaded, `P_LoadThings` iterates the THINGS lump and calls `P_SpawnMapThing` for each entry. Thing entries contain a type number (looked up in `mobjinfo[]`), position, angle, and flags (skill level bitmask, multiplayer-only flag, deaf flag). Skill-level filtering happens here — a thing with skill flags `0b011` only spawns on skills 1 and 2. The "deaf" flag sets `MF_AMBUSH` on the spawned monster, preventing `P_NoiseAlert` from waking it up until it has LOS to a player.

---

### 4.3 `p_map.c` — Collision Detection and Movement

`p_map.c` is the most complex file in the physics layer. It implements all spatial queries: can this object move here? What does this hitscan ray hit? What objects are in blast radius? It uses the blockmap for all broad-phase queries and callback iterators for narrow-phase testing.

#### The Iterator Pattern

`p_map.c` uses a consistent pattern: define a callback function that examines one candidate line or thing, then invoke a blockmap iterator that calls it for every candidate in the relevant blocks. Global temporaries (`tmthing`, `tmx`, `tmy`, `tmflags`, `tmfloorz`, `tmceilingz`) carry state between the setup and the callbacks.

```c
// Example: checking if a position is valid for movement
boolean P_CheckPosition(mobj_t *thing, fixed_t x, fixed_t y)
{
    // Set up globals
    tmthing   = thing;
    tmx = x;  tmy = y;
    tmfloorz  = -MAXINT;
    tmceilingz = MAXINT;
    tmbbox[BOXTOP]    = y + thing->radius;
    tmbbox[BOXBOTTOM] = y - thing->radius;
    tmbbox[BOXRIGHT]  = x + thing->radius;
    tmbbox[BOXLEFT]   = x - thing->radius;

    // Iterate all lines in overlapping blocks → PIT_CheckLine
    P_BlockLinesIterator(x, y, PIT_CheckLine);

    // Iterate all things in overlapping blocks → PIT_CheckThing
    P_BlockThingsIterator(x, y, PIT_CheckThing);

    // Result: tmfloorz / tmceilingz now reflect
    // the highest floor and lowest ceiling in the move target area
    return (tmceilingz - tmfloorz >= thing->height);
}
```

`PIT_CheckLine` adjusts `tmfloorz`/`tmceilingz` for each line's front/back sector heights. `PIT_CheckThing` checks radius overlap with other objects and applies flags (`MF_SOLID` blocks movement; `MF_SPECIAL` triggers pickup). After both iterators complete, the gap between floor and ceiling tells whether the thing fits.

#### Movement — `P_TryMove`

`P_TryMove` wraps `P_CheckPosition` with the actual position update:

```
  P_TryMove(thing, newx, newy):
    1. P_CheckPosition → compute tmfloorz, tmceilingz
    2. If ceiling - floor < thing->height: return false (doesn't fit)
    3. If tmfloorz - thing->z > 24: return false (step too high)
    4. If tmceilingz < thing->z + thing->height: return false (bumps ceiling)
    5. For each special line hit (spechit[]):
         P_CrossSpecialLine() — trigger line specials (doors, switches)
    6. Update thing->x, thing->y, thing->floorz, thing->ceilingz
    7. Return true
```

The 24-unit step limit is the "stair climbing" threshold — players and monsters can step up ledges up to 24 units without jumping.

#### Wall Sliding — `P_SlideMove`

When `P_TryMove` fails (the move is blocked), rather than stopping dead, the game attempts to slide along the blocking surface. `P_SlideMove` uses `P_PathTraverse` to find the first blocking line, then computes a new velocity vector parallel to that line via `P_HitSlideLine`.

This gives DOOM its characteristic "gliding along walls" feel. Without it, hitting any wall at even a shallow angle would stop the player completely.

#### Hitscan — `P_AimLineAttack` / `P_LineAttack`

Hitscan weapons (pistol, shotgun, chaingun) use ray casting rather than projectiles. The two-step process:

```
  Step 1 — P_AimLineAttack(shooter, angle, range):
    Cast a ray, find the best target slope (auto-aim):
    · P_PathTraverse with PTR_AimTraverse callback
    · Callback records slope to each shootable thing in range
    · Returns the slope of the best target (or 0 for level)

  Step 2 — P_LineAttack(shooter, angle, range, slope, damage):
    Cast a ray at the computed slope:
    · P_PathTraverse with PTR_ShootTraverse callback
    · On thing hit: P_DamageMobj(target, ...)
    · On wall hit: spawn puff sprite at impact point
    · On sky: nothing (bullets don't bounce off sky)
```

`P_PathTraverse` traces a ray through the blockmap, collecting all line and thing intercepts along the ray and sorting them by distance. The callback is then invoked for each intercept in order from nearest to farthest. Returning `false` from the callback stops traversal (the ray is consumed).

#### Radius Attack — `P_RadiusAttack`

Explosions (rockets, barrel explosions) use `P_RadiusAttack`, which calls `P_BlockThingsIterator` on all blocks within the blast radius, then for each thing calls `PIT_RadiusAttack`:

```c
boolean PIT_RadiusAttack(mobj_t *thing)
{
    fixed_t dist = P_AproxDistance(thing->x - spot->x, thing->y - spot->y);
    dist = (dist - thing->radius) >> FRACBITS;  // in map units
    if (dist < 0) dist = 0;
    if (dist >= damage) return true;             // out of range

    if (P_CheckSight(thing, spot))               // LOS required
        P_DamageMobj(thing, spot, source, damage - dist);
    return true;
}
```

Damage falls off linearly from `damage` at the center to zero at the radius edge. LOS is required — objects behind walls are not damaged.

---

### 4.4 `p_enemy.c` — Monster AI

DOOM's monster AI is deliberately simple — the SE/30 cannot afford expensive pathfinding. Each monster runs a small decision tree once per tic via its state machine action functions.

#### The Thinker System

Every active object in the game has a `thinker_t` linked into the global thinker list. `P_Ticker` (called by `G_Ticker` once per tic) iterates the list and calls each thinker's `function`:

```c
void P_Ticker(void)
{
    thinker_t *th;
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
        if (th->function)
            th->function(th);   // P_MobjThinker for most objects
}
```

`P_MobjThinker` advances the object's state machine (decrements `tics`, transitions state when zero), applies momentum, handles gravity, and calls the current state's action function (e.g., `A_Chase`, `A_PosAttack`).

#### Chase Behavior — `A_Chase`

`A_Chase` is the action function for most monsters in their active (chasing) state:

```
  A_Chase(actor):
    1. Decrement melee/missile attack counters
    2. If target is dead or gone → find new target (P_LookForPlayers)
    3. Face target (A_FaceTarget)
    4. If melee range and melee attack ready → do melee
    5. Else if in missile range and missile attack ready → do missile
    6. P_Move() — try to step toward target
       If blocked → P_NewChaseDir() — pick new direction
    7. Play footstep/active sound if chase sound counter expired
```

#### Direction Selection — `P_NewChaseDir`

`P_NewChaseDir` computes the preferred movement direction toward the target using a 9-direction system (8 cardinal/diagonal + stopped). It tries directions in order of preference: direct toward target first, then diagonal alternatives, then perpendicular:

```
  Target is northeast of actor:
  Primary:   NORTH, EAST  (axis-aligned toward target)
  Diagonal:  NORTHEAST    (direct)
  Fallback:  SOUTH, WEST  (perpendicular)
  Last:      any non-blocked direction

  Each direction attempt calls P_TryMove — first one that succeeds is used.
  Result stored in actor->movedir and actor->movecount (tics in this direction).
```

This produces the characteristic DOOM monster navigation: mostly direct pursuit, with wandering when blocked. It is not pathfinding — monsters cannot navigate around corners they cannot directly see. They will press against a wall until `movecount` expires, then pick a new direction. Players exploit this constantly.

#### Line of Sight — `P_CheckSight`

LOS checking is one of the more expensive operations in the physics tick. `P_CheckSight` first consults the reject matrix (returns false immediately if the sectors are guaranteed non-visible), then casts a 2D ray through the BSP tree checking for blocking lines.

```
  P_CheckSight(t1, t2):
    1. Reject matrix lookup → false if guaranteed blocked
    2. sightcounts++ (debug)
    3. P_CrossBSPNode() — recursive BSP ray cast
       At each node: check if ray crosses partition plane
       At each seg: check if ray is blocked by opaque line
         (two-sided lines with floor-to-ceiling gap allow passage)
    4. Return true if ray reaches t2 without hitting opaque geometry
```

The BSP structure makes this efficient: the tree prunes large portions of the level at each node. Still, on levels with many active monsters all checking LOS simultaneously, this can be a meaningful fraction of logic tic time.

#### Sound Propagation — `P_RecursiveSound`

When a player fires a weapon or makes noise, `P_NoiseAlert` marks nearby monsters as aware. It does this by recursively flooding through sectors via two-sided linedefs (doorways, windows):

```
  P_RecursiveSound(sector, soundblocks):
    sector->soundtraversed = soundblocks + 1   (mark visited)
    sector->soundtarget    = soundtarget        (who made the noise)

    For each line bordering this sector:
      If line has no back sector: skip (solid wall, sound stops)
      If line has ML_SOUNDBLOCK flag: soundblocks++
      If soundblocks >= 2: skip (double-blocked, sound stops)
      P_RecursiveSound(back_sector, soundblocks)
```

Sound travels freely through open doorways. A single `ML_SOUNDBLOCK` line (e.g., a door) attenuates sound by one level. Two `ML_SOUNDBLOCK` lines fully stop it. This creates the game mechanic of "silent" rooms where monsters won't hear gunfire from the other side of two sound-blocking doors.

---

### 4.5 `p_inter.c` — Damage and Pickups

`p_inter.c` handles what happens when objects *interact* — when a player touches a pickup, or when something takes damage.

#### Pickup Dispatch — `P_TouchSpecialThing`

Called by `PIT_CheckThing` when a player overlaps a `MF_SPECIAL` object. It switches on the object's `sprite` to determine what was picked up and calls the appropriate give function:

```
  Ammo pickups:    P_GiveAmmo(player, type, count)
  Weapon pickups:  P_GiveWeapon(player, type, dropped)
  Health pickups:  P_GiveBody(player, amount)
  Armor pickups:   P_GiveArmor(player, type)
  Key pickups:     P_GiveCard(player, card)
  Power-ups:       P_GivePower(player, power)
```

Each give function checks whether the item can be picked up (e.g., `P_GiveBody` returns false if health is already at max), applies the effect, and returns a boolean. If false, the item is not removed. `P_GiveAmmo` also handles the "first ammo gives weapon" logic: picking up bullets for the first time gives the player the pistol if they don't have it.

#### Damage — `P_DamageMobj`

```
  P_DamageMobj(target, inflictor, source, damage):
    1. Ignore if target is not shootable (MF_SHOOTABLE not set)
    2. Ignore if target is invulnerable (pw_invulnerability active)
    3. Apply thrust: push target away from inflictor
       momx += FixedMul(push, cos(angle))
       momy += FixedMul(push, sin(angle))
    4. Special case: player damage reduced by armor
       saved   = damage * (armortype == 1 ? 0.33 : 0.50)
       armor  -= saved
       damage -= saved
    5. target->health -= damage
    6. If health <= 0: P_KillMobj(source, target)
       Else: check for pain state transition
             (if damage > info->painchance * random: set pain state)
    7. If source is a player and target is a monster:
         target->target = source   (monster now pursues the player who shot it)
```

The thrust application is important for feel: getting hit pushes the player (or monster) away from the damage source. Rocket explosions throw the player across the room. This is computed from the 2D angle between inflictor and target.

#### Monster Infighting

The line `target->target = source` in step 7 has an important emergent consequence. If a monster's projectile accidentally hits another monster (e.g., a fireball passes through a crowd), the hit monster's target is set to the firing monster. The hit monster then attacks back. This creates the "monster infighting" behavior that players exploit heavily — getting monsters to fight each other.

#### `P_KillMobj`

```
  P_KillMobj(source, target):
    1. Clear MF_SOLID, MF_SHOOTABLE, MF_FLOAT, MF_SKULLFLY flags
    2. Set MF_CORPSE, MF_DROPOFF (can fall off ledges now)
    3. target->height >>= 2  (flatten corpse for walking over)
    4. If source is player: score++ (kill count)
    5. Play death sound
    6. Set death state (or xdeath state if health < -info->spawnhealth)
    7. If MT_WOLFSS / MT_KEEN / MT_BOSSBRAIN: trigger special exit logic
    8. Drop item if info->droppeditem set (e.g., shotgun guy drops shotgun)
```

The split between normal death (`S_DEATH`) and extreme death (`S_XDEATH`) is based on whether health went deeply negative — gibbing requires health < -spawnhealth. Each monster type defines both sequences in `info.c`.

---

---

## Chapter 5: UI & HUD

The UI layer sits entirely above the simulation. It reads game state but never writes it (with one exception: cheat codes in `st_stuff.c` directly modify player state). All drawing goes into `screens[0]` — the 320×200 8-bit intermediate buffer — which is then blitted to the 1-bit framebuffer by `I_FinishUpdate`.

---

### 5.1 `v_video.c` — Patch Drawing

All UI drawing uses DOOM's `patch_t` format — the same post-based column format described in §3.7. `V_DrawPatch(x, y, scrn, patch)` draws a patch into one of the five `screens[]` buffers. The implementation iterates columns, skips transparent gaps (posts), and writes palette indices directly to the 8-bit buffer.

`V_DrawPatchDirect` draws to `screens[0]` and also marks the dirty region for the blit — on the SE/30, this is the primary draw target for all UI elements.

---

### 5.2 `m_menu.c` — Menu System

The menu system is a straightforward array-of-arrays structure. Each menu is a `menu_t`:

```c
typedef struct {
    short       numitems;
    menu_t     *prevMenu;         // ESC goes here
    menuitem_t *menuitems;        // array of items
    void       (*routine)(int);   // draw function
    short       x, y;             // screen position of first item
    short       lastOn;           // last selected item index
} menu_t;
```

Each `menuitem_t` has a status (selectable / title / blank), a name (lump name for the graphic), and an action function called when the item is activated. The active menu draws its items as patches via `V_DrawPatch`, with the animated skull cursor (`M_SKULL1`/`M_SKULL2`) drawn at the selected item's position. The skull alternates every `SKULLANIMCOUNT` (8) tics.

#### Screen Size and Detail Controls

`M_SizeDisplay` adjusts `screenblocks` (1-11), which controls how large the game view is. At `screenblocks=10` the status bar is visible; at 11 the view fills the screen. `R_ExecuteSetViewSize` is called on the next frame to apply the change. On the SE/30 with `opt_scale2x=1`, `M_SizeDisplay` caps at `screenblocks=8` (the 2x mode only supports that size).

`M_ChangeDetail` cycles `detailLevel` (0=HIGH, 1=LOW, 2=QUAD) and calls `I_MacBeep(detailLevel+1)` for audible feedback, then queues a view size change. This is the in-game control for the most impactful performance option.

---

### 5.3 `m_misc.c` — Configuration

`m_misc.c` maintains a `defaults[]` table — an array of `{name, location, defaultvalue}` entries mapping config file keys to global variable addresses:

```c
defaultdata_t defaults[] = {
    { "mouse_sensitivity", &mouseSensitivity,  5    },
    { "screenblocks",      &screenblocks,      8    },
    { "detaillevel",       &detailLevel,       2    },
    { "halfline",          &opt_halfline,      1    },
    { "fog_scale",         &fog_scale,         10240},
    { "dither_gamma_x100", &dither_gamma_x100, 60   },
    // ... ~40 entries total
};
```

`M_LoadDefaults` opens `doom.cfg`, reads it line by line, and for each `key value` pair finds the matching entry and writes the value to the pointed-to variable. `M_SaveDefaults` does the reverse — writes every entry in the table to a fresh `doom.cfg`. This means any new tunable parameter only needs one entry in `defaults[]` to gain full load/save/command-line support.

---

### 5.4 `hu_stuff.c` — Heads-Up Display

The HUD renders on top of the game view inside `screens[0]`. It draws into the view area, not the status bar. `R_RenderPlayerView` pre-clears the view area of `screens[0]` to zero before rendering — so the only non-zero pixels in the view area after rendering are HUD overlays. `I_FinishUpdate` exploits this: it scans `screens[0]` for non-zero view-area pixels and blits only those as a black mask overlay onto the direct-rendered 1-bit framebuffer (`blit8_black`). This keeps HUD text visible without re-rendering the entire view.

Key HUD elements:
- **Level title** (`w_title`): displayed for `HU_TITLETIMEOUT` tics at level start
- **Message widget** (`w_message`): centered screen message (pickups, cheats, etc.), expires after `HU_MSGTIMEOUT` tics
- **Chat** (`w_chat`): multiplayer chat input; irrelevant on SE/30 but architecture retained

`HU_Init` loads the `FONTA` font patches (ASCII characters 33-95 as 8×8 patches). `M_DrawText` in `m_misc.c` uses this same font for menu text strings.

---

### 5.5 `st_stuff.c` — Status Bar

The status bar occupies the bottom 32 rows of `screens[0]` (rows 168-199). It displays health, armor, ammo, equipped weapon indicator, face, keys, and frag count (deathmatch).

#### Widget System

The status bar uses a set of typed widget structs, each with a value pointer, patch array, and dirty flag:
- `st_number_t` — draws a number digit by digit using `STN*` patches
- `st_percent_t` — number + percent sign
- `st_multicon_t` — selects one of N patches based on an integer value (weapon indicator, key icons)
- `st_binicon_t` — shows/hides a patch based on a boolean

Each widget has a `oldvalue` field. `ST_diffDraw` only redraws a widget if its current value differs from `oldvalue` — avoiding unnecessary patch draws when nothing changed.

#### Face Animation

The player face (`STFST*` patches) is the most complex widget. It cycles through expressions based on game events:

```
  Face update (every TICRATE/2 = 17 tics):

  Priority (highest first):
    1. God mode active          → STFGOD0
    2. Health <= 0              → STFDEAD0
    3. Taking damage            → pain face (direction toward damage source)
    4. Picked up item           → evil grin (STFEVL*)
    5. Just fired               → rampage face (STFKILL*)
    6. Low health (<25)         → worried face
    7. Otherwise                → straight-ahead face (STFST*)

  5 pain levels × 8 face variants = 40 base faces
  Plus: god, dead, evil grin, rampage, ouch = 42 total patch entries
```

The "ouch" face triggers when the player takes more than 20 points of damage in a single tic.

#### Palette Flashing

`ST_doPaletteStuff` applies full-screen palette shifts for visual feedback. It calls `I_SetPalette` with a different palette from the WAD:
- Palettes 1-8 (`STARTREDPALS`): red tint, proportional to recent damage
- Palettes 9-12 (`STARTBONUSPALS`): gold tint, on item pickup
- Palette 13 (`RADIATIONPAL`): green tint, while wearing radiation suit

On the SE/30 monochrome display, palette changes rebuild `grayscale_pal` and `mono_colormaps` via `I_RebuildDitherPalette` — effectively changing the luminance mapping. The red-tint damage flash becomes a slightly darkened/brightened frame, which is subtle but present.

#### Cheat Codes — `ST_Responder`

`st_stuff.c` processes cheat codes using the cheat sequence matcher from `m_cheat.c`. `cht_CheckCheat` maintains a per-cheat position pointer and advances it on matching keystrokes. Non-letter keys (movement, arrows) are filtered out with `>= 128` — without this filter, WASD movement would reset cheat sequences every frame.

Active cheats: `iddqd` (god), `idkfa` (all weapons/ammo/keys), `idfa` (weapons/ammo, no keys), `idclip` (no-clip), `idbehold*` (power-ups), `idclev##` (warp to level), `idmypos` (display coordinates).

The `idclev` cheat had a SE/30-specific fix: the original code guarded against `epsd < 1`, but Doom II uses `epsd=0` for all maps. The guard was skipped for `gamemode == commercial`.

---

## Chapter 6: Sound System

Sound is architecturally present but **deactivated on the SE/30** for performance reasons. The sound subsystem as designed is documented here for completeness and as a reference for future activation.

---

### 6.1 `s_sound.c` — Channel Management

`S_Init` allocates an array of `channel_t` structs (`numChannels`, default 3 on SE/30 as set in `doom.cfg`). Each channel tracks: origin object, sfx ID, handle (passed to the I_Sound layer), volume, and separation (pan).

#### `S_StartSound`

```
  S_StartSound(origin_mobj, sfx_id):
    1. S_getChannel() — find free channel or steal the lowest-priority one
    2. S_AdjustSoundParams() — compute volume, separation, pitch from distance
    3. I_StartSound() — hand off to hardware layer (stub on SE/30)
    4. Record in channel: origin, sfxinfo, handle
```

`S_getChannel` prefers truly free channels. If none are free it evicts the channel playing the lowest-priority sound (priority is a per-sfx constant in `sounds.c`).

#### Distance Attenuation

`S_AdjustSoundParams` computes three values from the 2D distance between listener and source:

```
  dist = P_AproxDistance(source - listener)

  volume:  if dist > S_CLIPPING_DIST (1200 units): silent
           else: vol = snd_SfxVolume * (S_CLIPPING_DIST - dist) / S_CLIPPING_DIST

  sep:     angle from listener to source → pan left/right
           sep = 128 + (S_STEREO_SWING * sin(angle)) / FRACUNIT
           (128 = center; 0 = full left; 255 = full right)

  pitch:   base pitch ± S_PITCH_PERTURB random variation
```

#### `S_UpdateSounds`

Called once per frame from `D_DoomLoop`. For each active channel, it checks whether the origin object is still alive and in the same position; if the origin has moved, it recomputes volume/pan/pitch and calls `I_UpdateSoundParams`. This keeps 3D sound positioning continuous as both the listener and sources move.

### 6.2 `i_sound_mac.c` — Hardware Stub

All `I_*Sound*` functions are stubs that return immediately. The sound system was deactivated because early profiling showed it would consume a significant fraction of the already-tight frame budget. Re-activating it is a post-1.0 item — the architecture is fully in place; only `i_sound_mac.c` needs a real implementation using Mac OS SndNewChannel / SndDoImmediate / SoundManager calls.

---

## Chapter 7: Platform Layer (Mac-Specific)

The four `i_*_mac.c` files form a complete abstraction between Classic Mac OS and the DOOM engine. The engine calls standard `I_*` interfaces; the Mac-specific implementations are the only code that touches the Mac Toolbox.

---

### 7.1 `i_main_mac.c` — Entry Point and Application Lifecycle

#### Mac Toolbox Initialization

`main()` begins with the Classic Mac OS "fat init" sequence — a series of `InitXxx()` calls that must happen before any Toolbox use:

```c
MaxApplZone();          // expand application heap to max
MoreMasters();          // allocate extra master pointer blocks
InitGraf(&qd.thePort);  // initialize QuickDraw
InitFonts();
FlushEvents(everyEvent, 0);
InitWindows();
InitMenus();
TEInit();               // TextEdit
InitDialogs(nil);
InitCursor();
```

These calls are mandatory and order-sensitive. `InitGraf` must come first; everything else depends on QuickDraw being initialized.

#### Working Directory Setup

`SetCWDToAppFolder` uses the Process Manager to find the application's own location on disk:

```c
ProcessInfoRec info;
FSSpec appSpec;
GetProcessInformation(&process, &info);  // → info.processAppSpec = app FSSpec
SetDefaultDirToFolder(appSpec.vRefNum, appSpec.parID);
```

`SetDefaultDirToFolder` calls `PBHSetVolSync` to set the HFS working directory to the app's folder. This ensures `fopen("doom.cfg")` and `access("doom1.wad")` find files relative to the app, not wherever the Finder's working directory happens to be.

#### Exit Architecture — `setjmp`/`longjmp`

DOOM's error and quit paths need to unwind the call stack cleanly and return control to `main()` for orderly shutdown. Classic Mac OS does not have exceptions. The solution:

```c
// In main():
jmp_buf doom_quit_jmp;
if (setjmp(doom_quit_jmp) == 0)
    D_DoomMain();
// Land here after I_Quit or I_Error:
DisposeWindow(bg_window);
ExitToShell();

// In i_system_mac.c:
void I_Quit(void)  { longjmp(doom_quit_jmp, 1); }
void I_Error(...)  { longjmp(doom_quit_jmp, 2); }
```

`ExitToShell()` is called from exactly one place — `main()` — making the shutdown path deterministic. All cleanup (window disposal, config save, sound shutdown) happens in the correct order regardless of how the quit was triggered.

#### Splash Screen and Settings Dialog

The splash dialog (DLOG 128) shows an animated PICT sequence while the user reads the splash screen. Animation runs inside `SplashFilterProc`, a `ModalFilterUPP` passed to `ModalDialog`. On each `nullEvent` the filter advances the PICT frame; `StdFilterProc` (obtained via `GetStdFilterProc`) handles the default button ring and Return/Enter keypress. Escape calls `ExitToShell` directly from the filter.

The settings dialog (DLOG 129) uses `GetNewDialog`/`DisposeDialog` — created fresh each open, destroyed on close. Checkboxes (items 15-19) toggle boolean opt flags; popup menus (items 29-30) select shade level and detail level. `populate_settings` reads current globals into widgets; `read_settings` writes widgets back to globals; `M_SaveDefaults` persists to `doom.cfg`.

---

### 7.2 `i_video_mac.c` — Framebuffer and Blit Layer

This is the largest and most SE/30-specific file. It owns everything between DOOM's 8-bit `screens[0]` and the physical 512×342 1-bit display.

#### Physical Framebuffer Layout

```
  Mac SE/30 screen: 512×342 pixels, 1-bit, rowbytes=64
  Game view: 320×200, centered:
    xoff = (512 - 320) / 2 = 96px = 12 bytes from left edge
    yoff = (342 - 200) / 2 = 71px from top edge

  Memory layout of one row (64 bytes):
  [12 bytes border][40 bytes game view][12 bytes border]
   ← xoff ──────────────────────────────────────────────→
```

#### Palette → Grayscale Conversion

DOOM's 8-bit palette contains 256 RGB triplets. `I_SetPalette` is called whenever the game changes palette (damage flash, pickup flash, menu transitions). It converts each palette entry to grayscale using BT.601 luminance weights:

```c
gray = (r * dither_r_wt + g * dither_g_wt + b * dither_b_wt) / 256;
// default weights: r=121, g=104, b=25  (sum ≈ 250)
```

After weighting, gamma correction is applied via `gamma_curve[]` (pre-computed by `I_BuildGammaCurve` using pure-integer Newton-Raphson square root — no `libm` calls, which would pull in the FPU-dependent math library). The result is stored in `grayscale_pal[256]`.

`I_BuildMonoColormaps` then composes all 34 DOOM colormaps with `grayscale_pal` into `mono_colormaps[34*256]`, a single lookup table used by direct 1-bit column drawers.

#### `I_FinishUpdate` — The Blit State Machine

**Figure 8 — I_FinishUpdate Blit State Machine:**

```
  I_FinishUpdate() called each frame
       │
       ├─ is_direct = (gamestate==GS_LEVEL && !menuactive && !automapactive && !wipegamestate)
       │
       ├─[is_direct]──────────────────────────────────────────────────────────────────┐
       │                                                                               │
       │  View area already 1-bit rendered by R_DrawColumn_Mono etc.                  │
       │  Only need to handle border + status bar + HUD overlay                       │
       │                                                                               │
       │  ├─[do_border]── blit8() border from screens[0] → fb_offscreen_buf           │
       │  ├─[do_sbar]──── blit8_sbar_thresh() status bar (rows 168-199)               │
       │  └─[hu_overlay]─ blit8_black() HU non-zero pixels as black mask over view   │
       │                                                                               │
       └─[!is_direct]─────────────────────────────────────────────────────────────────┤
                                                                                       │
         ├─[menuactive]──                                                              │
         │  First menu frame: snapshot fb_offscreen_buf → menu_bg_1bit                │
         │  Later frames: restore menu_bg_1bit → fb_offscreen_buf (frozen game bg)    │
         │                                                                             │
         └─[wipe/title/intermission]──                                                 │
            blit8_sbar_thresh() full screens[0] → fb_offscreen_buf                   │
                                                                                       │
       ├─[menuactive]── blit8_menu() menu_overlay_buf → fb_offscreen_buf (threshold)  │
       │                                                                               │
       └─ FLIP ────────────────────────────────────────────────────────────────────────┘
          memcpy fb_offscreen_buf → real framebuffer
          (200 rows × 64 bytes = 12,800 bytes per frame)
```

The `is_direct` fast path is critical for performance. During gameplay, the view area is already 1-bit rendered pixel-by-pixel by the column drawers — `I_FinishUpdate` does not need to blit it. Only the border, status bar, and HUD text overlays require blit work. On most frames with no HUD activity, the blit cost is just the status bar (40 bytes × 32 rows) plus the memcpy flip.

#### Double-Buffering

`fb_offscreen_buf` is a 512×342/8 = 21,888 byte buffer allocated at startup. All rendering targets this buffer. At the end of `I_FinishUpdate`, the game area (200 rows × 64 bytes) is `memcpy`'d to the real framebuffer at once. This eliminates tearing — the display always shows a complete frame.

`real_fb_base` stores the physical framebuffer address obtained from `qd.screenBits.baseAddr`. `fb_mono_base` is the pointer used by all renderers; it is redirected to `fb_offscreen_buf` (or `fb_source_buf` in 2x mode) so renderers never write directly to the display.

---

### 7.3 `i_input_mac.c` — Input Polling

Classic Mac OS input normally flows through `WaitNextEvent` / `GetNextEvent`. Using these in the game loop would drain events intended for menu dialogs and introduce latency. Instead, `I_PollMacInput` uses `GetKeys(KeyMap)` — a direct keyboard state snapshot that bypasses the event queue entirely:

```c
void I_PollMacInput(void)
{
    KeyMap keys;
    GetKeys(keys);   // 128-bit snapshot of all key states

    for (each entry in kKeyTable)
    {
        int macKey = kKeyTable[i].macKey;
        int isDown = (keys[macKey>>5] >> (macKey&31)) & 1;

        if (isDown && !prevKeys[macKey]) {
            // Key just pressed → post EV_KEYDOWN
            D_PostEvent(&ev);
        }
        if (!isDown && prevKeys[macKey]) {
            // Key just released → post EV_KEYUP
            D_PostEvent(&ev);
        }
    }

    // Check dither hotkeys before Doom sees them
    for (each entry in kDitherKeys)
        if (justPressed(entry.macKey))
            I_AdjustDither(entry.param, entry.delta);

    // Drain OS event queue for mouse buttons only
    while (GetOSEvent(mDownMask|mUpMask, &evt)) { ... }
}
```

`kKeyTable` maps Mac keycodes (hardware scan codes, not ASCII) to DOOM `KEY_*` constants. WASD have dual entries — both their movement key meaning and their ASCII meaning (for cheat code input). `kDitherKeys` intercepts dither-tuning keys before they reach the game's event queue.

---

### 7.4 `i_system_mac.c` — System Services

#### Timing

`I_GetTime` converts `TickCount()` (60 Hz Mac tick counter) to DOOM tics (35 Hz):

```c
int I_GetTime(void)
{
    long now = TickCount();
    return (int)((now - basetime) * TICRATE / 60);
}
```

`basetime` is recorded at startup. The 60→35 conversion is integer arithmetic — no division per call, just a multiply by 35 and divide by 60 (or equivalently, `* 7 / 12`).

#### Memory Allocation

`I_ZoneBase` tries increasingly smaller allocations until one succeeds:

```c
byte *I_ZoneBase(int *size)
{
    static int sizes[] = { 48*1024*1024, 32*1024*1024, 16*1024*1024,
                            8*1024*1024,  4*1024*1024,  2*1024*1024, 0 };
    for (int i = 0; sizes[i]; i++) {
        byte *p = (byte*)NewPtr(sizes[i]);
        if (p) { *size = sizes[i]; return p; }
    }
    I_Error("Not enough memory");
}
```

On an 8 MB SE/30 with System 7 occupying ~2 MB, the allocator typically gets 4-6 MB for the zone.

#### Logging

`doom_log(fmt, ...)` is the primary debugging tool. It writes to `doom_log.txt` in the app folder using HFS file APIs directly (not `fopen`/`fprintf`, which had reliability issues with Retro68's working directory handling). The file is opened with `FSpCreate` + `FSpOpenDF`; each write uses `FSWrite`. Line endings are `\r` (Classic Mac OS convention). The entire logging system is compiled out in release builds (`#ifndef DOOM_RELEASE_BUILD`).

---

# Part II — SE/30 Port Optimizations

The SE/30 runs at 16 MHz. Unmodified DOOM on a 33 MHz 386 ran at ~15-20 FPS on default settings. Simply getting the code to compile and run on the SE/30 produced approximately 1-2 FPS — unplayable. The optimizations below, applied cumulatively, brought the game to 6-10 FPS in typical gameplay, which is the practical ceiling given the hardware.

Each optimization entry follows the same structure: **why it was a candidate**, **why it should be faster on the 68030 specifically**, and **how it was implemented**.

The profiling tool throughout was `doom_log` combined with the `prof_*` tick accumulators in `d_main.c`, producing per-subsystem breakdowns every 35 tics.

---

### Opt 1: Compiler Flags — `-O3 -fomit-frame-pointer`

**Candidate:** GCC's default optimization level (`-O0`) produces straightforward, unoptimized code. `-O3` enables aggressive inlining, constant propagation, loop unrolling, and better register allocation — all of which reduce instruction count in tight loops.

**68030 specifics:** The 68030 has 8 data registers (d0-d7) and 8 address registers (a0-a7). Without `-fomit-frame-pointer`, register `a6` is reserved as the frame pointer, leaving only 7 address registers for the compiler. In loops that manipulate several pointers simultaneously — like the column drawers, which track a source texture pointer, a destination framebuffer pointer, and a colormap pointer — losing one register forces a spill to the stack. Each spill is a memory read/write that hits the 16-bit data bus. `-fomit-frame-pointer` frees `a6` for use as a general pointer register, eliminating those spills.

**Caveat — `d_main.c` exemption:** A known GCC 68k bug causes stack frame size miscalculation with `-O3` and `-fomit-frame-pointer` when a function has many large local variables. `IdentifyVersion` in `d_main.c` has exactly this pattern (multiple heap-allocated path strings, large local arrays). The crash it caused was silent — stack corruption overwriting adjacent locals. Fix: `d_main.c` is compiled at `-O2 -fno-omit-frame-pointer`:

```cmake
set_source_files_properties(d_main.c PROPERTIES
    COMPILE_FLAGS "-O2 -fno-omit-frame-pointer")
```

**Result:** Broad baseline improvement across all subsystems. Not measurable in isolation (applied from the start), but estimated 30-50% overall vs `-O0`.

---

### Opt 2: `-funroll-loops` Selective Application

**Candidate:** Loop unrolling eliminates branch instructions from tight inner loops. The column and span drawers loop over every pixel — any overhead per iteration (loop counter decrement, branch, jump-back) is paid thousands of times per frame.

**68030 specifics:** The 68030 has a 256-byte instruction cache. This is very small. A loop that is unrolled from 4 instructions to 40 instructions may no longer fit in the cache — on the next iteration, the CPU must fetch instructions from main memory across the 16-bit data bus, incurring wait states. Early testing applied `-funroll-loops` globally and observed a *regression* on `I_FinishUpdate` — the blit loop (already 8 pixels at a time) when unrolled blew the instruction cache, causing the blit to run slower than the unoptimized version.

**Implementation:** Restricted to the three files whose inner loops are tight enough to stay in cache when unrolled:

```cmake
set_source_files_properties(r_draw.c r_segs.c r_bsp.c PROPERTIES
    COMPILE_FLAGS "-funroll-loops")
```

`r_draw.c`: column/span drawers — inner loop is ~8-12 instructions; unrolled 4x stays within cache.
`r_segs.c`: `R_RenderSegLoop` — per-column computation, similar size.
`r_bsp.c`: BSP node check loop — short enough to benefit.

`i_video_mac.c` deliberately excluded: the 8-pixel blit loop was already at the cache limit.

**Result:** Modest gain in column/span draw time; no blit regression.

---

### Opt 3: `FixedMul` Inlined Macro

**Candidate:** `FixedMul` is the single most-called function in the renderer. Every texture coordinate step, every projection computation, every distance calculation goes through it. In the original code it was a C function — each call incurred a full 68k function call overhead.

**68030 specifics:** A 68k function call requires: `MOVE.L` the arguments onto the stack, `JSR` (4 cycles + bus fetch), the callee saves its registers, does its work, restores registers, and `RTS` (10 cycles). For a two-argument function called inside a loop that runs 100-200 times per column, per column every frame, this overhead accumulates to millions of wasted cycles per second. The actual work of `FixedMul` is a 32×32 multiply and a 16-bit shift — 5-10 cycles of real work drowned in 20+ cycles of call overhead.

Inlining via macro eliminates the call entirely. GCC can then register-allocate the intermediate `long long` result and fold the shift into adjacent operations.

```c
// Before: function call
fixed_t FixedMul(fixed_t a, fixed_t b) {
    return (fixed_t)(((long long)a * b) >> FRACBITS);
}

// After: macro — zero call overhead, inlined at every use site
#define FixedMul(a,b) ((fixed_t)(((long long)(a) * (b)) >> FRACBITS))
```

**Result:** One of the highest-impact single changes. Measurable improvement in `prof_r_segs` and `prof_r_masked`.

---

### Opt 4: `FixedDiv2` Using `long long` — FPU Removal

**Candidate:** The original `FixedDiv` used the 68882 FPU (`FDIV` instruction) to compute fixed-point division. Division is used for wall scale, sprite scale, and iscale computation.

**68030 specifics:** The 68030+68882 combination does have hardware floating-point, but FPU instructions carry hidden costs in this use case: the compiler must save/restore the FPU state on context (though cooperative multitasking makes this less frequent), FPU register loading has latency, and `FDIV` itself takes 28-40 cycles on the 68882. For small integer divisions that fit in 32 bits, the integer `DIVS.L` (signed 32÷32) takes ~40 cycles on the 68030 — comparable — but without FPU state overhead. For the `long long` shift approach used here, GCC generates a 64-bit left shift + integer divide, which avoids FPU entirely.

```c
// Before: FPU path (compiler generates FMOVEM + FLD + FDIV + FST)
fixed_t FixedDiv2(fixed_t a, fixed_t b) {
    return (fixed_t)((double)a / (double)b * FRACUNIT);
}

// After: pure integer
fixed_t FixedDiv2(fixed_t a, fixed_t b) {
    return (fixed_t)(((long long)a << FRACBITS) / b);
}
```

**Result:** Eliminated all FPU usage from the rendering hot path. Removes dependency on FPU state management and avoids FPU instruction latency.

---

### Opt 5: Direct 1-bit Rendering

**Candidate:** The original DOOM rendering pipeline always produces 8-bit output in `screens[0]`. For a monochrome display, every frame requires a full conversion pass: 320×200 = 64,000 bytes read, grayscale-converted, Bayer-dithered, and packed to 40 bytes × 200 rows = 8,000 bytes of 1-bit output. This conversion runs after rendering completes — it is pure overhead layered on top of the already-expensive render.

**68030 specifics:** The 64,000-byte `screens[0]` buffer does not fit in any cache (there is none on the 68030 aside from the 256-byte instruction cache). Every byte of the conversion reads from main memory over the 16-bit data bus. The 8,000-byte output also goes to main memory. At ~1 MB/s effective bandwidth, this conversion alone costs ~64ms — more than a full frame budget at 10 FPS.

**Implementation:** New column and span drawers in `r_draw.c` write directly to `fb_mono_base` during the render pass. The Bayer dither and grayscale lookup happen inline per pixel, at the time the pixel is computed — when the texture data is already in registers. The full-frame conversion pass is eliminated for the game view.

Key supporting infrastructure:
- `grayscale_pal[256]`: palette index → 0-255 gray (built by `I_RebuildDitherPalette`)
- `mono_colormaps[34*256]`: colormap + grayscale in one lookup (built by `I_BuildMonoColormaps`)
- `bayer4x4[4][4]`: Bayer threshold matrix
- `fb_mono_base`, `fb_mono_rowbytes`, `fb_mono_xoff`, `fb_mono_yoff`: framebuffer geometry, non-static so `r_draw.c` can access them

Function pointer dispatch (§3.2) switches between direct-render drawers (gameplay) and 8-bit drawers (menus, wipes, intermission) transparently.

**Figure 12 — Direct 1-bit Render Path vs Original:**

```
  ORIGINAL PATH (every frame):
  ┌──────────────┐   colfunc    ┌──────────────┐  blit loop   ┌─────────────┐
  │ Texture data │ ──────────▶  │  screens[0]  │ ──────────▶  │  1-bit FB   │
  │ (WAD, 8-bit) │  per pixel   │  8-bit buf   │  64K reads   │  (display)  │
  └──────────────┘              │  320×200 B   │  8K writes   └─────────────┘
                                └──────────────┘
                                   ↑ full frame conversion every frame

  SE/30 DIRECT PATH (gameplay):
  ┌──────────────┐   colfunc                    ┌─────────────┐
  │ Texture data │ ──────────────────────────▶  │ fb_offscreen│
  │ (WAD, 8-bit) │  per pixel, dither inline    │  1-bit buf  │
  └──────────────┘                              └──────┬──────┘
                                                       │ memcpy flip
                                                       ▼
                                               ┌─────────────┐
                                               │  1-bit FB   │
                                               │  (display)  │
                                               └─────────────┘
  screens[0] still written for HUD/status bar — but only those regions
  are blitted (not the full 64K view area).
```

**Result:** Largest single FPS gain in the project. Eliminated ~64ms of post-render work per frame.

---

### Opt 6: `I_FinishUpdate` 8-pixels-per-byte Blit

**Candidate:** Even with direct rendering for the view, the status bar and border still need to be blitted from `screens[0]` to the 1-bit framebuffer. The naive approach reads one byte, compares it to a threshold, and sets or clears one bit — 8 memory reads and 8 bit-operations per output byte.

**68030 specifics:** The 68030 data bus is 32 bits wide but slow. Minimizing memory transactions is key. 8 bytes-in-1-byte-out can be implemented with 8 branch-free bit-tests and OR operations into a single register, then one byte write — reducing memory transactions from 9 (8 reads + 1 write) to 2 effective writes (the read side stays in register pipeline).

**Implementation:**

```c
static inline unsigned char blit8(const byte *src)
{
    unsigned char out = 0;
    // BT.601-weighted grayscale threshold, 8 pixels → 1 byte
    // Mac 1-bit: 0=white, 1=black → invert sense
    if (sbar_gray[src[0]] < 128) out |= 0x80;
    if (sbar_gray[src[1]] < 128) out |= 0x40;
    // ... src[2..7] with bits 0x20..0x01
    return out;
}

// Status bar blit (40 bytes per row, 32 rows):
for (y = 0; y < SBARHEIGHT; y++) {
    const byte *sr  = src + (168 + y) * SCREENWIDTH;
    unsigned char *dst = fb_offscreen_buf + ...;
    for (x = 0; x < SCREENWIDTH; x += 8)
        *dst++ = blit8_sbar_thresh(sr + x);
}
```

`blit8_sbar_thresh` uses `sbar_gray[]` (raw BT.601, no game contrast stretch) for status bar accuracy. `blit8_black` (used for HUD overlay) maps any non-zero pixel to black, regardless of luminance — correct for text overlay. `blit8_menu` (used for menu overlay) adds a luminance threshold: only pixels brighter than `menu_thresh=88` are drawn, preventing the dark brownish menu background from rendering as solid black.

**Result:** Status bar blit is now 8× fewer memory writes. Combined with the direct-render path, `I_FinishUpdate` is reduced to: sbar blit (40B × 32 rows = 1280 byte reads) + memcpy flip (12,800 bytes) per frame.

---

### Opt 7: Double-Buffering

**Candidate:** Without double-buffering, the renderer writes directly to the display framebuffer. Since the Mac SE/30 refreshes its display at 60 Hz and the game renders at 6-10 Hz, the display will catch the framebuffer mid-write on nearly every frame — producing visible horizontal tearing.

**68030 specifics:** The off-screen buffer (`fb_offscreen_buf`, ~22 KB) fits in physical RAM and is accessed sequentially by the `memcpy` flip — a sequential memory access pattern is optimal for the 16-bit data bus. The 12,800-byte flip (200 rows × 64 bytes) takes approximately 12ms at ~1 MB/s, which is acceptable overhead given the tearing elimination.

**Implementation:**
```c
// At startup (I_InitGraphics):
real_fb_base  = (byte*)qd.screenBits.baseAddr;
fb_offscreen_buf = malloc(s_phys_rbytes * s_phys_height);  // 22KB
fb_mono_base  = fb_offscreen_buf;   // renderers write here

// At end of I_FinishUpdate:
byte *src = fb_offscreen_buf + s_phys_yoff * s_phys_rbytes;
byte *dst = real_fb_base      + s_phys_yoff * s_phys_rbytes;
for (int r = 0; r < 200; r++) {
    memcpy(dst, src, 64);
    src += s_phys_rbytes;
    dst += s_phys_rbytes;
}
```

**Result:** Eliminated tearing entirely. Also enabled the menu background freeze optimization (snapshot 1-bit buffer on first menu frame, restore on subsequent frames — avoiding re-blit of the frozen game scene every menu frame).

---

### Opt 8: Half-Line Rendering (`opt_halfline`)

**Candidate:** The column drawers iterate from `dc_yl` to `dc_yh` — the full vertical extent of each wall column. Halving the number of rows rendered halves the column-draw iteration count.

**68030 specifics:** Column draw is the single largest component of render time (30-40% of frame budget at QUAD detail). The inner loop is bounded by row count. Skipping every odd row and copying from the even row above is a sequential memory operation — fast on the 68030's linear memory model.

**Implementation:** Column heights are snapped to even: `dc_yh &= ~1`. After `R_RenderPlayerView`, a scanline-doubling pass copies each even row to the row below:

```c
// Scanline doubling pass (after R_RenderPlayerView):
for (int y = dc_yl_min; y < dc_yh_max; y += 2) {
    byte *even = fb_mono_base + y * fb_mono_rowbytes + xoff_byte;
    byte *odd  = even + fb_mono_rowbytes;
    memcpy(odd, even, col_bytes);
}
```

`opt_halfline=1` is the default in `doom.cfg`. It was originally off by default because the scanline-doubling pass on the raw framebuffer (without double-buffering) caused a white-flash artifact — the odd rows would be briefly visible as white between the clear and the copy. The double-buffering (Opt 7) eliminated this by keeping the copy in the off-screen buffer.

**Figure 14 — Half-line Rendering:**

```
  Full render (opt_halfline=0):     Half-line (opt_halfline=1):

  Row 0:  [rendered]                Row 0:  [rendered]
  Row 1:  [rendered]                Row 1:  [copied from row 0]
  Row 2:  [rendered]                Row 2:  [rendered]
  Row 3:  [rendered]                Row 3:  [copied from row 2]
  Row 4:  [rendered]                Row 4:  [rendered]
  Row 5:  [rendered]                Row 5:  [copied from row 4]
  ...                               ...

  Column draw iterations: N         Column draw iterations: N/2
  Scanline copy cost: 0             Scanline copy cost: ~40 memcpy/frame
  Visual quality: full              Visual quality: slight horizontal banding
                                    (visible on fine horizontal texture detail)
```

**Result:** ~40% reduction in column-draw time. Minor horizontal banding artifact visible on close surfaces with fine detail.

---

### Opt 9: Affine Texture Stepping (`opt_affine_texcol`)

**Candidate:** In perspective-correct texture mapping, the texture column index for each screen column of a wall is:

```
  texturecolumn = rw_offset - FixedMul(xtoviewangle[x], rw_distance)
```

This requires a `FixedMul` per column — a 64-bit multiply per column, called for every visible column of every wall every frame.

**68030 specifics:** Even inlined (Opt 3), `FixedMul` involves a 32×32→64 multiply and a 16-bit shift. On the 68030, `MULS.L` (32×32→32 result) takes 27 cycles; the 64-bit `long long` version requires two multiplies and a shift — roughly 60-80 cycles. For a 320-pixel-wide wall, this is 320 × ~70 = 22,400 cycles, about 1.4ms at 16 MHz. Replacing it with a linear step (one ADD per column) costs ~4 cycles each.

**Implementation:** `rw_texstep` is computed once per wall segment in `R_StoreWallRange`:

```c
// Compute texture column at both endpoints, step linearly:
fixed_t texcol_left  = rw_offset - FixedMul(xtoviewangle[start], rw_distance);
fixed_t texcol_right = rw_offset - FixedMul(xtoviewangle[stop],  rw_distance);
rw_texstep = (texcol_right - texcol_left) / (stop - start);
rw_texcol  = texcol_left;

// In R_RenderSegLoop:
dc_texturemid = ...; // from rw_texcol
rw_texcol += rw_texstep;   // one ADD per column
```

This is **affine** texture mapping — it linearly interpolates the texture coordinate across the screen, ignoring the perspective warp. On a high-resolution display it produces noticeable texture swimming on angled walls. On the 512×342 SE/30 display at 4-pixel QUAD resolution, the artifact is minimal.

**Result:** ~150 cycles saved per column in `R_RenderSegLoop`. Measurable improvement in `prof_r_segs`.

---

### Opt 10: `iscale` Linear Interpolation

**Candidate:** `dc_iscale` — the texture DDA step per screen pixel — is the reciprocal of `rw_scale`. In the original code, it was recomputed per column via integer division.

**68030 specifics:** Integer division (`DIVS.L`, 32÷32) takes approximately 40 cycles on the 68030. Like `FixedMul`, this is called for every column of every wall.

**Implementation:** `iscale` is computed at both wall endpoints and linearly interpolated:

```c
// In R_StoreWallRange:
fixed_t iscale_left  = FixedDiv(FRACUNIT, rw_scale_left);
fixed_t iscale_right = FixedDiv(FRACUNIT, rw_scale_right);
rw_iscalestep = (iscale_right - iscale_left) / (stop - start);
rw_iscale     = iscale_left;

// In R_RenderSegLoop:
dc_iscale  = rw_iscale;
rw_iscale += rw_iscalestep;   // one ADD per column
```

The interpolation is slightly inaccurate (scale is a hyperbolic function of screen position, not linear), but the error is imperceptible at SE/30 resolution.

**Result:** Eliminates per-column integer division entirely. Always active (not a config option). Saves ~40 cycles/column.

---

### Opt 11: Detail Level System (HIGH / LOW / QUAD)

**Candidate:** The number of columns drawn per frame is directly proportional to the view width. Rendering fewer, wider columns reduces colfunc invocations proportionally.

**68030 specifics:** Each colfunc call carries setup overhead (load registers, compute starting address, set up loop) regardless of how tall the column is. Doubling column width halves the number of calls and thus halves this overhead. Additionally, wider columns write more bits per RMW cycle — the QUAD drawer writes a full nibble (4 bits) per output byte without sub-byte masking, which is significantly faster than single-bit manipulation.

**Implementation:** `detailshift` controls column width:

| detailshift | Name | Column width | Drawer |
|---|---|---|---|
| 0 | HIGH | 1 pixel | `R_DrawColumn_Mono` |
| 1 | LOW  | 2 pixels | `R_DrawColumnLow_Mono` |
| 2 | QUAD | 4 pixels | `R_DrawColumnQuadLow_Mono` |
| 3 | MUSH | 8 pixels | `R_DrawColumnMushLow_Mono` (see Opt 15) |

QUAD is the default (`detailLevel=2` in `doom.cfg`). The QUAD drawer writes a nibble (4 bits) per row, using a pre-computed `quad_nibble[256]` LUT:

```c
// QUAD: 4 pixels = 1 nibble. No sub-nibble RMW needed.
byte pat  = (gray > bayer_thresh) ? 0x00 : 0x0F;  // all 4 black or all 4 white
byte mask = (dc_x & 4) ? 0x0F : 0xF0;
*dest = (*dest & ~mask) | (pat & mask);
```

At HIGH detail, each pixel requires a read-modify-write on a single bit — 3 operations per pixel. At QUAD, it is one read-modify-write per 4 pixels — 0.75 operations per pixel. This is a 4× reduction in memory transactions for the column draw inner loop.

**Result:** QUAD vs HIGH: ~3× FPS improvement. QUAD is the recommended and default setting.

---

### Opt 12: `opt_solidfloor` — Skip Flat Texture Rendering

**Candidate:** Floor and ceiling rendering (`R_DrawPlanes`) is the second-largest render cost after wall columns. Each floor/ceiling span requires affine texture coordinate computation (two `FixedMul` calls per pixel), flat texture lookup, and a full-width horizontal write. Replacing this with a solid fill eliminates all of it.

**68030 specifics:** The flat texture stepping math (`ds_xstep`, `ds_ystep`, `ds_xfrac`, `ds_yfrac` incremented per pixel) involves two `FixedMul` calls per pixel — matching the cost of wall column rendering but for every floor pixel, which often outnumbers wall pixels on open maps. Solid fill requires only a comparison against a threshold value per byte, which is handled in `I_FinishUpdate` at blit time (the floor regions were not directly rendered and remain white from the framebuffer clear — they just get the `solidfloor_gray` pattern applied during the blit pass).

**Implementation:** When `opt_solidfloor=1`:
- `R_DrawPlanes` skips all `R_MakeSpans`/`spanfunc` calls for non-sky planes
- The floor/ceiling areas of `fb_offscreen_buf` retain whatever background pattern was there from the buffer initialization (or the previous frame via double-buffering) — which the user tunes to an appropriate gray via `solidfloor_gray`
- Sky ceilings are exempted (checked by `picnum == skyflatnum`)

**Result:** Eliminates the entire floor/ceiling rendering workload. On open maps with large floor/ceiling areas, this is a 30-50% FPS improvement.

---

### Opt 13: Distance Fog System (`fog_scale`)

**Candidate:** Far geometry contributes very few visible pixels — a wall segment 200 units away at QUAD detail occupies perhaps 2 columns on screen. Yet the BSP traversal visits it, `R_StoreWallRange` processes it, and `R_RenderSegLoop` iterates its columns. Skipping far geometry saves work proportional to scene complexity.

**68030 specifics:** BSP traversal cost scales with level geometry complexity, independent of what is visible. On large open maps (e.g., E1M7, the computer station), hundreds of wall segments may be processed even though most are tiny distant slivers. Fog culling prunes these in `R_RenderSegLoop` after the scale is computed — no colfunc call, just a branch.

**Implementation:**

```c
// In R_RenderSegLoop:
boolean in_fog = (fog_scale > 0 && rw_scale < fog_scale);

// Wall: skip colfunc but still update clip arrays (occlusion must be maintained)
if (!in_fog) {
    dc_source = texture_column;
    colfunc();
}
// ceilingclip/floorclip updated regardless — fog doesn't remove occlusion

// Sprite culling (R_ProjectSprite):
if (fog_scale > 0 && (xscale << detailshift) < fog_scale) {
    // Skip unless important object
    if (!(thing->flags & (MF_COUNTKILL | MF_SPECIAL | MF_MISSILE)))
        return;  // cull decorative sprite
}
```

**Figure 15 — Fog Culling Decision Tree:**

```
  For each wall column:
  ┌─ fog_scale == 0? ──YES──▶ draw (fog disabled)
  │
  └─NO
     └─ rw_scale < fog_scale? ──NO──▶ draw (close enough)
        │
        YES
        └─ skip colfunc (fog zone)
           still update ceilingclip/floorclip ← important! occlusion maintained

  For each sprite:
  ┌─ fog_scale == 0? ──YES──▶ project (fog disabled)
  │
  └─NO
     └─ xscale < fog_scale? ──NO──▶ project (close enough)
        │
        YES
        └─ MF_COUNTKILL or MF_SPECIAL or MF_MISSILE?
           ├─YES──▶ project (enemies/pickups/projectiles always visible)
           └─NO───▶ cull (decorations, corpses, torches culled)
```

Sky fog required special handling: when a fog-zone column would have had a sky ceiling, the sky must still be drawn. `r_segs.c` records fog-zone sky column ranges in `sky_fog_top[]`/`sky_fog_bot[]`; `R_DrawPlanes` contains a fallback pass that re-renders sky for those columns after the main plane pass.

**Result:** On fogged maps at the default `fog_scale=10240`, approximately 30-40% of wall draw calls and 20-30% of sprite projections are eliminated on typical open maps.

---

### Opt 14: 2× Pixel Scale Mode (`opt_scale2x`)

**Candidate:** Render the game at half resolution (160×100) and scale up 2× to the display (320×200 game coordinates → displayed as 320×200 pixels). This halves all column-draw work by halving both the width and height of the render target.

**68030 specifics:** Column and span draw time is linear in the number of pixels written. Rendering at half resolution means each column is half as tall and there are half as many columns — approximately 4× fewer column-draw pixels total. The expansion step (each source pixel → 2×2 destination pixels) is handled by a LUT (`expand2x_lut[256]`, mapping each source byte to a `uint16_t` with each bit doubled) — a purely sequential memory operation that is cache-friendly.

**Implementation:**

```c
// At startup (when opt_scale2x=1):
fb_source_buf     = static 4096-byte buffer   // 160×100 / 8 = 2000 bytes
fb_mono_base      = fb_source_buf             // renderers write at half size
fb_mono_rowbytes  = 20                        // 160px / 8 bits = 20 bytes/row

// expand2x_lut: each byte maps to uint16_t with each bit doubled
// e.g., 0b10110100 → 0b1100111111000000
for (int i = 0; i < 256; i++) {
    uint16_t out = 0;
    for (int b = 0; b < 8; b++)
        if (i & (1<<b)) out |= (3 << (b*2));
    expand2x_lut[i] = out;  // big-endian: correct bit order
}

// expand2x_blit: source row → two dest rows, each byte → two dest bytes
void expand2x_blit(...) {
    for each source row r:
        for each source byte c:
            uint16_t ex = expand2x_lut[src[c]];
            dest_row0[c*2]   = ex >> 8;
            dest_row0[c*2+1] = ex & 0xFF;
        memcpy(dest_row1, dest_row0, dest_rbytes);  // duplicate row
}
```

**Result:** Significant FPS increase at the cost of visible pixelation. At 512×342 resolution with a small CRT, 2× pixels are 2×2 pixel blocks — noticeable but acceptable given the hardware era aesthetic. Default off (`scale2x=0`); recommended for slower maps or lower-spec setups.

---

### Opt 15 *(failed)*: MUSH Detail Mode (`detailLevel=3`)

**Candidate:** Extending the detail level system to 8-pixel-wide columns (`detailshift=3`) would further halve the QUAD column count. 8-pixel columns also have the property that each column maps to exactly one full byte in the 1-bit framebuffer — no nibble read-modify-write at all.

**68030 specifics:** At 8 pixels per column and 320 pixels per row, only 40 columns are drawn per row — matching the byte width of the 1-bit framebuffer. Each column drawer could write directly with `MOVE.B` — no masking, no OR, just a direct byte store. This was the theoretical appeal.

**Implementation:** `mush_byte[4][256]` LUT maps a palette index + Bayer row phase (0-3) to the full byte pattern for an 8-pixel-wide column. `R_DrawColumnMushLow_Mono` and `R_DrawSpanMushLow_Mono` use this LUT. Accessible via 4 detail-change beeps (HIGH→LOW→QUAD→MUSH→HIGH cycle).

**Failure:** Mean FPS in MUSH mode: ~7.3 FPS vs QUAD ~6-7 FPS — only ~1 FPS gain for a catastrophic visual quality loss. At 40 columns, individual scene elements (monsters, doorways) are too wide to be recognizable. More fundamentally, the bottleneck at QUAD detail is **BSP traversal and the blit**, not column draw. MUSH mode reduced column-draw cost to near-zero, but the blit and BSP costs didn't change — demonstrating that Amdahl's law limits any further column-draw optimization. Code retained in the codebase but `detailLevel=3` is not recommended for normal use.

---

### Opt 16 *(not pursued)*: Pre-Processing WAD on Modern Machine

**Candidate:** The 10-15 second startup time is dominated by `R_InitData` compositing textures and `W_InitMultipleFiles` reading the lump directory. Both are pure computation over WAD data. Running this work once on a modern x86 Mac (at ~100× the SE/30's speed) and shipping a pre-processed binary that the SE/30 loads directly would eliminate startup time entirely.

**Why it was not pursued:**

1. **Zone pointer coupling.** Texture data after `R_InitData` is a set of `texture_t` structs with embedded pointers into zone memory (`texpatch_t` arrays, column offset arrays). These pointers are valid only in the specific zone allocation of one run. Serializing them requires pointer fixups — effectively implementing relocatable binary format. This is significant complexity for a one-time startup cost.

2. **Not actually a gameplay problem.** The startup cost is paid once per session. Once loaded, all gameplay is at full speed. Users on real SE/30 hardware leave the machine running; the startup is tolerable.

3. **Workflow complexity.** Getting a pre-processed blob from the modern machine into the SE/30 requires the same shared-folder/floppy transfer used for the application binary itself. Any WAD change (different WAD file, different patches) requires re-running the preprocessor. This adds a step to an already manual workflow.

4. **Binary compatibility.** Pre-processed data would need to be regenerated for different game modes (shareware vs commercial), different WAD files, and potentially different `detailLevel` settings. The current architecture handles all of this automatically at startup.

---

### Opt 17 *(failed)*: Pre-Dithered Texture Columns (Option D)

**Candidate:** The per-pixel Bayer dithering in column drawers involves a table lookup (`grayscale_pal`) plus a threshold comparison (`> bayer4x4[y&3][x&3]`) on every pixel. Pre-computing the dithered texture columns at level load time would eliminate this work from the hot path.

**68030 specifics:** 4 Bayer row phases × all texture columns = 4 variants of each column, pre-dithered to 1-bit. The column drawer would just copy bits — no grayscale conversion, no threshold. This would reduce the column inner loop to essentially a memory copy.

**Implementation:** At texture load, generate 4 pre-dithered variants (one per `y & 3` Bayer row phase). Column drawer selects variant based on `dc_x & 3` (the column's Bayer phase).

**Failure — two reasons:**

1. **Negative FPS impact (-3.7%).** Pre-dithered textures are 4× larger, causing 4× more cache misses on the texture data reads. The 68030 has no data cache — every texture access goes to main memory. Trading one comparison per pixel for 4× more memory traffic was a net loss.

2. **Fundamental visual artifact.** Bayer dithering is a **screen-space** algorithm. The threshold matrix tiles across screen coordinates. When the same texture column is rendered at different distances (perspective projection changes which texture rows map to which screen rows), the dithering pattern should vary accordingly — it is a function of screen position, not texture position. Pre-dithering in texture space then perspective-projecting produces a "texture-space Bayer pattern" that appears as warped spirals and whorls on angled or distant surfaces. This is not a subtle quality loss — it is a severe visual artifact. The approach is fundamentally incompatible with perspective texture mapping. Reverted completely.

---

*End of document.*
