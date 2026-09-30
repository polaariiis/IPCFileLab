#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE /* flock, mkfifo, clock_gettime with -std=c11 */
#endif

#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

struct platform_lock
{
    int descriptor; /* the open "<channel>.lock" file, locked with flock() */
};

struct platform_notifier
{
    int descriptor; /* the "<channel>.<kind>.fifo" FIFO, open for reading and writing */
};

/* "<path><suffix>", malloc'ed. */
static char *with_suffix(const char *path, const char *suffix)
{
    const size_t path_length = strlen(path);
    const size_t suffix_length = strlen(suffix);
    char *name = malloc(path_length + suffix_length + 1);

    if (name != NULL)
    {
        memcpy(name, path, path_length);
        memcpy(name + path_length, suffix, suffix_length + 1);
    }

    return name;
}

ipc_status platform_absolute_path(const char *path, char **absolute)
{
    char directory[4096];
    char *combined;
    size_t directory_length;

    if (path[0] == '/')
    {
        *absolute = with_suffix(path, "");
        return *absolute != NULL ? IPC_OK : IPC_OUT_OF_MEMORY;
    }

    if (getcwd(directory, sizeof directory) == NULL)
        return IPC_IO_ERROR;

    directory_length = strlen(directory);
    combined = malloc(directory_length + 1 + strlen(path) + 1);
    if (combined == NULL)
        return IPC_OUT_OF_MEMORY;

    memcpy(combined, directory, directory_length);
    combined[directory_length] = '/';
    strcpy(combined + directory_length + 1, path);

    *absolute = combined;
    return IPC_OK;
}

ipc_status platform_lock_open(const char *path, platform_lock **lock)
{
    char *name = with_suffix(path, ".lock");
    platform_lock *created;
    int descriptor;

    if (name == NULL)
        return IPC_OUT_OF_MEMORY;

    descriptor = open(name, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    free(name);
    if (descriptor < 0)
        return IPC_SYNC_ERROR;

    created = malloc(sizeof *created);
    if (created == NULL)
    {
        close(descriptor);
        return IPC_OUT_OF_MEMORY;
    }

    created->descriptor = descriptor;
    *lock = created;
    return IPC_OK;
}

void platform_lock_close(platform_lock *lock)
{
    if (lock == NULL)
        return;

    close(lock->descriptor);
    free(lock);
}

ipc_status platform_lock_acquire(platform_lock *lock, int *stale_state_recoverable)
{
    while (flock(lock->descriptor, LOCK_EX) != 0)
    {
        if (errno != EINTR)
            return IPC_SYNC_ERROR;
    }

    *stale_state_recoverable = 1;
    return IPC_OK;
}

void platform_lock_release(platform_lock *lock)
{
    flock(lock->descriptor, LOCK_UN);
}

ipc_status platform_notifier_open(const char *path, const char *kind, platform_notifier **notifier)
{
    char suffix[16];
    char *name;
    struct stat info;
    platform_notifier *created;
    int descriptor;
    size_t index;

    /* ".data.fifo" or ".space.fifo" */
    suffix[0] = '.';
    for (index = 0; kind[index] != '\0' && index < 9; ++index)
        suffix[index + 1] = (char)(kind[index] >= 'A' && kind[index] <= 'Z' ? kind[index] - 'A' + 'a' : kind[index]);
    strcpy(suffix + index + 1, ".fifo");

    name = with_suffix(path, suffix);
    if (name == NULL)
        return IPC_OUT_OF_MEMORY;

    if (mkfifo(name, 0600) != 0 && errno != EEXIST)
    {
        free(name);
        return IPC_SYNC_ERROR;
    }

    /* Read and write: the FIFO stays open without a peer, and writes never block. */
    descriptor = open(name, O_RDWR | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    free(name);
    if (descriptor < 0)
        return IPC_SYNC_ERROR;

    if (fstat(descriptor, &info) != 0 || !S_ISFIFO(info.st_mode))
    {
        close(descriptor);
        return IPC_SYNC_ERROR;
    }

    created = malloc(sizeof *created);
    if (created == NULL)
    {
        close(descriptor);
        return IPC_OUT_OF_MEMORY;
    }

    created->descriptor = descriptor;
    *notifier = created;
    return IPC_OK;
}

void platform_notifier_close(platform_notifier *notifier)
{
    if (notifier == NULL)
        return;

    close(notifier->descriptor);
    free(notifier);
}

ipc_status platform_notifier_notify(platform_notifier *notifier)
{
    const char signal = 1;

    /* A full FIFO already holds unread notifications, which is just as good. */
    if (write(notifier->descriptor, &signal, 1) == 1 || errno == EAGAIN)
        return IPC_OK;

    return IPC_SYNC_ERROR;
}

ipc_status platform_notifier_wait(platform_notifier *notifier, int timeout_ms)
{
    struct pollfd waiting;
    char signal;
    int result;

    waiting.fd = notifier->descriptor;
    waiting.events = POLLIN;
    waiting.revents = 0;

    result = poll(&waiting, 1, timeout_ms);
    if (result == 0)
        return IPC_TIMEOUT;

    if (result < 0)
        return errno == EINTR ? IPC_OK : IPC_SYNC_ERROR; /* interrupted: the caller re-checks */

    /* Take one notification; another waiter may have taken it first, which is fine. */
    if (read(notifier->descriptor, &signal, 1) < 0 && errno != EAGAIN)
        return IPC_SYNC_ERROR;

    return IPC_OK;
}

long long platform_now_ms(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
