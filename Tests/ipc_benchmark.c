/*
 * IPC.Benchmark: time a send followed by a receive on one channel, for several message
 * sizes, and a receive that times out. Build Release; run from any directory:
 *
 *   IPC.Benchmark [channel-file]            (default: ipc_benchmark.dat)
 *
 * Each round trip is four file replacements (WRITING, READY, READING, EMPTY), so the
 * results mostly measure the file system.
 */

#include "ipc.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int measure(ipc_channel *channel, size_t size, int rounds)
{
    char *message = malloc(size > 0 ? size : 1);
    long long start;
    long long elapsed;
    int round;

    if (message == NULL)
        return 1;
    memset(message, 'x', size);

    start = platform_now_ms();
    for (round = 0; round < rounds; ++round)
    {
        void *data;
        size_t received;

        if (ipc_channel_send(channel, message, size, 1000) != IPC_OK
            || ipc_channel_receive(channel, &data, &received, 1000) != IPC_OK || received != size)
        {
            free(message);
            return 1;
        }
        ipc_free(data);
    }
    elapsed = platform_now_ms() - start;

    printf("%9zu bytes: %4d round trips in %6lld ms = %8.3f ms each\n", size, rounds, elapsed,
           (double)elapsed / rounds);
    free(message);
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "ipc_benchmark.dat";
    ipc_channel *channel;
    void *data;
    size_t size;
    long long start;

    if (ipc_channel_open(path, &channel) != IPC_OK)
    {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }

    if (measure(channel, 16, 500) || measure(channel, 64 * 1024, 200) || measure(channel, 1024 * 1024, 50)
        || measure(channel, IPC_MAX_PAYLOAD_SIZE, 5))
    {
        fprintf(stderr, "round trip failed\n");
        ipc_channel_close(channel);
        return 1;
    }

    start = platform_now_ms();
    if (ipc_channel_receive(channel, &data, &size, 100) != IPC_TIMEOUT)
        fprintf(stderr, "expected a timeout\n");
    printf("receive with a 100 ms timeout on an empty channel returned after %lld ms\n",
           platform_now_ms() - start);

    ipc_channel_close(channel);
    return 0;
}
