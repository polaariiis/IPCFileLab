/*
 * A separate process for IPC.Tests:
 *
 *   send <channel> <text> [timeout-ms]   send <text>; exit status = ipc_status
 *   receive <channel> [timeout-ms]       print the received message; exit status = ipc_status
 *   die-writing <channel> <text>         take the lock, publish WRITING with <text>, and exit
 *                                        without releasing the lock (a sender dying mid-write)
 *   die-reading <channel>                take the lock, turn READY into READING, and exit
 *                                        (a receiver dying mid-read)
 *   die-ready <channel> <text>           take the lock, publish READY with <text>, and exit
 *                                        without notifying (a sender dying before notify)
 *
 * The die-* modes follow the steps of channel.c with its internal functions and end the
 * process with _Exit(), which leaves the lock to the operating system: an abandoned mutex
 * on Windows, a released flock() elsewhere.
 */

#include "file_storage.h"
#include "ipc.h"
#include "platform.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int timeout_argument(int argc, char **argv, int index)
{
    return argc > index ? atoi(argv[index]) : IPC_WAIT_FOREVER;
}

static int send_message(const char *path, const char *text, int timeout_ms)
{
    ipc_channel *channel;
    ipc_status status = ipc_channel_open(path, &channel);

    if (status == IPC_OK)
    {
        status = ipc_channel_send(channel, text, strlen(text), timeout_ms);
        ipc_channel_close(channel);
    }

    return (int)status;
}

static int receive_message(const char *path, int timeout_ms)
{
    ipc_channel *channel;
    void *data = NULL;
    size_t size = 0;
    ipc_status status = ipc_channel_open(path, &channel);

    if (status == IPC_OK)
    {
        status = ipc_channel_receive(channel, &data, &size, timeout_ms);
        ipc_channel_close(channel);
    }

    if (status == IPC_OK)
    {
        fwrite(data, 1, size, stdout);
        fflush(stdout);
        ipc_free(data);
    }

    return (int)status;
}

/* Writes <text> as a channel file in `state`. */
static void write_channel(const char *path, channel_state state, const char *text)
{
    const size_t size = strlen(text);
    unsigned char *file = malloc(CHANNEL_HEADER_SIZE + size);
    channel_header header;

    if (file == NULL)
        exit(100);

    header.state = state;
    header.payload_size = (uint32_t)size;
    protocol_encode_header(&header, file);
    memcpy(file + CHANNEL_HEADER_SIZE, text, size);

    if (file_storage_write(path, file, CHANNEL_HEADER_SIZE + size) != IPC_OK)
        exit(101);
    free(file);
}

static int die_holding_lock(const char *mode, const char *path, const char *text)
{
    char *absolute;
    platform_lock *lock;
    int unused;

    if (platform_absolute_path(path, &absolute) != IPC_OK || platform_lock_open(absolute, &lock) != IPC_OK
        || platform_lock_acquire(lock, &unused) != IPC_OK)
        return 102;

    if (strcmp(mode, "die-writing") == 0)
        write_channel(absolute, CHANNEL_WRITING, text);
    else if (strcmp(mode, "die-ready") == 0)
        write_channel(absolute, CHANNEL_READY, text);
    else
    {
        unsigned char *file;
        size_t size;

        if (file_storage_read(absolute, CHANNEL_HEADER_SIZE + IPC_MAX_PAYLOAD_SIZE, &file, &size) != IPC_OK
            || size < CHANNEL_HEADER_SIZE || file[0] != CHANNEL_READY)
            return 103;
        file[0] = CHANNEL_READING;
        if (file_storage_write(absolute, file, size) != IPC_OK)
            return 104;
    }

    _Exit(3); /* the lock is still held: the operating system releases or abandons it */
}

int main(int argc, char **argv)
{
    if (argc >= 4 && strcmp(argv[1], "send") == 0)
        return send_message(argv[2], argv[3], timeout_argument(argc, argv, 4));
    if (argc >= 3 && strcmp(argv[1], "receive") == 0)
        return receive_message(argv[2], timeout_argument(argc, argv, 3));
    if (argc >= 4 && (strcmp(argv[1], "die-writing") == 0 || strcmp(argv[1], "die-ready") == 0))
        return die_holding_lock(argv[1], argv[2], argv[3]);
    if (argc >= 3 && strcmp(argv[1], "die-reading") == 0)
        return die_holding_lock(argv[1], argv[2], "");

    fprintf(stderr, "usage: %s send|receive|die-writing|die-reading|die-ready <channel> ...\n", argv[0]);
    return 99;
}
