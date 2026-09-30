#ifndef IPC_PROTOCOL_H
#define IPC_PROTOCOL_H

/* The channel file: [state: uint8][payload size: uint32, little-endian][payload]. */

#include "ipc.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHANNEL_STATE_SIZE 1
#define CHANNEL_PAYLOAD_SIZE_FIELD_SIZE 4
#define CHANNEL_HEADER_SIZE (CHANNEL_STATE_SIZE + CHANNEL_PAYLOAD_SIZE_FIELD_SIZE)

typedef enum channel_state
{
    CHANNEL_EMPTY = 0,
    CHANNEL_WRITING = 1,
    CHANNEL_READY = 2,
    CHANNEL_READING = 3
} channel_state;

typedef struct channel_header
{
    channel_state state;
    uint32_t payload_size;
} channel_header;

/* Writes the header's CHANNEL_HEADER_SIZE bytes to `out`. */
void protocol_encode_header(const channel_header *header, unsigned char *out);

/*
 * Reads and checks the header of a channel file of `file_size` bytes whose first bytes are
 * `data` (at least CHANNEL_HEADER_SIZE of them when the file is that large). Returns
 * IPC_CORRUPT_DATA for a file that is too small, an unknown state, an EMPTY channel with a
 * payload, a payload over IPC_MAX_PAYLOAD_SIZE, or a file size that does not match.
 */
ipc_status protocol_decode_header(const unsigned char *data, size_t file_size, channel_header *header);

/* WRITING -> READY, READY -> READING and READING -> EMPTY are the only transitions. */
int protocol_is_valid_transition(channel_state current, channel_state next);

/* WRITING and READING are left behind only by an operation that did not finish. */
int protocol_is_recoverable_state(channel_state state);

#ifdef __cplusplus
}
#endif

#endif
