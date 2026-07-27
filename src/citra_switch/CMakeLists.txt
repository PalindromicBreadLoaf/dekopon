# The replacement __libnx_exception_entry is hand-written AArch64.
# And yes, this took as long as you'd expect it to.
enable_language(ASM)

# Dekopon frontend
add_executable(citra_switch
    applets/swkbd.cpp
    applets/swkbd.h
    applets/swkbd_horizon.cpp
    applets/swkbd_horizon.h
    citra_switch.cpp
    config.cpp
    config.h
    default_ini.h
    emu_window.cpp
    emu_window.h
    emulation.cpp
    harfbuzz_stubs.cpp
    # The exception entry stub must be a strong symbol in the executable itself so the linker picks
    # it over libnx's weak __libnx_exception_entry.
    horizon_exception.cpp
    horizon_exception_entry.S
    horizon_exception_entry.h
    horizon_sys_stubs.cpp
    input.cpp
    input.h
    menu.cpp
    menu.h
    menu_data.cpp
    menu_data.h
    overlay_menu.cpp
    overlay_menu.h
    rail_icons.h
    settings_menu.cpp
    settings_menu.h
)

# oaknut_horizon_jit is pulled in transitively via oaknut
target_link_libraries(citra_switch PRIVATE citra_core citra_common inih)

# The launcher draws with a libnx software framebuffer and renders text with FreeType
find_path(SWITCH_FREETYPE_INCLUDE_DIR ft2build.h PATH_SUFFIXES freetype2 REQUIRED)
find_library(SWITCH_FREETYPE freetype REQUIRED)
find_library(SWITCH_PNG png REQUIRED)
find_library(SWITCH_BZ2 bz2 REQUIRED)
find_library(SWITCH_ZLIB z REQUIRED)
target_include_directories(citra_switch PRIVATE ${SWITCH_FREETYPE_INCLUDE_DIR})
# Order matters for the static newlib link: freetype -> png -> bz2 -> z.
target_link_libraries(citra_switch PRIVATE
    ${SWITCH_FREETYPE} ${SWITCH_PNG} ${SWITCH_BZ2} ${SWITCH_ZLIB})

# The GLES bring-up path pulls the switch-mesa EGL/GLES driver. It is mutually exclusive with NXVK
# until NXVK either supports nouveau or Zink (or both).
if (ENABLE_OPENGL)
    # EGL provided by switch-mesa
    find_path(SWITCH_GL_INCLUDE_DIR EGL/egl.h REQUIRED)
    find_library(SWITCH_EGL EGL REQUIRED)
    find_library(SWITCH_GLESV2 GLESv2 REQUIRED)
    find_library(SWITCH_GLAPI glapi REQUIRED)
    find_library(SWITCH_DRM_NOUVEAU drm_nouveau REQUIRED)

    target_include_directories(citra_switch PRIVATE ${SWITCH_GL_INCLUDE_DIR})
    # Prepend our externals/glad so the GLES2 loader is found
    target_include_directories(citra_switch BEFORE PRIVATE
        $<TARGET_PROPERTY:glad,INTERFACE_INCLUDE_DIRECTORIES>)
    target_link_libraries(citra_switch PRIVATE glad
        ${SWITCH_EGL} ${SWITCH_GLESV2} ${SWITCH_GLAPI} ${SWITCH_DRM_NOUVEAU})
endif()

# Vulkan supported provided by the NXVK project
if (ENABLE_VULKAN)
    target_link_libraries(citra_switch PRIVATE nxvk)
endif()

nx_generate_nacp(OUTPUT dekopon.nacp
    NAME "Dekopon"
    AUTHOR "Azahar Team \& PalindromicBreadLoaf"
    VERSION "2.0.0-RC1"
)

# Compile deko3d shaders with uam and pack them into the romfs.
# Do note that the Deko3D backend was never finished and currently isn't planned to be.
if (ENABLE_DEKO3D)
    find_program(UAM_EXE NAMES uam
        PATHS "$ENV{DEVKITPRO}/tools/bin" "${DEVKITPRO}/tools/bin" "/opt/devkitpro/tools/bin"
        REQUIRED)

    set(DK_SHADER_SRC_DIR "${CMAKE_SOURCE_DIR}/src/video_core/renderer_deko3d/shaders")
    set(DK_ROMFS_DIR "${CMAKE_CURRENT_BINARY_DIR}/romfs")
    set(DK_ROMFS_SHADER_DIR "${DK_ROMFS_DIR}/shaders")
    file(MAKE_DIRECTORY "${DK_ROMFS_SHADER_DIR}")

    set(DK_DKSH_FILES "")
    # Compile one glsl shader to dksh.
    function(dk_add_shader NAME STAGE)
        add_custom_command(
            OUTPUT "${DK_ROMFS_SHADER_DIR}/${NAME}.dksh"
            COMMAND "${UAM_EXE}" -s ${STAGE}
                -o "${DK_ROMFS_SHADER_DIR}/${NAME}.dksh" "${DK_SHADER_SRC_DIR}/${NAME}.glsl"
            DEPENDS "${DK_SHADER_SRC_DIR}/${NAME}.glsl"
            COMMENT "uam ${STAGE}: ${NAME}.glsl -> ${NAME}.dksh"
            VERBATIM
        )
        set(DK_DKSH_FILES ${DK_DKSH_FILES} "${DK_ROMFS_SHADER_DIR}/${NAME}.dksh" PARENT_SCOPE)
    endfunction()

    dk_add_shader(present_vsh vert)
    dk_add_shader(present_fsh frag)
    dk_add_shader(ubershader_vsh vert)
    dk_add_shader(ubershader_fsh frag)

    add_custom_target(dekopon_shaders DEPENDS ${DK_DKSH_FILES})
    dkp_add_asset_target(dekopon_romfs "${DK_ROMFS_DIR}")
    add_dependencies(dekopon_romfs dekopon_shaders)
    set_property(TARGET dekopon_romfs APPEND PROPERTY DKP_ASSET_FILES ${DK_DKSH_FILES})

    nx_create_nro(citra_switch
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/dekopon.nro"
        NACP "${CMAKE_CURRENT_BINARY_DIR}/dekopon.nacp"
        ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg"
        ROMFS dekopon_romfs
    )
else()
    nx_create_nro(citra_switch
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/dekopon.nro"
        NACP "${CMAKE_CURRENT_BINARY_DIR}/dekopon.nacp"
        ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg"
    )
endif()
