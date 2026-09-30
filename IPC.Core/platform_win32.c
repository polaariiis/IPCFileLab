#include "platform.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct platform_lock
{
    HANDLE mutex;
};

struct platform_notifier
{
    HANDLE event;
};

/*
 * 64-bit FNV-1a: a stable hash of the channel path for the kernel object names. Letters are
 * folded to lower case because Windows paths are case-insensitive.
 */
static uint64_t hash_path(const char *path)
{
    uint64_t hash = 14695981039346656037ull;

    for (; *path != '\0'; ++path)
    {
        unsigned char c = (unsigned char)*path;

        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        hash ^= c;
        hash *= 1099511628211ull;
    }

    return hash;
}

static void object_name(char *name, size_t size, const char *prefix, const char *path)
{
    snprintf(name, size, "%s%016llx", prefix, (unsigned long long)hash_path(path));
}

wchar_t *platform_wide_path(const char *path)
{
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    wchar_t *wide;

    if (length <= 0)
        return NULL;

    wide = malloc((size_t)length * sizeof *wide);
    if (wide != NULL && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, length) != length)
    {
        free(wide);
        wide = NULL;
    }

    return wide;
}

/* `wide` as a malloc'ed UTF-8 string; NULL on failure. */
static char *utf8_path(const wchar_t *wide)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    char *path;

    if (length <= 0)
        return NULL;

    path = malloc((size_t)length);
    if (path != NULL && WideCharToMultiByte(CP_UTF8, 0, wide, -1, path, length, NULL, NULL) != length)
    {
        free(path);
        path = NULL;
    }

    return path;
}

ipc_status platform_absolute_path(const char *path, char **absolute)
{
    wchar_t *wide = platform_wide_path(path);
    wchar_t *full = NULL;
    DWORD needed;
    ipc_status status = IPC_IO_ERROR;

    if (wide == NULL)
        return IPC_INVALID_ARGUMENT; /* not UTF-8 */

    needed = GetFullPathNameW(wide, 0, NULL, NULL);
    if (needed > 0)
        full = malloc(needed * sizeof *full);
    if (full != NULL && GetFullPathNameW(wide, needed, full, NULL) > 0)
    {
        *absolute = utf8_path(full);
        status = *absolute != NULL ? IPC_OK : IPC_OUT_OF_MEMORY;
    }

    free(full);
    free(wide);
    return status;
}

ipc_status platform_lock_open(const char *path, platform_lock **lock)
{
    char name[64];
    platform_lock *created = malloc(sizeof *created);

    if (created == NULL)
        return IPC_OUT_OF_MEMORY;

    object_name(name, sizeof name, "IPCFileLabMutex", path);
    created->mutex = CreateMutexA(NULL, FALSE, name);
    if (created->mutex == NULL)
    {
        free(created);
        return IPC_SYNC_ERROR;
    }

    *lock = created;
    return IPC_OK;
}

void platform_lock_close(platform_lock *lock)
{
    if (lock == NULL)
        return;

    CloseHandle(lock->mutex);
    free(lock);
}

ipc_status platform_lock_acquire(platform_lock *lock, int *stale_state_recoverable)
{
    const DWORD result = WaitForSingleObject(lock->mutex, INFINITE);

    if (result == WAIT_OBJECT_0)
    {
        *stale_state_recoverable = 0;
        return IPC_OK;
    }

    if (result == WAIT_ABANDONED)
    {
        *stale_state_recoverable = 1;
        return IPC_OK;
    }

    return IPC_SYNC_ERROR;
}

void platform_lock_release(platform_lock *lock)
{
    ReleaseMutex(lock->mutex);
}

ipc_status platform_notifier_open(const char *path, const char *kind, platform_notifier **notifier)
{
    char prefix[32];
    char name[64];
    platform_notifier *created = malloc(sizeof *created);

    if (created == NULL)
        return IPC_OUT_OF_MEMORY;

    snprintf(prefix, sizeof prefix, "IPCFileLab%sEvent", kind);
    object_name(name, sizeof name, prefix, path);
    created->event = CreateEventA(NULL, FALSE, FALSE, name); /* auto-reset, not signalled */
    if (created->event == NULL)
    {
        free(created);
        return IPC_SYNC_ERROR;
    }

    *notifier = created;
    return IPC_OK;
}

void platform_notifier_close(platform_notifier *notifier)
{
    if (notifier == NULL)
        return;

    CloseHandle(notifier->event);
    free(notifier);
}

ipc_status platform_notifier_notify(platform_notifier *notifier)
{
    return SetEvent(notifier->event) ? IPC_OK : IPC_SYNC_ERROR;
}

ipc_status platform_notifier_wait(platform_notifier *notifier, int timeout_ms)
{
    const DWORD result = WaitForSingleObject(notifier->event, (DWORD)timeout_ms);

    if (result == WAIT_OBJECT_0)
        return IPC_OK;

    if (result == WAIT_TIMEOUT)
        return IPC_TIMEOUT;

    return IPC_SYNC_ERROR;
}

long long platform_now_ms(void)
{
    return (long long)GetTickCount64();
}
