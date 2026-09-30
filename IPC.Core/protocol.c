#include "protocol.h"

void protocol_encode_header(const channel_header *header, unsigned char *out)
{
    const uint32_t size = header->payload_size;

    out[0] = (unsigned char)header->state;
    out[1] = (unsigned char)(size & 0xFFu);
    out[2] = (unsigned char)((size >> 8) & 0xFFu);
    out[3] = (unsigned char)((size >> 16) & 0xFFu);
    out[4] = (unsigned char)((size >> 24) & 0xFFu);
}

ipc_status protocol_decode_header(const unsigned char *data, size_t file_size, channel_header *header)
{
    uint32_t size;

    if (file_size < CHANNEL_HEADER_SIZE)
        return IPC_CORRUPT_DATA;

    size = (uint32_t)data[1]
        | ((uint32_t)data[2] << 8)
        | ((uint32_t)data[3] << 16)
        | ((uint32_t)data[4] << 24);

    switch (data[0])
    {
    case CHANNEL_EMPTY:
        if (size != 0 || file_size != CHANNEL_HEADER_SIZE)
            return IPC_CORRUPT_DATA;
        break;
    case CHANNEL_WRITING:
    case CHANNEL_READY:
    case CHANNEL_READING:
        if (size > IPC_MAX_PAYLOAD_SIZE || file_size != CHANNEL_HEADER_SIZE + (size_t)size)
            return IPC_CORRUPT_DATA;
        break;
    default:
        return IPC_CORRUPT_DATA;
    }

    header->state = (channel_state)data[0];
    header->payload_size = size;
    return IPC_OK;
}

int protocol_is_valid_transition(channel_state current, channel_state next)
{
    return (current == CHANNEL_WRITING && next == CHANNEL_READY)
        || (current == CHANNEL_READY && next == CHANNEL_READING)
        || (current == CHANNEL_READING && next == CHANNEL_EMPTY);
}

int protocol_is_recoverable_state(channel_state state)
{
    return state == CHANNEL_WRITING || state == CHANNEL_READING;
}
