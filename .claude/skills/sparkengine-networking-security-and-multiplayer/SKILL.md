---
name: sparkengine-networking-security-and-multiplayer
description: >-
  SparkEngine multiplayer networking and security: UDP transport and wire format, connection
  handshake/trust boundaries, entity replication, client prediction and reconciliation, lag
  compensation, MMO account/auth/session-token security (PBKDF2, CSPRNG), packet validation,
  WorldServer/AreaServer coordination, and what multiplayer test evidence actually exists.
  TRIGGER when the user says things like "add a network message type", "is the login system
  secure", "session token", "password hashing", "packets are rejected", "client prediction
  rubber-bands", "lag compensation misses", "hook up encryption", "rate limit clients",
  "protocol version mismatch", "server full / connect rejected", "MMO account", "RCON",
  "replication scope", or "run the network tests".
  DO NOT TRIGGER for database/save-file persistence or schema migrations (use
  sparkengine-persistence-save-and-migrations), game-module DLL ABI or hot-reload seams (use
  sparkengine-modules-sdk-abi-and-hot-reload), the chronology of past networking incidents (use
  sparkengine-failure-archaeology), or generic build/CI failures (use
  sparkengine-build-ci-and-dependencies).
---

# SparkEngine networking, security, and multiplayer

Runbook for the engine's UDP multiplayer stack and its security posture. Everything below was
verified on 2026-08-23 against the working tree of branch `claude/whole-nine-yards-20260823`
(uncommitted changes ahead of `0e1fe7e7`) by reading source — no full-suite or CI run at this
exact tree. Status labels are strict:

- **Implemented** — code exists and compiles into the engine.
- **Wired** — a production caller actually invokes it (not just tests).
- **Tested** — a registered test in `Tests/` exercises it.
- **CI-enforced** — the test runs in the blocking CI jobs (`build-linux-gcc/clang`,
  `build-windows-vs2022` run `ctest`; the single CTest entry `SparkEngineTests` wraps all tests).
- **Release-ready** is claimed for NOTHING in this document. Several load-bearing gaps remain
  (see "Open gaps" below).

Jargon, once: **CSPRNG** = cryptographically secure pseudo-random number generator (OS-backed).
**PBKDF2** = password-based key derivation function 2, an iterated HMAC that makes password
guessing expensive. **Reconciliation** = client re-applies unacknowledged inputs after snapping
to an authoritative server state. **Lag compensation** = server rewinds hitboxes to the shooter's
view time before ray-testing. **RCON** = remote console (privileged admin commands).

## When NOT to use this skill

| You are actually asking about | Go to |
|---|---|
| Saving accounts/characters to disk or DB, schema migrations | `sparkengine-persistence-save-and-migrations` |
| Module DLL boundaries, `IEngineContext`, hot-reload teardown ordering | `sparkengine-modules-sdk-abi-and-hot-reload` |
| "Why did commit X revert Y", incident history, trust-boundary night | `sparkengine-failure-archaeology` (Era 6 covers 2026-08-23 networking commits `68998265`, `5556061d`, `8c096003`) |
| A concrete observed bug you need to localize first | `sparkengine-debugging-playbook` |
| CI job failures, sanitizer rows, dependency setup | `sparkengine-build-ci-and-dependencies` |
| Whether a change is safe to merge / release gates | `sparkengine-change-control-and-release-readiness` |

## Map of the stack

All engine networking lives in `SparkEngine/Source/Engine/Networking/` and is guarded by
`ENABLE_NETWORKING` (a CMake option, ON by default). When OFF, `NetworkManagerStub`
(`NetworkManager.h:742`) compiles in its place and most network tests silently reduce to a
trivially-green skip stub — see "The skip trap" below.

| File(s) | Role | Status |
|---|---|---|
| `NetworkManager.{h,cpp}`, `NetworkConnection.cpp`, `NetworkReliable.cpp`, `NetworkReplication.cpp` | Core UDP client/server: raw Winsock2/POSIX sockets, handshake, channels, replication | Implemented, wired, tested |
| `PacketValidator.{h,cpp}` | Per-message-type schema/size/auth/direction validation, called at ingress (`NetworkConnection.cpp:903`) | Implemented, wired, tested |
| `ClientPrediction.{h,cpp}`, `SubTickInput.{h,cpp}` | Client-side prediction + reconciliation; sub-tick input timing | Implemented, wired, tested |
| `LagCompensation.{h,cpp}` | Server-side hitbox rewind with hardened ray validation | Implemented, wired, tested |
| `WorldServer.{h,cpp}`, `AreaServer.{h,cpp}` | HeroEngine-style area coordination. **In-process only — zero socket calls**; NetworkManager bridges packets to them | Implemented, wired (via MMO module), tested |
| `DedicatedServer.{h,cpp}` | Standalone server loop; owns real sockets; trusted in-process RCON API | Implemented, tested |
| `ITransport.h`, `UDPTransport.h`, `SteamTransport.h`, `NetworkIntegration.h` (`NetworkStack`) | Pluggable transport abstraction. **NetworkManager does NOT use it** — it opens raw sockets directly. `SteamTransport` is a fail-everything stub (see its 17-line `@warning`) | Implemented, **not wired**, tested |
| `NetworkEncryption.{h,cpp}` | XOR stream cipher + truncated HMAC, `RateLimiter`, `ReplayProtection` | Implemented, **not wired** (test-only consumers), tested |
| `NetworkSecurity.h` | Older XOR + connection-token helper used only by `NetworkStack` | Implemented, **not wired**, tested — and uses `std::mt19937`, not the CSPRNG |
| `DeltaSnapshotManager`, `EntityReplicator`, `InterpolationBuffer`, `NetQuantize`, `ReplicationFields`, `ConnectionScope(Filter)` | Snapshot deltas, interpolation, quantization, interest scoping | Implemented, tested |
| `InstabilitySimulator.{h,cpp}` | Deliberate packet loss/jitter injection for testing | Implemented, tested |
| `INetworkRuntime.h`, `NetworkManagerRuntimeAdapter.{h,cpp}` | DedicatedServer-facing seam over NetworkManager | Implemented, wired |
| `SparkEngine/Source/Utils/SecureRandom.{h,cpp}` | OS CSPRNG: `BCryptGenRandom` (Windows) / `getrandom` (Linux) / `arc4random_buf` (macOS) | Implemented, wired, tested |
| `SparkEngine/Source/Utils/PasswordHash.{h,cpp}` | Self-contained PBKDF2-HMAC-SHA256 | Implemented, wired, tested |
| `GameModules/SparkGameMMO/Source/Account/MMOAccountSystem.{h,cpp}` | MMO registration/login/sessions/bans; synchronous borrowed credential views | Implemented, wired, production-linked unit-tested |

Wire format reference: `docs/specs/networking-wire-format.md` (Version 1.0, 2026-04-01).
Wiki: `wiki/subsystems/Networking.md`, `wiki/subsystems/Multiplayer-Quick-Start.md`.

## Wire protocol facts (verified in code, not just the spec)

- Every datagram: magic `0x5350524B` ("SPRK"), then `type(u16) channel(u8) senderID(u32)
  sequence(u32) timestamp(f32) payloadLen(u32)` — minimum 23 bytes on the wire
  (`NetworkManager.cpp:266-316`). Bad magic, short header, datagrams above the IPv4 UDP wire
  maximum (65,507 bytes), payloads above `MAX_NETWORK_MESSAGE_PAYLOAD_SIZE` (65,484 bytes after
  the 23-byte header), or `channel > 2` ⇒ packet dropped with a WARN. Shared limits live in
  `NetworkWireLimits.h` and outbound rejection occurs before reliable sequence allocation.
- Default port `27015` (`NetworkManager.h:88`). Channels: 0 Unreliable, 1 Reliable,
  2 ReliableOrdered.
- **There is no protocol version negotiation.** The spec's handshake diagram says
  "Connect (version, name)" but `NetworkManager::HandleConnect` (`NetworkConnection.cpp:651`)
  reads only a player-name string from the payload. The magic word is the only wire-level
  compatibility guard. Decision rule: treat client and server as **same-build-only**; if you
  change any message layout, you must rebuild both sides and update
  `docs/specs/networking-wire-format.md` plus the wire-freeze tests
  (`Tests/TestTFNetProtocolLayout.cpp` for the TERRAFRONT module protocols).
- Ingress trust boundary (all landed 2026-08-23, commits `68998265`/`5556061d`/`8c096003`):
  clients only accept gameplay datagrams from the exact server endpoint configured by
  `Connect`; unauthenticated senders of custom (module-defined) message types are rejected;
  rejected connects no longer receive a full entity sync (`HandleConnect` returns the admitted
  `ClientID` or `INVALID_CLIENT`, and `SendFullSync` refuses non-clients); chat can never reach
  `ExecuteRcon`.
- `PacketValidator` runs on every accepted message (`NetworkConnection.cpp:903`) enforcing
  min/max payload size, auth requirement, client/server direction, and string sanitization per
  `MessageSchema`.

## Auth and token security — the reconciled ledger

This is the part most often misquoted. State it exactly:

| Claim | Reality (verified) | Status |
|---|---|---|
| "CSPRNG everywhere" | `Spark::SecureRandom::Fill` is a real OS CSPRNG and **fails closed** (returns `false`, callers refuse to proceed). Used by `PasswordHash::Create` and `MMOAccountSystem::GenerateSessionToken`. **BUT** `NetworkSecurity.h:185-194` still seeds `std::mt19937` from `std::random_device` for its keys/tokens, and `NetworkEncryption.cpp:21-23` uses `std::mt19937_64` — neither is a CSPRNG. Both are currently unwired library code, so no live traffic depends on the weak RNG today. | Implemented + tested for SecureRandom; weak-RNG residue labeled **open** |
| "PBKDF2 600k" | `Spark::PasswordHash` (`PasswordHash.cpp:21-23`): PBKDF2-HMAC-SHA256, `kIterations = 600000`, verify accepts 600k–1M only (older/lower work factors **fail closed**), 128-bit salt (`kSaltBytes = 16`), 256-bit derived key, constant-time compare, 1 KB password cap, self-describing `pbkdf2-sha256$iters$salt$dk` format. HMAC key prepared once (not per-iteration) to kill a password-length DoS. **Do not conflate with TERRAFRONT**: `GameModules/SparkGameMMOFPS/.../TFAccountSystem.cpp:39` uses its own `TFCrypto` PBKDF2 at **150,000** iterations with legacy-hash migration. Two systems, two work factors. | Implemented, wired, tested (`TestPasswordHash.cpp` incl. a published-vector construction check; `TestTFOnboarding.cpp` for TF) |
| "128-bit tokens" | `MMOAccountSystem::GenerateSessionToken()` = `SecureRandom::HexToken(16)` → 128 random bits as 32 hex chars. Login retries up to 8 times on map collision, fails closed if the CSPRNG fails ("Unable to create a secure session"), and only replaces an existing session after a token is secured (`MMOAccountSystem.cpp:201-248`). Engine-side `NetworkEncryption` `ConnectionToken` is also 128-bit with constant-time `ValidateToken` — but unwired. | Implemented, wired (MMO), production-linked account tests in `TestMMOCredentialSecurity.cpp` |
| "mutexes" | `MMOAccountSystem` guards every public method with a `std::recursive_mutex`; account-system `Update` is wired into `SparkGameMMOModule::OnUpdate` (`GameModules/SparkGameMMO/Source/Core/Main.cpp:402`), so the 30-min idle session timeout and 15-min lockout expiry actually run. `NetworkManager` serializes public lifecycle/state mutation with `m_apiMutex`, uses value snapshots for stats/clients/inputs, and invokes registered message/timeout/reconnect/replication callbacks without holding API or replication locks; lifecycle/mutation epochs prevent unsafe resume or lost dirtiness after unlocked callbacks. | Implemented, wired, callback-deadlock regressions tested |
| "finite/normalization hardening" | `LagCompensator::RaycastRewound` (`LagCompensation.cpp:184-221`) rejects non-finite origin/dir/rewind/maxDist, rejects near-zero direction, normalizes internally, and skips poses with non-finite or non-positive radius/height. `WorldServer::GetAreaForPosition` (`WorldServer.cpp:166`) and `AreaServer` entity migration (`AreaServer.cpp:115`) reject non-finite positions. `SubTickInput` clamps `tickFraction` to `[0, 0.9999]`. | Implemented, wired, tested (`TestLagCompensation*.cpp`) |

Login flow hardening that exists: account lockout after 5 failed attempts
(`MAX_FAILED_LOGINS`), 20-entry login history ring, ban expiry auto-clear, uniform
"Invalid username or password" for unknown-user and wrong-password, registration rejects when
`PasswordHash::Create` returns empty (CSPRNG unavailable), and the legacy per-account `salt`
field is intentionally empty — salt lives inside the self-describing hash string.

## Open gaps — do NOT claim release readiness while these stand

1. **Encryption is not on the wire.** `NetworkManager` sends plaintext UDP.
   `NetworkEncryption` (XOR keystream + 4-byte truncated HMAC + replay window) and
   `NetworkSecurity` have **zero production consumers** — only tests and the equally-unwired
   `NetworkStack`. Both headers self-describe as interim stand-ins for DTLS/AES-GCM. Anything
   you read about "per-connection session keys" is library capability, not deployed behavior.
2. **Transport abstraction is parallel, not primary.** `NetworkManager` opens raw sockets
   directly; `ITransport`/`UDPTransport`/`NetworkStack` form a second, unwired path.
   `SteamTransport` is a documented stub: `Initialize()`/`Send()` return `false`,
   `Receive()` returns `-1`, `IsReady()` returns `false`.
3. **Fallback singleton retained.** `NetworkManager::GetInstance()`
   (`NetworkManager.cpp:130-145`) prefers the `EngineContext` network service but falls back to
   a function-local `static NetworkManager`. This was deliberately kept ("load-bearing in
   bootstrap", commit `52636dda` — see failure-archaeology). In module DLL code, always resolve
   via the injected `IEngineContext`; a per-image static is a *different object* from the
   host's (the exact trap documented at `GameModules/SparkGameMMO/Source/World/MMOWorldSetup.cpp:136`
   for `SeamlessAreaManager`).
4. **No live rate limiting.** The only flood control on real ingress is per-packet size/schema
   checks and unreliable-channel drop-under-flood; the `RateLimiter` class is unwired.
5. **Closed race claim — keep snapshots by value.** `NetworkManager::GetPendingInputs()`,
   `GetStats()`, and connected-client accessors now lock and return value snapshots. Do not
   regress them to references into containers whose mutex is released at return.
6. **Caps to respect, not remove.** `ClientPrediction` trims pending inputs above
   `m_maxPendingInputs` (default 128) — oldest inputs are silently dropped under sustained
   server ACK starvation; 64 KB payload cap; `ReplayProtection` window is 256 sequences.
7. **MMO accounts remain RAM-only, but credential lifetime is tested.** `MMOAccountSystem`
   keeps accounts/sessions in `unordered_map`s and **clears them in `Shutdown()`** — no
   persistence hookup (routing: persistence design belongs to
   `sparkengine-persistence-save-and-migrations`). `Tests/CMakeLists.txt` now production-links
   `MMOAccountSystem.cpp`; `TestMMOCredentialSecurity.cpp` covers real registration/hash-only
   storage plus full-capacity secure erasure, while `TestPasswordHash.cpp` covers the KDF.
8. **RCON is in-process only.** `DedicatedServer` logs at startup that
   `rconPassword`/`rconPort` are "reserved but inactive; no remote RCON transport is enabled"
   (`DedicatedServer.cpp:75-78`). `ExecuteRcon` is a trusted local API; the old chat-`/command`
   bridge is deleted. Do not reintroduce any network→RCON path without adding real auth.
9. **Closed in-process plaintext-lifetime gap, with explicit limits.** `MMOLoginUI` owns the
   password in non-copyable `SensitiveCharBuffer<64>` and clears it on all state/submit/exit
   paths; account/KDF APIs borrow `string_view`, KDF intermediates are erased, and sensitive
   console history is redacted across unregister and Shutdown/Initialize. Caller-owned strings,
   ImGui/OS input state, allocator history, paging, crash dumps, clipboard/IME, and register
   spills remain outside what portable in-process erasure can guarantee.

## Decision rules

- **Adding a message type?** Add the enum value in `NetworkManager.h` (`MessageType : uint16_t`),
  register a `MessageSchema` in `PacketValidator` (min/max size, `requiresAuth`, direction),
  update `docs/specs/networking-wire-format.md`, and add an adversarial case to
  `Tests/TestPacketValidatorReal.cpp` or `TestNetworkStress.cpp`. Unregistered custom types from
  unauthenticated senders are rejected by design — do not "fix" that.
- **Hashing anything password-like?** Use `Spark::PasswordHash::Create/Verify`. Never
  hand-roll, never lower the 600k floor, never store salt separately.
- **Generating any token/nonce/key that matters?** Use `Spark::SecureRandom`. If `Fill`/`HexToken`
  fails, fail the operation — do not fall back to `std::mt19937`/`rand()`. If you touch
  `NetworkSecurity.h` or `NetworkEncryption.cpp`, migrating their RNG to `SecureRandom` is the
  correct move (they are currently unwired, so this is low-risk).
- **Wiring encryption for real?** That is a design task: replace XOR/HMAC-4 with a vetted AEAD,
  not a wiring-only change. Per the manifest's wiring rule ("wire it in or delete it"), the
  unwired `NetworkStack`/`NetworkEncryption` layers are standing exceptions — raise them in
  `sparkengine-change-control-and-release-readiness` before shipping claims.
- **Server-side validation of client input?** Reject non-finite floats at the boundary (follow
  the `LagCompensation.cpp:184` pattern), range-check enums before casting, and validate
  before acquiring long-held locks.
- **Accessing NetworkManager from a game module?** `context->GetNetworkService()` first;
  `NetworkManager::GetInstance()` only resolves correctly because it consults `EngineContext` —
  but in a statically-linked module image, prefer the context explicitly.

## Multiplayer test evidence

Registered evidence (all files are compiled into the single `SparkTests` binary;
`add_test(NAME SparkEngineTests COMMAND SparkTests)` in `Tests/CMakeLists.txt` registers it with
CTest; every `Test*.cpp` in `Tests/` is referenced by the CMakeLists — verified by scripted
cross-check at HEAD):

| Area | Files |
|---|---|
| Wire/adversarial ingress | `TestNetworkStress.cpp` (ConnectionFlood, InvalidMagic, TruncatedPackets, PayloadLengthOverflow, SpoofedSenderIDs, ReplayAttack, …), `TestPacketValidator{,Real}.cpp`, `TestNetworkManagerEdgeCases.cpp` |
| Core manager / integration | `TestNetworkManager{Real,Integration,Orchestration}.cpp`, `TestNetworkIntegration.cpp`, `TestNetworkStack.cpp`, `TestNetworkMMOIntegration.cpp` (handshake, chat broadcast, entity migration, full-stack stress) |
| Prediction / lag comp / interp | `TestClientPrediction.cpp`, `TestLagCompensation{,Integration}.cpp`, `TestNetworkInterpolation.cpp`, `TestNetQuantize.cpp`, `TestReplicationFields.cpp` |
| Crypto and credential lifetime | `TestPasswordHash.cpp`, `TestSecureRandom.cpp`, `TestMMOCredentialSecurity.cpp`, `TestSparkConsoleConcurrency.cpp`, `TestNetworkEncryption.cpp` (round-trip, tamper, replay window), `TestNetworkSecurity{,PhaseHH}.cpp` |
| Module protocols (TERRAFRONT) | `TestTFNetProtocolLayout.cpp` (wire freezes), `TestTFServerValidation.cpp` (movement clamp, fire origin, input-rate token bucket), `TestTFOnboarding.cpp` (accounts) |

Run them (repo root, after a configured build — see project `CLAUDE.md` for presets):

```bash
cmake --build --preset windows-release          # or your configured preset / plain build dir
ctest --test-dir build/windows-release -C Release --output-on-failure   # runs SparkEngineTests (everything)
```

Filter to one file's tests (env-var filter is built into the test runner; full selector
list: `sparkengine-validation-and-qa` §3). Binaries land in `<builddir>/bin/` —
`build/<preset>/bin/` for preset builds, `build/bin/` for a raw `cmake -B build` layout:

```bash
SPARK_TEST_FILE=TestNetworkStress ./build/linux-gcc-release/bin/SparkTests     # Linux preset build
# Windows (Git Bash): SPARK_TEST_FILE=TestNetworkStress ./build/windows-release/bin/SparkTests.exe
```

**The skip trap.** `TestNetworkMMOIntegration.cpp` (and siblings) compile to a single
always-passing `*_Skipped` test when `ENABLE_NETWORKING` is OFF. A green `ctest` proves nothing
about networking unless the build had networking on. Before trusting a pass, confirm the real
tests ran:

```bash
grep -o "ENABLE_NETWORKING[:=]*[A-Z]*" build/windows-release/CMakeCache.txt   # use your build dir
SPARK_TEST_FILE=TestNetworkMMOIntegration ./build/windows-release/bin/SparkTests 2>&1 | grep -c "MMOIntegration_"
# expect >1 test names, not just "MMOIntegration_Skipped"
```

## Failure modes

| Symptom | Likely cause | First check |
|---|---|---|
| "Invalid packet magic 0x…" warnings flood the log | Non-SparkEngine traffic on the port, or client/server built from different revisions (no version negotiation) | `git log --oneline -- SparkEngine/Source/Engine/Networking` on both builds |
| Client connects but sees no entities | Connect was rejected (server full) — since `8c096003` rejected clients get *nothing*, by design | Server log for "Connection rejected for pending client" |
| Custom module message silently dropped | Sender not authenticated; unregistered custom types require auth | `PacketValidator` stats via `GetPacketValidator()`, `rejectedUnauthenticated` counter |
| Login returns "Unable to create a secure session" | OS CSPRNG failure (`SecureRandom::Fill` false) — fail-closed working as intended | `TestSecureRandom` locally; check OS entropy facilities |
| Old password hashes stop verifying after engine update | Deliberate: `PasswordHash::Verify` rejects iterations outside 600k–1M and any non-`pbkdf2-sha256` scheme | Confirm stored hash prefix; re-register or migrate — do not widen the accept range |
| MMO module operates on a different NetworkManager than the host | Per-DLL static from the `GetInstance()` fallback | Resolve via `IEngineContext`; see gap #3 |
| Prediction rubber-bands under high latency | >128 unACKed inputs trimmed (`ClientPrediction.cpp:29`) | `GetPendingInputCount()`; raise `SetMaxPendingInputs` deliberately, don't remove the cap |
| Hitscan misses that "should" hit | Non-finite/zero-length ray or pose rejected by hardened `RaycastRewound` | Log inputs at the call site; the validation intentionally fails closed |
| All network tests "pass" instantly | `ENABLE_NETWORKING=OFF` skip trap | See verification commands above |

## Provenance and maintenance

Authored 2026-08-23 against the working tree of branch `claude/whole-nine-yards-20260823`
(uncommitted changes ahead of `0e1fe7e7`); all file:line anchors read from the working tree
that day. Line numbers drift — prefer the greps.

Re-verify the load-bearing claims:

```bash
# PBKDF2 work factor + bounds (expect 600000 / 600000 / 1000000)
grep -n "kIterations\|kMinimumIterations\|kMaximumIterations" SparkEngine/Source/Utils/PasswordHash.cpp

# CSPRNG backends
grep -n "BCryptGenRandom\|getrandom\|arc4random_buf" SparkEngine/Source/Utils/SecureRandom.cpp

# 128-bit session token + fail-closed login
grep -n "HexToken(16)\|Unable to create a secure session" GameModules/SparkGameMMO/Source/Account/MMOAccountSystem.cpp

# Weak RNG residue in unwired security layers (if these return nothing, gap #1a is fixed)
grep -n "mt19937" SparkEngine/Source/Engine/Networking/NetworkSecurity.h SparkEngine/Source/Engine/Networking/NetworkEncryption.cpp

# Encryption still unwired? (expect consumers only in NetworkIntegration.h and Tests/)
grep -rln "EncryptPacket\|NetworkSecurity" SparkEngine/Source SparkEditor/Source GameModules --include=*.cpp --include=*.h | grep -v Tests

# Fallback singleton still present
grep -n "static NetworkManager instance" SparkEngine/Source/Engine/Networking/NetworkManager.cpp

# Ingress hardening anchors
grep -n "0x5350524B\|MAX_NETWORK_MESSAGE_PAYLOAD_SIZE\|Invalid packet channel" SparkEngine/Source/Engine/Networking/NetworkManager.cpp
grep -n "MAX_UDP_WIRE_DATAGRAM_SIZE\|NETWORK_WIRE_HEADER_SIZE" SparkEngine/Source/Engine/Networking/NetworkWireLimits.h
grep -n "m_packetValidator.ValidatePacket" SparkEngine/Source/Engine/Networking/NetworkConnection.cpp

# Finite-input hardening anchors
grep -n "isfinite" SparkEngine/Source/Engine/Networking/LagCompensation.cpp SparkEngine/Source/Engine/Networking/WorldServer.cpp SparkEngine/Source/Engine/Networking/AreaServer.cpp

# MMO account/credential production linkage and tests
grep -n "MMOAccountSystem.cpp\|TestMMOCredentialSecurity" Tests/CMakeLists.txt

# TERRAFRONT's separate 150k iteration count (do not conflate with 600k)
grep -n "kPbkdf2Iterations" GameModules/SparkGameMMOFPS/Source/Account/TFAccountSystem.cpp

# Stub transport still a stub
grep -n "Steamworks SDK not linked" SparkEngine/Source/Engine/Networking/SteamTransport.h
```

If any grep's expectation breaks, update this skill in the same change — and if the fix
resolves an "open" item, record the resolving commit in `sparkengine-failure-archaeology`
rather than deleting history.
