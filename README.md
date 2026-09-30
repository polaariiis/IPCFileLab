# IPCFileLab

IPCFileLab is a portable C11 example of file-backed inter-process communication (IPC). It implements one single-slot message channel shared by sender and receiver processes, on Windows, Linux and macOS.

IPC means independent processes exchange data. This project uses a file as the durable channel data, an inter-process lock to protect it, and notifications to wake waiting processes. The file is the source of truth; a notification only tells a waiting process to check the file again.

The project was first written in Windows-only C++17; [MIGRATION.md](MIGRATION.md) records how the original behaved and what changed in the port.

## Architecture

```text
IPC.Sender / IPC.Receiver
          |
     ipc.h (C API)
          |
      channel.c
      +----+----------------+
      |    |                |
 protocol file_storage   platform
                          +-- lock
                          +-- data notification
                          +-- space notification
```

```text
IPC.Core       Static C library: the channel (CMake target IPC.Core, alias IPCFileLab::IPC)
IPC.Sender     Console sender program
IPC.Receiver   Console receiver program
IPC.Tests      GoogleTest tests (C++), with IPC.TestHelper for multi-process tests
```

`IPC.Sender` and `IPC.Receiver` depend only on `IPC.Core`; they do not depend on each other.

## Components

- `ipc.h` is the public C API: open, close, send, receive, status codes.
- `channel.c` owns initialization, validation, state transitions, waiting, notification and recovery (the C++ `FileChannel`).
- `protocol.c` defines the states and the header and encodes and checks the header (the C++ `ChannelState`, `ChannelHeader` and `Serializer`). Messages are plain bytes.
- `file_storage.c` reads the channel file and writes complete replacements through a temporary file (the C++ `FileStorage`).
- `platform_win32.c` and `platform_posix.c` provide the lock and the notifications (the C++ `ChannelSynchronizer` and `ChannelNotifier`).

## File format and state machine

The on-disk format is:

```text
[state: uint8][payloadSize: uint32 little-endian][payload bytes]
```

The channel state machine is:

```text
EMPTY
  |
WRITING
  |
READY
  |
READING
  |
EMPTY
```

The sender may create `WRITING` only after observing `EMPTY`. Only `WRITING -> READY`, `READY -> READING` and `READING -> EMPTY` are allowed. The final empty transition writes a fresh header with payload size zero, so an empty file never retains old payload bytes. The format is byte-for-byte the same as the C++ version's.

## C API

```c
#include "ipc.h"

ipc_channel *channel;
if (ipc_channel_open("ipc.dat", &channel) == IPC_OK)
{
    ipc_channel_send(channel, "hello", 5, 1000);          /* wait at most 1 s for space */

    void *data;
    size_t size;
    if (ipc_channel_receive(channel, &data, &size, IPC_WAIT_FOREVER) == IPC_OK)
        ipc_free(data);                                    /* the caller owns the message */

    ipc_channel_close(channel);
}
```

Every function returns an `ipc_status` (`IPC_OK`, `IPC_TIMEOUT`, `IPC_CORRUPT_DATA`, ...); `ipc_status_string()` describes it. Timeouts are in milliseconds; `IPC_WAIT_FOREVER` waits without limit and `0` does not wait. A handle is used by one thread at a time; threads and processes that share a channel each open their own handle.

## Synchronization and back-pressure

| | Windows | Linux, macOS |
|---|---|---|
| Lock | Named mutex `IPCFileLabMutex<hash>` | `flock()` on `<channel>.lock` |
| Notifications | Named auto-reset events `IPCFileLabDataEvent<hash>`, `IPCFileLabSpaceEvent<hash>` | FIFOs `<channel>.data.fifo`, `<channel>.space.fifo` |

The Windows names end in a 64-bit FNV-1a hash of the channel's full path (case-insensitive). The POSIX files are created next to the channel file (mode 0600) and stay there; they hold no data.

The lock protects all state checks and changes. The data notification wakes a receiver after `READY`; the space notification wakes a sender after `EMPTY`.

The channel has one slot. A sender finding a non-empty state releases the lock and waits for the space notification. A receiver finding a non-ready state releases the lock and waits for the data notification. After every wake-up the state is checked again under the lock, so a notification is a hint, not a message. Waits also re-check the channel at least once a second, so a process that died before notifying cannot leave another one waiting.

This gives single-slot back-pressure: a second sender cannot overwrite a ready message, and one receiver consumes each ready message before the channel becomes empty again.

## Initialization, validation, and recovery

Opening a channel creates a missing file as a valid empty header. It never resets an existing valid `READY` message.

Malformed data is reported as `IPC_CORRUPT_DATA` rather than waited on forever: missing or too-small files, invalid state bytes, an `EMPTY` state with a payload, payload sizes over 16 MiB, truncated payloads, and unexpected file sizes. The file size is checked before the file is read into memory.

Every write goes to a temporary file in the same directory, which then replaces the channel file: `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` on Windows, `fsync()` and `rename()` on Linux and macOS. Under the lock, readers therefore see either the old complete file or the new complete file, never a half-written one.

`WRITING` and `READING` are recoverable stale states: they are left only by a process that died in the middle of an operation. They are replaced by an empty header when a channel is opened, and when the lock is taken over from a process that died holding it. This discards a message that was not fully published (`WRITING`) or was in delivery (`READING`), so a partial payload is never delivered and no message is delivered twice. `READY` is always kept. Corrupted files are not repaired automatically.

Windows reports a mutex as abandoned when its owner terminates without releasing it; only then is a stale state recovered, otherwise it is `IPC_PROTOCOL_ERROR`. `flock()` cannot report that, but the kernel releases the lock of a dead process and a live owner never leaves `WRITING`/`READING` behind, so on Linux and macOS a stale state seen under the lock is always recovered.

| A process dies... | Afterwards |
|---|---|
| before writing, or while writing the temporary file | Channel unchanged; a temporary file (`ipc*`) may be left in the directory |
| after publishing `WRITING` | Reset to `EMPTY`; that message is lost (its sender never reported success) |
| after publishing `READY`, before notifying | Kept; a waiting receiver gets it within a second |
| after marking `READING` | Reset to `EMPTY`; the message is lost (at-most-once delivery) |
| after writing `EMPTY`, before notifying | A waiting sender continues within a second |

## Build and test

CMake builds the library, the programs and the tests; GoogleTest is downloaded with `FetchContent` during configuration.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Options: `IPC_WARNINGS_AS_ERRORS` (off by default), `IPC_BUILD_PROGRAMS` and `IPC_BUILD_TESTS` (on when IPCFileLab is the top-level project, off when another project includes it). Visual Studio opens the folder as a CMake project. CI builds and tests Debug and Release on Windows, Linux and macOS, and runs AddressSanitizer and UndefinedBehaviorSanitizer on Linux.

To use the library from another CMake project:

```cmake
include(FetchContent)
FetchContent_Declare(IPCFileLab URL https://github.com/polaariiis/IPCFileLab/archive/<commit>.tar.gz)
FetchContent_MakeAvailable(IPCFileLab)
target_link_libraries(my_target PRIVATE IPCFileLab::IPC)
```

## Run the programs

Run the receiver and sender from the same working directory so both use the same relative `ipc.dat` path:

```sh
./IPC.Receiver
./IPC.Sender
```

Typical output is:

```text
Message sent successfully: Message from sender 12345
Received: Message from sender 12345
```

Starting the sender first is supported: the message is stored as `READY` until a receiver starts. Starting the receiver first is supported: it waits for the data notification. Multiple senders and receivers share the same single slot and are serialized by the lock.

## Test coverage

The GoogleTest suite (40 tests) covers the header encoding and its validation; storage reads, writes, missing and oversized files and temporary-file cleanup; initialization, reopening, preserving a ready message, invalid states, invalid and truncated sizes, corruption found while open, stale `WRITING`/`READING` recovery and the platform rule for stale states; empty, 1 MiB, binary and 16 MiB messages and the rejection of larger ones; invalid arguments; send and receive timeouts; receiver waiting, sender back-pressure, repeated ordered exchange, and multiple senders and receivers as threads; and, with real processes, sender-first and receiver-first exchange, several sender and receiver processes, and processes that die mid-write, mid-read and before notifying.

## Performance

`IPC.Benchmark` times a send followed by a receive on one channel. Release build, Windows 10, NTFS on an NVMe SSD (Ryzen 5 8645HS laptop), per round trip:

| Message | C++ version | C version |
|---|---:|---:|
| 16 bytes | 5.3 ms | 5.4 ms |
| 64 KiB | 71 ms | 34 ms |
| 1 MiB | 32 ms | 14 ms |
| 16 MiB | 210 ms | 47–56 ms |

A round trip is four file replacements, so the file system dominates (the 64 KiB case is consistently slower than 1 MiB on this machine in both versions). The C version checks the state from the 5-byte header instead of reading the whole file each time. A receive with a 100 ms timeout returns after about 110 ms. Linux and macOS were not benchmarked.

## Known limitations

- The channel provides at-most-once delivery. A process crash during `READING` discards that in-flight message during recovery.
- One slot: a sender waits while a message is unread. There is no queue.
- Timeouts bound the wait for the channel's state, not the wait for the lock itself: a process that hangs (without dying) while holding the lock blocks the others. Every operation holds the lock only for a few file writes.
- If writing the channel file fails midway on Windows (a disk error, not a crash), the channel can stay in `READING`; since no mutex was abandoned, operations then report `IPC_PROTOCOL_ERROR` until the channel is reopened, which resets it.
- Payloads are raw bytes of at most 16 MiB; there is no message schema or versioning.
- Every message is written three times (as `WRITING`, `READY` and `READING`) through temporary files, trading speed for crash safety. It suits occasional messages, not high-throughput streams.
- Paths are UTF-8 on every platform (Windows converts them for its wide-character APIs). On Windows the channel's directory is limited to `MAX_PATH` (260 characters), because temporary files are created with `GetTempFileNameW`.
- The channel file and its side files are accessible to the same user; there is no authentication of peers.
- The C++ and C versions use the same file format but different lock names, so they must not use one channel at the same time.
- The console programs send or receive one message per invocation.

## License

IPCFileLab is released under the MIT License; see [LICENSE](LICENSE).
