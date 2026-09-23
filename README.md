# IPCFileLab

IPCFileLab is a Windows C++17 example of file-backed inter-process communication (IPC). It implements one single-slot message channel shared by the sender and receiver processes.

IPC means independent processes exchange data. This project uses a file as the durable channel data and Windows kernel objects for synchronization. The file is the source of truth; events only notify a waiting process that it should check the file again.

## Architecture

```text
IPC.Sender / IPC.Receiver
          |
      FileChannel
      +----+----------------+
      |    |                |
 Serializer FileStorage Synchronization
                         +-- named mutex
                         +-- data event
                         +-- space event
```

```text
IPC.Core       Static library containing the channel implementation
IPC.Sender     Console sender application
IPC.Receiver   Console receiver application
IPC.Tests      GoogleTest target built by CMake
```

`IPC.Sender` and `IPC.Receiver` depend only on `IPC.Core`; they do not depend on each other.

## Components

- `Message` holds a string payload.
- `Serializer` converts a message payload to and from bytes. The current serializer has no schema, so every byte sequence is a valid payload.
- `FileStorage` reads channel bytes and writes complete replacements through a temporary file.
- `FileChannel` owns initialization, protocol validation, state transitions, waiting, and notification.
- `ChannelState` defines `Empty`, `Writing`, `Ready`, and `Reading`.
- `ChannelHeader` describes the logical state and payload-size fields.
- `ChannelSynchronizer` wraps the named Windows mutex.
- `ChannelNotifier` wraps a named auto-reset Windows event.

## File format and state machine

The on-disk format remains exactly:

```text
[state: uint8][payloadSize: uint32][payload bytes]
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

The sender may create `WRITING` only after observing `EMPTY`. `FileChannel::setState()` permits only `WRITING -> READY`, `READY -> READING`, and `READING -> EMPTY`. The final empty transition writes a fresh header with payload size zero, so an empty file never retains old payload bytes.

## Synchronization and back-pressure

Each canonical absolute channel path has its own deterministic Windows object names. The named mutex protects all state checks and modifications. The data event wakes one receiver after `READY`; the space event wakes one sender after `EMPTY`.

The channel has one slot. A sender finding a non-empty state releases the mutex and waits for the space event. A receiver finding a non-ready state releases the mutex and waits for the data event. Because the state is rechecked under the mutex after every wake-up, an event is a notification rather than a message and does not replace protocol validation.

This gives single-slot back-pressure: a second sender cannot overwrite a ready message, and one receiver consumes each ready message before the channel becomes empty again.

## Initialization, validation, and recovery

Constructing `FileChannel` creates a missing `ipc.dat` as a valid empty header. It never resets an existing valid `READY` message.

The channel rejects malformed data with `std::runtime_error` rather than waiting forever. Validation includes missing or too-small files, invalid state bytes, invalid empty-channel sizes, payload sizes over 16 MiB, truncated payloads, and unexpected file sizes.

`FileStorage::write()` writes a complete temporary file, flushes and closes it, then replaces the target with `MoveFileExA` using `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`. Under the channel mutex, readers therefore see either the old complete file or the new complete file, not the direct-write intermediate content.

`WRITING` and `READING` are recoverable stale states. When a channel is opened or an abandoned mutex is acquired, either state is replaced by an empty header. This discards a message that was not fully published (`WRITING`) or was in delivery (`READING`), avoiding delivery of an incomplete payload or duplicate delivery. `READY` is preserved, including after an abandoned mutex, because it represents a fully published message. Corrupted files and invalid headers are not recovered automatically; they produce a clear error for the application layer.

Windows reports a mutex abandoned when its owner terminates without releasing it. `ChannelSynchronizer::lock()` reports that condition to `FileChannel`, which applies the state-based policy above before proceeding.

## Build with Visual Studio

Open `IPCFileLab.sln` in Visual Studio 2022 or later, select `Debug|x64`, and build the solution. The solution continues to contain:

```text
IPC.Core
IPC.Sender
IPC.Receiver
```

## Build and test with CMake

CMake builds the same production targets and downloads GoogleTest with `FetchContent` during configuration.

```powershell
cmake -S . -B build-cmake
cmake --build build-cmake --config Debug
ctest --test-dir build-cmake -C Debug --output-on-failure
```

The CMake targets are `IPC.Core`, `IPC.Sender`, `IPC.Receiver`, and `IPC.Tests`. `IPC.Tests` is registered with CTest through GoogleTest discovery.

## Run the applications

Run the receiver and sender from the same working directory so both use the same relative `ipc.dat` path:

```powershell
.\IPC.Receiver.exe
.\IPC.Sender.exe
```

Typical output is:

```text
Sender state: 0
Message sent successfully: Message from sender 12345
Receiver state: 2
Received: Message from sender 12345
```

Starting the sender first is supported: the message is stored as `READY` until a receiver starts. Starting the receiver first is supported: it waits for the data event. Multiple senders and receivers share the same single slot and are serialized by the mutex.

## Test coverage

The GoogleTest suite covers empty, normal, and 1 MiB serialization; storage reads and writes; missing files; fresh initialization; preserving a ready message; invalid state and payload-size rejection; stale-writing recovery; empty-message delivery; receiver waiting; sender back-pressure; repeated ordered exchange; and multiple senders and receivers.

Manual CMake-built integrations also verify sender-first delivery and two senders with two receivers.

## Known limitations and future improvements

- The channel is Windows-specific because it uses named mutexes, named events, and `MoveFileExA`.
- The channel provides at-most-once delivery. A process crash during `READING` discards that in-flight message during recovery.
- The serializer intentionally stores only raw string bytes; it has no versioning or structured-message validation.
- A 16 MiB payload limit protects the single-slot file from unbounded message sizes.
- The console applications send or receive one message per invocation. They are useful integration endpoints, not a long-running service protocol.
