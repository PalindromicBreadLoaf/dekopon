// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

// Newlib gap-fills for the non-NXVK Switch build.

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <dirent.h>
#include <malloc.h>
#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

extern "C" {

uid_t getuid(void) {
    return 0;
}
uid_t geteuid(void) {
    return 0;
}
gid_t getgid(void) {
    return 0;
}
gid_t getegid(void) {
    return 0;
}

static long GetAvailableProcessorCount(void) {
    u64 core_mask = 0;
    if (R_FAILED(svcGetInfo(&core_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) ||
        core_mask == 0) {
        return -1;
    }
    return __builtin_popcountll(core_mask);
}

long sysconf(int name) {
    switch (name) {
    case _SC_PAGESIZE:
        return 4096;
    case _SC_PHYS_PAGES:
        return (3ll * 1024 * 1024 * 1024) / 4096;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN:
        return GetAvailableProcessorCount();
    default:
        return -1;
    }
}

int posix_memalign(void** memptr, size_t alignment, size_t size) {
    if (alignment < sizeof(void*) || (alignment & (alignment - 1)) != 0) {
        return EINVAL;
    }
    void* p = memalign(alignment, size);
    if (!p) {
        return ENOMEM;
    }
    *memptr = p;
    return 0;
}

// Horizon has no POSIX signals
int pthread_sigmask(int how, const sigset_t* set, sigset_t* oldset) {
    (void)how;
    (void)set;
    if (oldset) {
        std::memset(oldset, 0, sizeof(*oldset));
    }
    return 0;
}

int flock(int fd, int operation) {
    (void)fd;
    (void)operation;
    return 0;
}

int dirfd(DIR* dirp) {
    (void)dirp;
    errno = ENOTSUP;
    return -1;
}

int fstatat(int fd, const char* path, struct stat* buf, int flag) {
    (void)fd;
    (void)path;
    (void)buf;
    (void)flag;
    errno = ENOTSUP;
    return -1;
}

int getpwuid_r(uid_t uid, struct passwd* pwd, char* buf, size_t buflen, struct passwd** result) {
    (void)uid;
    (void)pwd;
    (void)buf;
    (void)buflen;
    if (result) {
        *result = nullptr;
    }
    return ENOTSUP;
}

} // extern "C"
