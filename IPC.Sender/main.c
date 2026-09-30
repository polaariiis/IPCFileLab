#include "ipc.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define current_process_id() _getpid()
#else
#include <unistd.h>
#define current_process_id() getpid()
#endif

int main(void)
{
    ipc_channel *channel;
    char message[64];
    ipc_status status = ipc_channel_open("ipc.dat", &channel);

    if (status != IPC_OK)
    {
        fprintf(stderr, "Failed to open channel: %s\n", ipc_status_string(status));
        return 1;
    }

    snprintf(message, sizeof message, "Message from sender %d", (int)current_process_id());

    status = ipc_channel_send(channel, message, strlen(message), IPC_WAIT_FOREVER);
    ipc_channel_close(channel);

    if (status != IPC_OK)
    {
        fprintf(stderr, "Failed to send message: %s\n", ipc_status_string(status));
        return 1;
    }

    printf("Message sent successfully: %s\n", message);
    return 0;
}
