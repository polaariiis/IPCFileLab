#include "ipc.h"

#include "file_storage.h"
#include "platform.h"
#include "protocol.h"

#include <stdlib.h>
#include <string.h>

/*
 * A waiting process re-checks the channel at least this often even without a notification,
 * so a peer that died after changing the state but before notifying cannot leave it stuck.
 */
#define RECHECK_INTERVAL_MS 1000

struct ipc_channel
{
    char *path; /* absolute path of the channel file */
    platform_lock *lock;
    platform_notifier *data_notifier;  /* signalled after READY */
    platform_notifier *space_notifier; /* signalled after EMPTY */
};

/* ------------------------------------------------------------------ file states */

static ipc_status write_empty(const char *path)
{
    unsigned char data[CHANNEL_HEADER_SIZE];
    channel_header header;

    header.state = CHANNEL_EMPTY;
    header.payload_size = 0;
    protocol_encode_header(&header, data);

    return file_storage_write(path, data, sizeof data);
}

/* Reads and checks the header; the payload is not read. */
static ipc_status read_state(const char *path, channel_state *state)
{
    unsigned char start[CHANNEL_HEADER_SIZE];
    size_t file_size = 0;
    channel_header header = { CHANNEL_EMPTY, 0 };
    ipc_status status = file_storage_read_start(path, start, sizeof start, &file_size);

    if (status == IPC_OK)
        status = protocol_decode_header(start, file_size, &header);
    if (status == IPC_OK)
        *state = header.state;

    return status;
}

/*
 * Moves the channel from `current` to `next`. `file` is the whole channel file as last
 * written (header and payload); only its state byte changes. EMPTY writes a fresh header.
 */
static ipc_status set_state(const char *path, unsigned char *file, size_t file_size,
                            channel_state current, channel_state next)
{
    if (!protocol_is_valid_transition(current, next))
        return IPC_PROTOCOL_ERROR;

    if (next == CHANNEL_EMPTY)
        return write_empty(path);

    file[0] = (unsigned char)next;
    return file_storage_write(path, file, file_size);
}

/*
 * The state as seen while holding the lock. A WRITING or READING state left by a process
 * that died holding the lock is reset to EMPTY: WRITING was never published and READING
 * may already have been delivered. Otherwise such a state is a protocol error.
 */
static ipc_status current_state(const ipc_channel *channel, int stale_state_recoverable, channel_state *state)
{
    ipc_status status = read_state(channel->path, state);

    if (status != IPC_OK || !protocol_is_recoverable_state(*state))
        return status;

    if (!stale_state_recoverable)
        return IPC_PROTOCOL_ERROR;

    status = write_empty(channel->path);
    if (status == IPC_OK)
        *state = CHANNEL_EMPTY;
    return status;
}

/* ------------------------------------------------------------------ waiting */

/* The time at which a wait of `timeout_ms` ends; -1 for IPC_WAIT_FOREVER. */
static long long deadline_of(int timeout_ms)
{
    return timeout_ms == IPC_WAIT_FOREVER ? -1 : platform_now_ms() + timeout_ms;
}

/* Waits for `notifier` or the re-check interval; IPC_TIMEOUT once the deadline has passed. */
static ipc_status wait_for(platform_notifier *notifier, long long deadline)
{
    int slice = RECHECK_INTERVAL_MS;
    ipc_status status;

    if (deadline >= 0)
    {
        const long long remaining = deadline - platform_now_ms();

        if (remaining <= 0)
            return IPC_TIMEOUT;
        if (remaining < slice)
            slice = (int)remaining;
    }

    status = platform_notifier_wait(notifier, slice);
    return status == IPC_TIMEOUT ? IPC_OK : status; /* either way: check the channel again */
}

/* ------------------------------------------------------------------ opening and closing */

/* Under the lock: create a missing channel as EMPTY, reset a stale WRITING or READING. */
static ipc_status initialize(const ipc_channel *channel)
{
    channel_state state;
    ipc_status status;

    if (!file_storage_exists(channel->path))
        return write_empty(channel->path);

    status = read_state(channel->path, &state);
    if (status == IPC_OK && protocol_is_recoverable_state(state))
        status = write_empty(channel->path);

    return status;
}

ipc_status ipc_channel_open(const char *path, ipc_channel **channel)
{
    ipc_channel *opened;
    ipc_status status;
    int unused;

    if (path == NULL || path[0] == '\0' || channel == NULL)
        return IPC_INVALID_ARGUMENT;

    opened = calloc(1, sizeof *opened);
    if (opened == NULL)
        return IPC_OUT_OF_MEMORY;

    status = platform_absolute_path(path, &opened->path);
    if (status == IPC_OK)
        status = platform_lock_open(opened->path, &opened->lock);
    if (status == IPC_OK)
        status = platform_notifier_open(opened->path, "Data", &opened->data_notifier);
    if (status == IPC_OK)
        status = platform_notifier_open(opened->path, "Space", &opened->space_notifier);
    if (status == IPC_OK)
        status = platform_lock_acquire(opened->lock, &unused);
    if (status == IPC_OK)
    {
        status = initialize(opened);
        platform_lock_release(opened->lock);
    }

    if (status != IPC_OK)
    {
        ipc_channel_close(opened);
        return status;
    }

    *channel = opened;
    return IPC_OK;
}

void ipc_channel_close(ipc_channel *channel)
{
    if (channel == NULL)
        return;

    platform_notifier_close(channel->space_notifier);
    platform_notifier_close(channel->data_notifier);
    platform_lock_close(channel->lock);
    free(channel->path);
    free(channel);
}

/* ------------------------------------------------------------------ send and receive */

/* Under the lock with the channel EMPTY: publish `file` (a WRITING file) as READY. */
static ipc_status publish(ipc_channel *channel, unsigned char *file, size_t file_size)
{
    ipc_status status = file_storage_write(channel->path, file, file_size);

    if (status != IPC_OK)
        return status;

    status = set_state(channel->path, file, file_size, CHANNEL_WRITING, CHANNEL_READY);
    if (status != IPC_OK)
    {
        write_empty(channel->path); /* do not leave an unpublished WRITING behind */
        return status;
    }

    return platform_notifier_notify(channel->data_notifier);
}

ipc_status ipc_channel_send(ipc_channel *channel, const void *data, size_t size, int timeout_ms)
{
    unsigned char *file;
    size_t file_size;
    channel_header header;
    long long deadline;
    ipc_status status;

    if (channel == NULL || (data == NULL && size > 0) || timeout_ms < IPC_WAIT_FOREVER)
        return IPC_INVALID_ARGUMENT;

    if (size > IPC_MAX_PAYLOAD_SIZE)
        return IPC_PAYLOAD_TOO_LARGE;

    /* The whole file as it will be written: WRITING header, then the payload. */
    file_size = CHANNEL_HEADER_SIZE + size;
    file = malloc(file_size);
    if (file == NULL)
        return IPC_OUT_OF_MEMORY;

    header.state = CHANNEL_WRITING;
    header.payload_size = (uint32_t)size;
    protocol_encode_header(&header, file);
    if (size > 0)
        memcpy(file + CHANNEL_HEADER_SIZE, data, size);

    deadline = deadline_of(timeout_ms);
    for (;;)
    {
        channel_state state;
        int stale_state_recoverable;

        status = platform_lock_acquire(channel->lock, &stale_state_recoverable);
        if (status != IPC_OK)
            break;

        status = current_state(channel, stale_state_recoverable, &state);
        if (status == IPC_OK && state == CHANNEL_EMPTY)
        {
            status = publish(channel, file, file_size);
            platform_lock_release(channel->lock);
            break;
        }

        platform_lock_release(channel->lock);
        if (status != IPC_OK)
            break;

        /* A message is waiting: wait until a receiver makes space. */
        status = wait_for(channel->space_notifier, deadline);
        if (status != IPC_OK)
            break;
    }

    free(file);
    return status;
}

/* Under the lock with the channel READY: take the message and empty the channel. */
static ipc_status take(ipc_channel *channel, void **data, size_t *size)
{
    unsigned char *file = NULL;
    unsigned char *payload = NULL;
    size_t file_size = 0;
    channel_header header = { CHANNEL_EMPTY, 0 };
    ipc_status status = file_storage_read(channel->path, CHANNEL_HEADER_SIZE + IPC_MAX_PAYLOAD_SIZE,
                                          &file, &file_size);

    if (status == IPC_OK)
        status = protocol_decode_header(file, file_size, &header);
    if (status == IPC_OK && header.state != CHANNEL_READY)
        status = IPC_PROTOCOL_ERROR;
    /* The copy is made before the state changes, so a failure leaves the message READY. */
    if (status == IPC_OK && header.payload_size > 0)
    {
        payload = malloc(header.payload_size);
        if (payload == NULL)
            status = IPC_OUT_OF_MEMORY;
        else
            memcpy(payload, file + CHANNEL_HEADER_SIZE, header.payload_size);
    }
    if (status == IPC_OK)
        status = set_state(channel->path, file, file_size, CHANNEL_READY, CHANNEL_READING);
    if (status == IPC_OK)
        status = set_state(channel->path, file, file_size, CHANNEL_READING, CHANNEL_EMPTY);
    if (status == IPC_OK)
        status = platform_notifier_notify(channel->space_notifier);

    free(file);
    if (status != IPC_OK)
    {
        free(payload);
        return status;
    }

    *data = payload;
    *size = header.payload_size;
    return IPC_OK;
}

ipc_status ipc_channel_receive(ipc_channel *channel, void **data, size_t *size, int timeout_ms)
{
    long long deadline;
    ipc_status status;

    if (channel == NULL || data == NULL || size == NULL || timeout_ms < IPC_WAIT_FOREVER)
        return IPC_INVALID_ARGUMENT;

    deadline = deadline_of(timeout_ms);
    for (;;)
    {
        channel_state state;
        int stale_state_recoverable;

        status = platform_lock_acquire(channel->lock, &stale_state_recoverable);
        if (status != IPC_OK)
            return status;

        status = current_state(channel, stale_state_recoverable, &state);
        if (status == IPC_OK && state == CHANNEL_READY)
        {
            status = take(channel, data, size);
            platform_lock_release(channel->lock);
            return status;
        }

        platform_lock_release(channel->lock);
        if (status != IPC_OK)
            return status;

        /* Nothing to read yet: wait until a sender publishes a message. */
        status = wait_for(channel->data_notifier, deadline);
        if (status != IPC_OK)
            return status;
    }
}

/* ------------------------------------------------------------------ helpers */

void ipc_free(void *data)
{
    free(data);
}

const char *ipc_status_string(ipc_status status)
{
    switch (status)
    {
    case IPC_OK:
        return "success";
    case IPC_INVALID_ARGUMENT:
        return "invalid argument";
    case IPC_TIMEOUT:
        return "timed out";
    case IPC_PAYLOAD_TOO_LARGE:
        return "message is too large";
    case IPC_CORRUPT_DATA:
        return "channel file is malformed";
    case IPC_PROTOCOL_ERROR:
        return "channel has a stale or unexpected state";
    case IPC_IO_ERROR:
        return "channel file could not be read or written";
    case IPC_SYNC_ERROR:
        return "channel lock or notification failed";
    case IPC_OUT_OF_MEMORY:
        return "out of memory";
    }

    return "unknown status";
}
