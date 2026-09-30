#ifndef IPC_PLATFORM_H
#define IPC_PLATFORM_H

/*
 * The operating-system parts of a channel: an inter-process lock that protects the channel
 * file, and notifications that wake a waiting process. platform_win32.c uses a named mutex
 * and named auto-reset events; platform_posix.c uses flock() and FIFOs next to the channel
 * file. A notification is only a hint: the state in the file is always re-checked.
 */

#include "ipc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct platform_lock platform_lock;
typedef struct platform_notifier platform_notifier;

/* An absolute form of `path` (malloc'ed), so every process names the same channel alike. */
ipc_status platform_absolute_path(const char *path, char **absolute);

/* The lock for the channel file at `path` (an absolute path). */
ipc_status platform_lock_open(const char *path, platform_lock **lock);
void platform_lock_close(platform_lock *lock);

/*
 * Waits for the lock. *stale_state_recoverable is set to 1 when a WRITING or READING state
 * seen while holding the lock can only have been left by a process that died holding it:
 * on Windows when the mutex was abandoned, on POSIX always (flock is released when its
 * owner dies, and a live owner never leaves those states behind).
 */
ipc_status platform_lock_acquire(platform_lock *lock, int *stale_state_recoverable);
void platform_lock_release(platform_lock *lock);

/* A notification for the channel at `path`; `kind` is "Data" or "Space". */
ipc_status platform_notifier_open(const char *path, const char *kind, platform_notifier **notifier);
void platform_notifier_close(platform_notifier *notifier);

/* Wakes one waiter, or the next one to wait. */
ipc_status platform_notifier_notify(platform_notifier *notifier);

/* Waits for a notification: IPC_OK when notified, IPC_TIMEOUT after `timeout_ms` (>= 0). */
ipc_status platform_notifier_wait(platform_notifier *notifier, int timeout_ms);

/* Milliseconds of a monotonic clock, for timeouts. */
long long platform_now_ms(void);

#ifdef __cplusplus
}
#endif

#endif
