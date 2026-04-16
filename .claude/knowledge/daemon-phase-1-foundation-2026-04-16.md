# SparkDaemon Phase 1 — Foundation Implementation

**Last updated:** 2026-04-16
**Type:** Observation
**Status:** Active
**Cross-references:** `daemon-services-architecture-2026-04-16.md`

## TL;DR

Phase 1 of the daemon services plan is implemented and tested. The foundation
(protocol + client + server + Control service + daemon executable) is in
place on Linux/macOS. Windows support is stubbed — `Connect()` / `Run()`
return an error until the named-pipe transport lands in a later phase.

- **Binary:** `build/<preset>/bin/SparkDaemon`
- **Default socket:** `./.spark-daemon.sock` (0600 permissions)
- **10 new tests** (6 codec + 4 loopback) all pass in 1.02s
- **Full test suite:** 5511/5512 pass, 1 pre-existing flaky warn (no regressions)

## Files added

| File | Purpose |
|------|---------|
| `SparkEngine/Source/Utils/DaemonProtocol.h` | Wire format: `FrameHeader`, `ServiceId`, `ControlMessage`, codec helpers |
| `SparkEngine/Source/Utils/DaemonFraming.h` | Header-only `SendAll` / `RecvAll` / `SendFrame` / `RecvFrame` — shared by both client and server |
| `SparkEngine/Source/Utils/DaemonClient.{h,cpp}` | Engine-side client: `Connect` / `Request` / `SendOneWay` / `Ping` / `Disconnect` |
| `SparkDaemon/src/ServiceBase.h` | Polymorphic service interface |
| `SparkDaemon/src/ControlService.{h,cpp}` | Built-in ping / version / shutdown handler |
| `SparkDaemon/src/DaemonServer.{h,cpp}` | Accept loop, per-connection thread, dispatch |
| `SparkDaemon/src/main.cpp` | Executable entry point |
| `SparkDaemon/CMakeLists.txt` | Standalone executable target (Linux/macOS only) |
| `Tests/TestDaemonProtocol.cpp` | Codec round-trip + wire-layout tests |
| `Tests/TestDaemonFoundation.cpp` | Loopback test — real server + real client over AF_UNIX |

## Wire format (as shipped)

```
[4B payload length (LE)][2B service ID (LE)][2B message type (LE)][N B payload]
```

- Max payload: 16 MiB (rejects larger frames before allocating)
- Service IDs: Control=0, Asset=1, Shader=2, Collab=3, Build=4
- Control message numbers are wire constants — bumped only with protocol semver

## Two bugs caught during implementation

Recording these because they're subtle and will come up again.

### 1. Don't abort in-flight I/O on the shutdown flag

First draft had `SendAll` / `RecvAll` check the shutdown atomic **before** the
first syscall. That broke the shutdown ack: the `ShutdownRequest` handler
sets `m_shouldStop=true` and returns `ShutdownAck`; the server then tried
`SendAll` which saw the flag and aborted immediately — never writing the
ack. Client blocked forever waiting for a response the server refused to
send.

**Fix:** Check the flag **only when the loop would otherwise retry** (on
`EAGAIN` / `EWOULDBLOCK` after a `SO_RCVTIMEO` timeout, or `EINTR`). Bytes
already accepted by the kernel are never abandoned. Semantically the flag
means "abort *waiting*", not "abort sending what you already have".

### 2. Closing a listen fd from another thread doesn't unblock accept()

Stop() initially tried `shutdown(listenFd, SHUT_RDWR); close(listenFd)` to
kick the accept loop. On Linux `shutdown()` on a listening socket returns
`ENOTCONN`, and closing an fd while another thread is blocked inside
`accept()` on it is undefined behavior.

**Fix:** Make the listen socket non-blocking and wrap accept in a
`poll(pfd, 1, 500)` loop that rechecks `m_shouldStop` on timeout. Stop()
just flips the flag — the poll wakes within 500ms and the loop exits
cleanly. No cross-thread fd manipulation required.

## Phase 1 scope boundaries

**Done:**
- AF_UNIX transport on Linux/macOS
- Binary framing + codec + max-size guard
- Client connect/request/disconnect with internal mutex (multi-thread safe)
- Server accept loop + per-connection dispatch thread
- Registration-time service map; `AddService(unique_ptr<ServiceBase>)`
- Control service: ping, version, shutdown
- Graceful shutdown via `SIGINT`/`SIGTERM` or client `ShutdownRequest`

**Explicitly deferred:**
- Windows named-pipe transport — stubbed to return "not implemented"
- Daemon auto-launch from the engine (engine currently has no
  `Process::Builder::Detached()` call to spawn SparkDaemon on first use)
- `VersionResponse` semver negotiation (clients just read the string today)
- Real services (Asset, Shader, Collab, Build) — each is its own phase

## Engine integration checklist for Phase 2+

When wiring the first real service (Shader, most likely) into the engine:

1. Add `Process::Builder("SparkDaemon").Detached().Launch()` from engine
   startup, with a guard so repeated engines on the same socket don't stomp
   each other (check `kDefaultSocketName` existence + connect; only spawn
   if connect fails).
2. Handle the "no daemon available" fallback — every subsystem that uses
   the daemon must keep its in-process code path intact (per the original
   plan: "the engine falls back to in-process behavior when no daemon is
   running").
3. Put service-client wrappers (e.g. `ShaderDaemonClient`) next to the
   existing subsystem, not in `Utils/`. They compose `DaemonClient` +
   service-specific `BinaryWriter` payloads.

## Testing notes

The loopback tests take ~500ms each because `DaemonServer::Run()` exits on
its 500ms poll tick after `Stop()`. Safe margin, not a correctness issue.
If this becomes a throughput concern, switch to an `eventfd`-based wake-up.
