#ifndef IPC_H
#define IPC_H

/*
 * IPCFileLab: file-backed inter-process communication through one single-slot channel.
 *
 * The channel is a file holding [state: uint8][payload size: uint32 LE][payload]. A sender
 * may write only when the state is EMPTY; a receiver reads only when it is READY:
 *
 *     EMPTY -> WRITING -> READY -> READING -> EMPTY
 *
 * Portable C11. Works between processes (and between threads, each with its own channel
 * handle) on Windows, Linux and macOS. See README.md for the protocol and recovery rules.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Largest payload, in bytes. */
#define IPC_MAX_PAYLOAD_SIZE (16u * 1024u * 1024u)

/* Timeout value that waits until the operation can complete. */
#define IPC_WAIT_FOREVER (-1)

typedef enum ipc_status
{
    IPC_OK = 0,
    IPC_INVALID_ARGUMENT,  /* NULL handle or buffer, negative timeout other than IPC_WAIT_FOREVER */
    IPC_TIMEOUT,           /* the channel stayed full (send) or empty (receive) */
    IPC_PAYLOAD_TOO_LARGE, /* more than IPC_MAX_PAYLOAD_SIZE bytes */
    IPC_CORRUPT_DATA,      /* the channel file is malformed: size, state or payload length */
    IPC_PROTOCOL_ERROR,    /* a stale WRITING/READING state that cannot be recovered */
    IPC_IO_ERROR,          /* reading, writing or replacing the channel file failed */
    IPC_SYNC_ERROR,        /* creating or using the lock or a notification failed */
    IPC_OUT_OF_MEMORY
} ipc_status;

/* An open channel. Use one handle from one thread at a time. */
typedef struct ipc_channel ipc_channel;

/*
 * Opens the channel stored in the file at `path` (UTF-8 on every platform), creating it as
 * EMPTY when it does not exist. A stale WRITING or READING state is reset to EMPTY; a READY
 * message is kept. On success *channel receives a handle to close with ipc_channel_close().
 */
ipc_status ipc_channel_open(const char *path, ipc_channel **channel);

/* Closes the handle. The channel file stays. NULL is ignored. */
void ipc_channel_close(ipc_channel *channel);

/*
 * Writes `size` bytes (0 is allowed; `data` may then be NULL) as the channel's message.
 * Waits while the channel holds a message, at most `timeout_ms` milliseconds
 * (IPC_WAIT_FOREVER: no limit; 0: do not wait).
 */
ipc_status ipc_channel_send(ipc_channel *channel, const void *data, size_t size, int timeout_ms);

/*
 * Takes the channel's message. Waits while the channel is empty, at most `timeout_ms`
 * milliseconds. On IPC_OK, *data is a buffer of *size bytes that the caller releases with
 * ipc_free() (*data is NULL when *size is 0); on failure both are left unchanged.
 */
ipc_status ipc_channel_receive(ipc_channel *channel, void **data, size_t *size, int timeout_ms);

/* Releases a buffer returned by ipc_channel_receive(). NULL is ignored. */
void ipc_free(void *data);

/* A short English description of `status`, for messages and logs. */
const char *ipc_status_string(ipc_status status);

#ifdef __cplusplus
}
#endif

#endif
