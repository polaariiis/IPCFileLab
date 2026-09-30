# Migration to portable C

This branch (`migration/c-port-studyboard`) ports IPCFileLab from Windows-only C++17 to
portable C11 so that StudyBoard can use it. It is the same project in another language:
one file-backed single-slot channel, the same state machine, the same file format and the
same recovery rules. It is not a redesign.

## The original, as implemented (C++, `main` at `f5486ce`)

Traced from the source and tests, not only the README:

* **Opening** (`FileChannel` constructor) normalises the path (`absolute().lexically_normal()`),
  derives three Windows object names from `std::hash` of it (a named mutex, a "data" and a
  "space" auto-reset event), then, under the mutex: creates a missing file as an empty
  header; resets `WRITING`/`READING` to an empty header; keeps `READY`; throws on anything
  malformed.
* **File format:** `[state: uint8][payloadSize: uint32][payload]`, the size in the machine's
  byte order (little-endian on every target the original ran on). `EMPTY` must have size 0
  and no payload; other states must have exactly `payloadSize` bytes, at most 16 MiB.
* **send:** lock → if the state is `WRITING`/`READING`: recover when the mutex was abandoned,
  otherwise throw "stale channel state" → if not `EMPTY`: unlock and wait for the space
  event (forever) and retry → else write the whole file as `WRITING` with the payload, then
  `READY` (the whole file again), signal the data event, unlock.
* **receive:** lock → same stale check → if `READY`: check the size, rewrite the file as
  `READING`, copy the payload, write an empty header (`EMPTY`), signal the space event,
  unlock, return → else unlock and wait for the data event (forever) and retry.
* **Transitions** (`setState`): only `WRITING→READY`, `READY→READING`, `READING→EMPTY`;
  `EMPTY` always rewrites a fresh 5-byte header.
* **Storage:** every write goes to a temporary file in the same directory
  (`GetTempFileNameA`), is flushed and closed, then replaces the channel with
  `MoveFileExA(REPLACE_EXISTING | WRITE_THROUGH)`. Every read reads the whole file.
* **Notification:** auto-reset events; one `SetEvent` wakes one waiter or stays signalled
  until one waits. After every wake-up the state is re-read under the mutex — events are
  hints, the file is the truth.
* **Errors:** `std::runtime_error` for everything.
* **Quirks kept in mind:** the library prints "Sender state"/"Receiver state" to stdout; the
  method is spelled `recieve`; a file of any size is read completely before its size is
  checked; waits have no timeout; object names depend on the compiler's `std::hash`.

### What happens when a process dies (original)

| Moment of death | Channel afterwards | Delivery |
|---|---|---|
| Before writing (holding the mutex, state `EMPTY`) | `EMPTY` | Nothing was sent |
| While writing the temporary file | Unchanged (`EMPTY`); the temporary file is left behind | Message not sent |
| After `WRITING` was published, before `READY` | `WRITING` → reset to `EMPTY` by the next lock (abandoned mutex) or open | Message lost; the sender never reported success |
| After `READY` was published | `READY`, kept | Delivered later |
| After `READING` was published | `READING` → reset to `EMPTY` | Message lost (at-most-once) |
| After `EMPTY` was written, before the space event | `EMPTY` | Fine, but a sender waiting forever for the space event is not woken until another receive |
| During the file replacement | The old or the new complete file | As for the state it contains |

## Mapping to C

| C++ (original) | C (this branch) |
|---|---|
| `FileChannel` | `channel.c`: open, close, send, receive, recovery, transitions |
| `ChannelState`, `ChannelHeader`, `Serializer`, `Message` | `protocol.c/h`: the state enum, the header struct, explicit encoding and validation. Payloads are plain bytes (`const void*`, `size_t`), so `Message` and the pass-through `Serializer` are not needed as types |
| `FileStorage` | `file_storage.c/h`: whole-file read, temporary-file replacement |
| `ChannelSynchronizer`, `ChannelNotifier` | `platform.h` with `platform_win32.c` and `platform_posix.c`: a lock and two notifications |
| exceptions | `ipc_status` values |
| `IPC.Sender`, `IPC.Receiver`, GoogleTest `IPC.Tests` | the same programs in C; the same GoogleTest tests calling the C API, plus real multi-process tests |

The layers stay the same: channel → storage / protocol / platform.

## Portability decisions

| Topic | Windows | Linux, macOS | Why |
|---|---|---|---|
| Lock | Named mutex (as before), abandoned-mutex detection | `flock` on `<channel>.lock`, released by the kernel when a process dies | No portable named mutex; `flock` has the property recovery needs |
| Stale `WRITING`/`READING` seen under the lock | Recovered if the mutex was abandoned, else `IPC_PROTOCOL_ERROR` (as before) | Recovered: under the lock these states only remain when the previous holder died | `flock` cannot report abandonment |
| Notification | Named auto-reset events (as before) | A FIFO per event (`<channel>.data.fifo`, `<channel>.space.fifo`): notify writes a byte, wait `poll`s and reads one | No named events on POSIX; `sem_timedwait` is missing on macOS |
| Object names | `IPCFileLabMutex…`, `…DataEvent…`, `…SpaceEvent…` + 64-bit FNV-1a of the full path | Files next to the channel | `std::hash` is not available in C and not stable across compilers |
| File replacement | `GetTempFileNameA` + `MoveFileExA(REPLACE_EXISTING \| WRITE_THROUGH)` (as before) | `mkstemp` in the same directory + `fsync` + `rename` | Both replace the file as a whole for other processes |
| Byte order | The size is encoded little-endian explicitly — the same bytes the original wrote on x86/x64 | | No struct is written directly |

## Deliberate changes (each needed for portability or for StudyBoard)

* **Timeouts:** `send` and `receive` take a timeout in milliseconds (`IPC_WAIT_FOREVER`
  for the original behaviour). A library inside an application must not block forever.
* **Waits re-check the state at least once a second** even without a notification, so a
  peer that died between writing `EMPTY`/`READY` and notifying cannot leave a waiter stuck
  (the last row of the table above). This is a periodic re-check, not busy polling.
* **The library prints nothing**; the console programs print.
* **The file size is checked before it is read into memory** (at most header + 16 MiB).
* **Status codes instead of exceptions.**

Everything else — the format, the states, the transitions, the recovery policy, the 16 MiB
limit, at-most-once delivery — stays as it was.
