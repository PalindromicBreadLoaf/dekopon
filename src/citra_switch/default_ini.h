// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace DefaultINI {

// Options will be added overtime for other things. I just need some basics now.
constexpr const char* sConfigFile = R"(
[Core]
# Whether to use the dynarmic JIT (1, default) or the dyncom interpreter (0).
use_cpu_jit =
# Map guest memory directly into the JIT address space (0, default, restart required).
fastmem = false
# CPU clock speed as a percentage of the real 3DS (5 - 400, default 100).
cpu_clock_percentage =
# Disabled by default to reduce CPU overhead.
is_new_3ds = false

[Renderer]
# Renderer backend: 0: Software, 1: OpenGL, 2: Vulkan (default).
graphics_api =
# Use GLES instead of desktop GL (Forced 1 since we only have GLES).
use_gles =
# Internal resolution scale. 0: auto (window size), 1: native (default).
resolution_factor =
# Synchronise presentation to vblank (1, default).
use_vsync =
# Run PICA and renderer work on a dedicated host thread (0, default, restart required).
async_gpu_emulation = false
# Drain after each GPU trigger for compatibility testing (0, default).
strict_gpu_sync = false
# Compile shaders on a background thread to reduce hitching (0, default).
async_shader_compilation =
# Persist compiled shaders to the SD card to cut post first-run stutter (1, default).
use_disk_shader_cache =
# Run PICA vertex shaders on the GPU (1, default) instead of the CPU shader engine.
# Turning this off costs a lot of performance, but can work around shader stutter or
# rendering bugs in some games. Not recommended to use at all.
use_hw_shader =
# Texture upscaling filter. 0: none (default), 1: Anime4K, 2: Bicubic, 3: ScaleForce,
# 4: xBRZ, 5: MMPX. Anything other than none costs GPU time.
texture_filter =
# Anisotropic filtering for game textures. 0: off, 1: 2x, 2: 4x, 3: 8x, 4: 16x (default).
anisotropic_filtering =
# Sample the 3DS screens bilinearly (1, default) rather than nearest-neighbour.
filter_mode =
# Only scale the screens by whole-number factors, trading screen area for even pixels
# (0, default).
use_integer_scaling =
# Show an on-screen frame-rate counter (0, default).
show_fps = false
# Show the "Compiling shaders" notice during gameplay while shaders are being compiled (0, default).
show_shader_compile_notice = false
# Compile PICA vertex shaders to native code instead of interpreting them (1, default).
# Only affects draws that fall back to the CPU shader engine.
# Has chance of crashing on some games, although should be safe.
use_shader_jit =
# Skip drawing the right eye of the top screen (0, default).
# Greatly improves performance in some games, but can cause flickering in others.
# Can also be toggled live from the quick menu.
disable_right_eye_render = false

[Layout]
# Stretch top-only and bottom-only layouts to fill the entire display (0, default).
stretch_fullscreen = false
# Where the small screen sits inside the big one in the "Bottom screen overlay" layout.
# 0: top right, 1: middle right, 2: bottom right (default), 3: top left, 4: middle left,
# 5: bottom left, 6: top centre, 7: bottom centre.
# Can also be changed live from the quick menu.
overlay_screen_position =
# Width of the overlaid screen as a percentage of the big screen, 10-60 (25, default).
overlay_screen_size =
# How opaque the overlaid screen is drawn, 10-100 (100, default).
overlay_screen_opacity =

[Utility]
# Load a custom texture pack from load/textures/<TITLE_ID>/ (0, default).
# Can also be toggled live from the quick menu.
custom_textures = false
# Preload the whole pack at boot instead of streaming it in (0, default).
# Costs memory up front but avoids in-game hitches. Only matters with custom_textures on.
preload_textures = false
# Dump the game's textures to dump/textures/<TITLE_ID>/ to build a pack (0, default).
# Takes effect on the next launch.
dump_textures = false

[System]
# Console region. -1: auto-select (default), 0: JPN, 1: USA, 2: EUR, 3: AUS, 4: CHN, 5: KOR, 6: TWN.
region_value =
# Where the emulated clock starts. 0: the Switch's clock (default), 1: the fixed time below.
init_clock =
# Fixed start-up time as a Unix timestamp, used when init_clock is 1.
init_time =
# Initial CPU tick count. 0: random (default), 1: the fixed value below.
init_ticks_type =
init_ticks_override =

[Miscellaneous]
# Log filter, e.g. "*:Info" (default) or "*:Debug Core.Cpu:Trace".
log_filter =

[Switch]
# Directory scanned for titles. Defaults to "roms/" under the dekopon directory when unset.
# The dekopon directory itself is set from sdmc:/switch/dekopon/user_dir.txt
roms_dir =
# Optional second directory scanned for titles. Unset by default.
roms_dir_2 =
# Descend into the ROM directories' subfolders when scanning (1, default).
scan_recursive =
# What drives the touch pointer. 0: left stick (default), 1: gyro, 2: right stick.
pointer_source =
# Gyro pointer sensitivity per axis, as a percentage of the default speed (100, default). 10-500.
gyro_sensitivity_x =
gyro_sensitivity_y =
# Motion sensor source. 0: automatic (default), 1: left controller, 2: right controller,
# 3: console/handheld sensor fusion.
gyro_source =
# Physical gyro orientation. 0: horizontal (default), 1: vertical (follows the screen direction).
gyro_orientation =
# Bitmask of screen-layout presets the R3 button cycles through (bit 0 = the first preset).
# Defaults to every preset enabled. The quick menu always offers every layout.
layout_cycle_mask =
# Last address used for Artic Base or the Artic Setup Tool.
last_artic_base_addr =
# Freeze the game while the quick menu is open (0, default).
pause_in_quick_menu = false
# Clockwise rotation of the launcher and the in-game overlay.
menu_rotation = 0
# Turn the d-pad with the menu, for holding the console the same way up as the UI (0, default).
menu_rotate_input = false
# Which GitHub releases the updater follows. 0: stable releases, 1: include prereleases.
update_channel = 0
# Release tag declined at the automatic startup prompt. Manual checks ignore this value.
dismissed_update =
# Build that last reached the launcher. Shows the What's New card once when it changes.
last_seen_version =
# Show the release notes card the first time a new version is launched (1, default).
show_whats_new = true

[Camera]
# PNG or JPEG the emulated cameras show.. Anything in the camera/ folder is offered.
image =
# Which cameras the image feeds. 0: all (default), 1: outer only, 2: inner only.
target = 0

[Experimental]
# Reduce the emulated CPU clock while movie playback is detected (0, default).
movie_cpu_throttle = false
# CPU clock used by the movie throttle, as a percentage (45, default). 10-100.
movie_cpu_clock_percentage = 45
# Skip draws that cannot use the accelerated vertex path (0, default).
skip_slow_draw = false
# Skip texture copies whose source is not already cached by the GPU (0, default).
skip_texture_copy = false
# Keep cached GPU surfaces when the CPU writes 8 bytes or less (0, default).
skip_cpu_write = false

[Controls]
# Use input streamed by the real 3DS while connected to Artic Base (0, default).
use_artic_base_controller = false
# Controller remapping, editable from Settings > Controller Mapping.
# Each control stores the physical Switch button that drives it, by index:
#   0:A 1:B 2:X 3:Y 4:Up 5:Down 6:Left 7:Right 8:L 9:R 10:+ 11:- 12:ZL 13:ZR 14:L3 15:R3
#   16 leaves the control unbound.
# map_toggle_pointer/map_cycle_layout/map_touch_tap/map_swap_screens are the emulator actions.
map_a =
map_b =
map_x =
map_y =
map_up =
map_down =
map_left =
map_right =
map_l =
map_r =
map_start =
map_select =
map_zl =
map_zr =
map_toggle_pointer =
map_cycle_layout =
map_touch_tap =
map_swap_screens =
)";

} // namespace DefaultINI
