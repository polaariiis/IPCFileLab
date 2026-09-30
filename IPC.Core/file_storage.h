#ifndef IPC_FILE_STORAGE_H
#define IPC_FILE_STORAGE_H

/*
 * Whole-file reads and writes of the channel file. A write goes to a temporary file in the
 * same directory, which then replaces the channel file, so another process never sees a
 * half-written file.
 */

#include "ipc.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if a file exists at `path`, else 0. */
int file_storage_exists(const char *path);

/* Replaces the file at `path` with `size` bytes of `data` (`data` may be NULL when 0). */
ipc_status file_storage_write(const char *path, const void *data, size_t size);

/*
 * Reads the whole file at `path`. A file larger than `max_size` is IPC_CORRUPT_DATA and is not
 * read. On IPC_OK *data is a malloc'ed buffer of *size bytes (NULL when the file is empty).
 */
ipc_status file_storage_read(const char *path, size_t max_size, unsigned char **data, size_t *size);

/*
 * Reads at most `count` bytes from the start of the file into `buffer` and reports the file's
 * size in *file_size (for checking the header without reading the payload).
 */
ipc_status file_storage_read_start(const char *path, unsigned char *buffer, size_t count, size_t *file_size);

#ifdef __cplusplus
}
#endif

#endif
