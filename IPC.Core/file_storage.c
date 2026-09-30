#if defined(__linux__) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE /* mkstemp, fsync with -std=c11 */
#endif

#include "file_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ helpers */

/* Opens `path` for binary reading and reports its size. */
static ipc_status open_for_reading(const char *path, FILE **file, size_t *file_size)
{
    long end;
    FILE *opened = fopen(path, "rb");

    if (opened == NULL)
        return IPC_IO_ERROR;

    if (fseek(opened, 0, SEEK_END) != 0 || (end = ftell(opened)) < 0 || fseek(opened, 0, SEEK_SET) != 0)
    {
        fclose(opened);
        return IPC_IO_ERROR;
    }

    *file = opened;
    *file_size = (size_t)end;
    return IPC_OK;
}

#ifdef _WIN32

static ipc_status write_temporary(const char *path, const void *data, size_t size, char *temporary)
{
    char directory[MAX_PATH];
    const char *slash = strrchr(path, '\\');
    const char *forward = strrchr(path, '/');
    FILE *file;
    size_t length;

    if (forward != NULL && (slash == NULL || forward > slash))
        slash = forward;

    length = slash != NULL ? (size_t)(slash - path) : 0;
    if (length >= sizeof directory)
        return IPC_IO_ERROR;

    if (length == 0)
        strcpy(directory, ".");
    else
    {
        memcpy(directory, path, length);
        directory[length] = '\0';
    }

    if (GetTempFileNameA(directory, "ipc", 0, temporary) == 0)
        return IPC_IO_ERROR;

    file = fopen(temporary, "wb");
    if (file == NULL)
    {
        DeleteFileA(temporary);
        return IPC_IO_ERROR;
    }

    if ((size > 0 && fwrite(data, 1, size, file) != size) || fflush(file) != 0)
    {
        fclose(file);
        DeleteFileA(temporary);
        return IPC_IO_ERROR;
    }

    if (fclose(file) != 0)
    {
        DeleteFileA(temporary);
        return IPC_IO_ERROR;
    }

    return IPC_OK;
}

int file_storage_exists(const char *path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

ipc_status file_storage_write(const char *path, const void *data, size_t size)
{
    char temporary[MAX_PATH];
    const ipc_status status = write_temporary(path, data, size, temporary);

    if (status != IPC_OK)
        return status;

    if (!MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileA(temporary);
        return IPC_IO_ERROR;
    }

    return IPC_OK;
}

#else

static ipc_status write_all(int descriptor, const unsigned char *data, size_t size)
{
    while (size > 0)
    {
        const ssize_t written = write(descriptor, data, size);

        if (written <= 0)
            return IPC_IO_ERROR;

        data += written;
        size -= (size_t)written;
    }

    return IPC_OK;
}

int file_storage_exists(const char *path)
{
    struct stat info;

    return stat(path, &info) == 0;
}

ipc_status file_storage_write(const char *path, const void *data, size_t size)
{
    const char *slash = strrchr(path, '/');
    const size_t directory_length = slash != NULL ? (size_t)(slash - path) + 1 : 0;
    char *temporary = malloc(directory_length + sizeof "ipcXXXXXX");
    int descriptor;
    ipc_status status;

    if (temporary == NULL)
        return IPC_OUT_OF_MEMORY;

    memcpy(temporary, path, directory_length);
    strcpy(temporary + directory_length, "ipcXXXXXX");

    descriptor = mkstemp(temporary);
    if (descriptor < 0)
    {
        free(temporary);
        return IPC_IO_ERROR;
    }

    status = write_all(descriptor, data, size);
    if (status == IPC_OK && fsync(descriptor) != 0)
        status = IPC_IO_ERROR;
    if (close(descriptor) != 0 && status == IPC_OK)
        status = IPC_IO_ERROR;
    if (status == IPC_OK && rename(temporary, path) != 0)
        status = IPC_IO_ERROR;

    if (status != IPC_OK)
        unlink(temporary);

    free(temporary);
    return status;
}

#endif

/* ------------------------------------------------------------------ reading */

ipc_status file_storage_read(const char *path, size_t max_size, unsigned char **data, size_t *size)
{
    FILE *file;
    size_t file_size;
    unsigned char *buffer = NULL;
    ipc_status status = open_for_reading(path, &file, &file_size);

    if (status != IPC_OK)
        return status;

    if (file_size > max_size)
    {
        fclose(file);
        return IPC_CORRUPT_DATA;
    }

    if (file_size > 0)
    {
        buffer = malloc(file_size);
        if (buffer == NULL)
        {
            fclose(file);
            return IPC_OUT_OF_MEMORY;
        }

        if (fread(buffer, 1, file_size, file) != file_size)
        {
            free(buffer);
            fclose(file);
            return IPC_IO_ERROR;
        }
    }

    fclose(file);
    *data = buffer;
    *size = file_size;
    return IPC_OK;
}

ipc_status file_storage_read_start(const char *path, unsigned char *buffer, size_t count, size_t *file_size)
{
    FILE *file;
    size_t size;
    size_t wanted;
    ipc_status status = open_for_reading(path, &file, &size);

    if (status != IPC_OK)
        return status;

    wanted = size < count ? size : count;
    if (fread(buffer, 1, wanted, file) != wanted)
        status = IPC_IO_ERROR;

    fclose(file);
    if (status == IPC_OK)
        *file_size = size;
    return status;
}
