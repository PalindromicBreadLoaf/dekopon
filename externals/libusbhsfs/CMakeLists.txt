# SPDX-FileCopyrightText: Azahar Emulator Project
# Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
# SPDX-License-Identifier: GPL-2.0-or-later

# libusbhsfs mounts USB mass storage devices as devoptab volumes ("ums0:/", ...).

set(USBHSFS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/libusbhsfs")

set(USBHSFS_SOURCES
    ${USBHSFS_SRC}/source/usbhsfs_drive.c
    ${USBHSFS_SRC}/source/usbhsfs_log.c
    ${USBHSFS_SRC}/source/usbhsfs_manager.c
    ${USBHSFS_SRC}/source/usbhsfs_mount.c
    ${USBHSFS_SRC}/source/usbhsfs_request.c
    ${USBHSFS_SRC}/source/usbhsfs_scsi.c
    ${USBHSFS_SRC}/source/usbhsfs_utils.c
    ${USBHSFS_SRC}/source/fatfs/diskio.c
    ${USBHSFS_SRC}/source/fatfs/ff.c
    ${USBHSFS_SRC}/source/fatfs/ff_dev.c
    ${USBHSFS_SRC}/source/fatfs/ffsystem.c
    ${USBHSFS_SRC}/source/fatfs/ffunicode.c
    ${USBHSFS_SRC}/source/sxos/usbfs.c
    ${USBHSFS_SRC}/source/sxos/usbfs_dev.c
)

# FAT12/16/32 and exFAT are built in. NTFS and EXT2/3/4 require switch-ntfs-3g switch-lwext4
find_path(USBHSFS_NTFS_3G_INCLUDE_DIR ntfs-3g/volume.h)
find_library(USBHSFS_NTFS_3G ntfs-3g)
find_path(USBHSFS_LWEXT4_INCLUDE_DIR ext4.h)
find_library(USBHSFS_LWEXT4 lwext4)

if (USBHSFS_NTFS_3G AND USBHSFS_NTFS_3G_INCLUDE_DIR AND
    USBHSFS_LWEXT4 AND USBHSFS_LWEXT4_INCLUDE_DIR)
    set(USBHSFS_GPL_BUILD ON)
else()
    set(USBHSFS_GPL_BUILD OFF)
endif()

if (USBHSFS_GPL_BUILD)
    list(APPEND USBHSFS_SOURCES
        ${USBHSFS_SRC}/source/ntfs-3g/ntfs.c
        ${USBHSFS_SRC}/source/ntfs-3g/ntfs_dev.c
        ${USBHSFS_SRC}/source/ntfs-3g/ntfs_disk_io.c
        ${USBHSFS_SRC}/source/lwext4/ext.c
        ${USBHSFS_SRC}/source/lwext4/ext_dev.c
        ${USBHSFS_SRC}/source/lwext4/ext_disk_io.c
    )
    message(STATUS "libusbhsfs: FAT, exFAT, NTFS and EXT2/3/4 support")
else()
    message(STATUS "libusbhsfs: FAT and exFAT support only. Install switch-ntfs-3g and "
                   "switch-lwext4 and reconfigure to also mount NTFS and EXT2/3/4 drives")
endif()

add_library(usbhsfs STATIC ${USBHSFS_SOURCES})

string(TIMESTAMP USBHSFS_BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S UTC" UTC)
target_compile_definitions(usbhsfs PRIVATE
    LIB_TITLE="libusbhsfs"
    BUILD_TIMESTAMP="${USBHSFS_BUILD_TIMESTAMP}"
    _GNU_SOURCE
)
target_compile_options(usbhsfs PRIVATE -w)
target_include_directories(usbhsfs PUBLIC ${USBHSFS_SRC}/include)
target_include_directories(usbhsfs PRIVATE ${USBHSFS_SRC}/source)

if (USBHSFS_GPL_BUILD)
    target_compile_definitions(usbhsfs PRIVATE GPL_BUILD)
    target_include_directories(usbhsfs PRIVATE
        ${USBHSFS_NTFS_3G_INCLUDE_DIR} ${USBHSFS_LWEXT4_INCLUDE_DIR})
    target_link_libraries(usbhsfs PUBLIC ${USBHSFS_NTFS_3G} ${USBHSFS_LWEXT4})
endif()
