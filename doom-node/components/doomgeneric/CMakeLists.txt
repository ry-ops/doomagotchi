# Vendored upstream engine — doom-node/third_party/doomgeneric — READ ONLY.
#
# The source list below mirrors upstream `doomgeneric/Makefile` (SRC_DOOM),
# minus the platform `main()` file: we supply our own DG_* implementation in
# `main/doomgeneric_esp32p4.c`. Keep this list in sync with the submodule pin;
# a diff here on a submodule bump is the signal to re-review upstream.
#
# Deliberately excluded (not in upstream SRC_DOOM): every doomgeneric_<plat>.c,
# plus mus2mid.c / gusconf.c / icon.c / i_sdl*/i_allegro* (SDL/Allegro audio,
# and FEATURE_SOUND is off in doomfeatures.h).

get_filename_component(DG_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../third_party/doomgeneric/doomgeneric" ABSOLUTE)

set(DG_UNITS
    dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main
    d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom
    i_joystick i_scale i_sound i_system i_timer memio m_argv m_bbox m_cheat
    m_config m_controls m_fixed m_menu m_misc m_random p_ceilng p_doors p_enemy
    p_floor p_inter p_lights p_map p_maputl p_mobj p_plats p_pspr p_saveg
    p_setup p_sight p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw
    r_main r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff
    s_sound tables v_video wi_stuff w_checksum w_file w_main w_wad z_zone
    w_file_stdc i_input i_video doomgeneric)

set(DG_SRCS "")
foreach(u ${DG_UNITS})
    list(APPEND DG_SRCS "${DG_DIR}/${u}.c")
endforeach()

idf_component_register(
    SRCS ${DG_SRCS}
    INCLUDE_DIRS "${DG_DIR}")

# Match upstream Makefile defines (default + Makefile.soso are identical here).
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    NORMALUNIX LINUX SNDSERV _DEFAULT_SOURCE)

# 1993 C against GCC 15 on a bare-metal target. We do NOT patch the vendored
# tree (ADR 0001), so the vendored component alone silences the noise and
# un-promotes the diagnostics GCC 14+ made errors by default. Our own code
# (main/) keeps the strict defaults.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -w
    -std=gnu11
    -fno-strict-aliasing
    -Wno-error=implicit-function-declaration
    -Wno-error=implicit-int
    -Wno-error=int-conversion
    -Wno-error=incompatible-pointer-types)
