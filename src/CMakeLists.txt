cmake_minimum_required(VERSION 3.9)
project(DoomSE30 C CXX)

# Target 68030 (SE/30 CPU), optimize for size+speed
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -m68030 -O3 -fomit-frame-pointer -Wall -Wno-unused-variable -Wno-unused-parameter -Wno-missing-braces -DNORMALUNIX -D__BIG_ENDIAN__ -Wno-sequence-point")

# All Doom engine source files
set(DOOM_SOURCES
    # Engine core
    d_main.c
    d_net.c
    doomdef.c
    doomstat.c
    d_items.c
    dstrings.c
    g_game.c

    # Renderer
    r_bsp.c
    r_data.c
    r_draw.c
    r_main.c
    r_plane.c
    r_segs.c
    r_sky.c
    r_things.c

    # Map / game logic
    p_ceilng.c
    p_doors.c
    p_enemy.c
    p_floor.c
    p_inter.c
    p_lights.c
    p_map.c
    p_maputl.c
    p_mobj.c
    p_plats.c
    p_pspr.c
    p_saveg.c
    p_setup.c
    p_sight.c
    p_spec.c
    p_switch.c
    p_telept.c
    p_tick.c
    p_user.c

    # HUD / menus / intermission
    am_map.c
    f_finale.c
    f_wipe.c
    hu_lib.c
    hu_stuff.c
    m_menu.c
    st_lib.c
    st_stuff.c
    wi_stuff.c

    # Sound
    s_sound.c
    sounds.c

    # Video
    v_video.c

    # Utility
    info.c
    m_argv.c
    m_bbox.c
    m_cheat.c
    m_fixed.c
    m_misc.c
    m_random.c
    m_swap.c
    tables.c

    # WAD / memory
    w_wad.c
    z_zone.c

    # Mac platform layer
    i_main_mac.c
    i_system_mac.c
    i_video_mac.c
    i_input_mac.c
    i_sound_mac.c
    i_net_mac.c

    # Resources
    doom.r
    doom_mike.r
)

if(DOOM_RELEASE_BUILD)
    # Release: no Retro68 console window — suppress it by omitting CONSOLE
    add_application(DoomSE30
        CREATOR "DMSE"
        ${DOOM_SOURCES}
    )
    target_compile_definitions(DoomSE30 PRIVATE DOOM_RELEASE_BUILD=1)
else()
    # Debug: CONSOLE enables the Retro68 console window for printf output
    add_application(DoomSE30
        CREATOR "DMSE"
        ${DOOM_SOURCES}
        CONSOLE
    )
endif()

# d_main.c is startup/loop code, not a hot path.
# -O3 -fomit-frame-pointer on a large translation unit causes GCC 68k
# to miscalculate stack frames (known issue), crashing in IdentifyVersion.
# -O2 with frame pointer present is the safe compromise.
set_source_files_properties(d_main.c PROPERTIES
    COMPILE_FLAGS "-O2 -fno-omit-frame-pointer"
)

# -funroll-loops only on files whose inner loops stay under 256 bytes unrolled.
# 68030 has a 256-byte I-cache; unrolling beyond that causes thrashing.
# r_segs.c EXCLUDED: R_RenderSegLoop was 384-542 bytes unrolled.
# r_draw.c EXCLUDED: R_DrawColumnQuadLow_Mono was 424 bytes unrolled (already 2x manual).
set_source_files_properties(r_bsp.c PROPERTIES
    COMPILE_FLAGS "-funroll-loops"
)
