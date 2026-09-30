#include "ipc.h"

#include <stdio.h>

int main(void)
{
    ipc_channel *channel;
    void *message;
    size_t size;
    ipc_status status = ipc_channel_open("ipc.dat", &channel);

    if (status != IPC_OK)
    {
        fprintf(stderr, "Failed to open channel: %s\n", ipc_status_string(status));
        return 1;
    }

    status = ipc_channel_receive(channel, &message, &size, IPC_WAIT_FOREVER);
    ipc_channel_close(channel);

    if (status != IPC_OK)
    {
        fprintf(stderr, "Failed to receive message: %s\n", ipc_status_string(status));
        return 1;
    }

    printf("Received: %.*s\n", (int)size, size > 0 ? (const char *)message : "");
    ipc_free(message);
    return 0;
}
