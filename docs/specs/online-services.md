# SparkEngine Online-Services Boundary Specification

**Contract version:** 1.1  
**Date:** 2026-09-27  
**Status:** Normative boundary contract. The failure budgets in this document are requirements. Where the current code does not yet enforce a budget, the document says so.  
**Work item:** `NET-110` (outside the `stable-v1` profile). **Owner decision:** OD-08 in [`docs/readiness/OWNER-DECISIONS.md`](../readiness/OWNER-DECISIONS.md).  
**Capability row:** `services.production` in `docs/site/readiness.json` (`implementation: absent`, `support: unsupported`).

## 1. Purpose and scope

This specification separates the online-facing **engine SDK interfaces** that SparkEngine ships from the
**product-owned services** that a game must build, buy, or get from a platform holder. It names every trust and
ownership boundary between them, and for each call across a boundary it gives:

- the timeout, retry, and circuit-breaker budget
- the failure semantics
- the current adapter status

OD-08 applies throughout. Identity, matchmaking, fleet, entitlement, and billing services are out of engine scope, and
SparkEngine ships no hosted online services. None of the processes or adapters described here is production
infrastructure. The narrative companion page is [Online Service Boundary](../../wiki/advanced/Online-Service-Boundary.md),
and the interface reference is [Online Services](../../wiki/gameplay-tools/Online-Services.md).

The key words MUST, MUST NOT, SHOULD, and MAY are used as described in RFC 2119.

## 2. Ownership model

### 2.1 Engine SDK interfaces (shipped in this repository)

| Surface | Source | Role at the boundary | Status |
|---|---|---|---|
| `IOnlinePlatform` / `OnlineServiceManager` | `SparkEngine/Source/Engine/OnlineServices/OnlineServices.h` | Client-side seam that a platform or product adapter implements. The manager owns one active adapter and is initialized, ticked, and shut down by `GameplayLifecycleShared.cpp` | Interface only |
| `NetworkManager` / `ITransport` / `UDPTransport` | `SparkEngine/Source/Engine/Networking/` | Gameplay UDP transport, reliability, replication. The bind policy (`NetworkBindPolicy.h`) allows only loopback or a private LAN | Experimental. Protocol v2 (`NET-100`): server-authenticated X25519 handshake, every later datagram sealed (ChaCha20-Poly1305); not independently reviewed |
| `DedicatedServer` | `SparkEngine/Source/Engine/Networking/DedicatedServer.h` | Headless authoritative tick loop, map rotation, trusted in-process administration, LAN discovery | Development/reference |
| `SparkServer` | `SparkServer/src/` | Headless module host. Publishes a JSON health snapshot (`ServerHealth.h`) and hosts `LocalAreaControlService` | Development/reference |
| `SparkGateway` (`GatewayCoordinator`, `IGatewayAuthenticator`, `IAreaControlPlane`, `IAreaPlacementPolicy`) | `SparkGateway/src/` | Admission, placement, routing, and the fenced cross-area handoff control plane | Development/reference, owner-local only |
| `SparkDaemon` / `SparkOrchestrator` | `SparkDaemon/src/` | Supervises allowlisted processes on a single host | Development/reference, single host |
| `Spark::PasswordHash` | `SparkEngine/Source/Utils/PasswordHash.h` | PBKDF2-HMAC-SHA256 helpers for an account store that the product builds | Library helper |

### 2.2 Product-owned services (never shipped by the engine)

| Service | What the product owns | Engine seam it plugs into |
|---|---|---|
| Identity and accounts | Sign-up, login, recovery, credential storage, issuing admission credentials | `IGatewayAuthenticator::Authenticate`, `IOnlinePlatform::Login` |
| Matchmaking and lobbies | Queues, skill rating, parties, session discovery beyond the LAN | `IOnlinePlatform::FindSessions` / `CreateSession` / `JoinSession`, `IAreaPlacementPolicy::Place` (consulted by `GatewayCoordinator::Admit`) |
| Fleet | Provisioning, scaling, placement, regional routing, health-driven replacement | Reads `SparkServer` health snapshots and launches `SparkServer` / `SparkGateway` processes |
| Moderation and abuse | Reports, sanctions, chat filtering policy, ban lists | The authenticator rejects sanctioned principals. The server kicks through trusted in-process administration |
| Entitlement | Ownership checks and DLC grants | `IGatewayAuthenticator` (admission), plus product code in the game module |
| Billing | Store, payments, receipts, refunds, tax | None. No engine interface touches billing |
| Player data | Cloud saves, leaderboards, achievements, friends, presence | The matching `IOnlinePlatform` calls |
| Secrets and operations | Production key material, telemetry pipelines, backups, incident response | Key-file paths passed to `KeyFileAuthenticator` / `LocalAreaControlPlane` (OD-22: no production secret ships in engine configuration) |

## 3. Deployment diagram

```mermaid
flowchart LR
    subgraph ClientHost["Player device (untrusted)"]
        Game["Game client<br/>(game module + engine)"]
        OSM["OnlineServiceManager<br/>+ IOnlinePlatform adapter"]
        Game --> OSM
    end

    subgraph Product["Product-owned services (NOT engine, OD-08)"]
        Identity["Identity / accounts"]
        Match["Matchmaking / lobbies"]
        Fleet["Fleet control"]
        Moderation["Moderation / abuse"]
        Entitlement["Entitlement"]
        Billing["Billing"]
        PlayerData["Player data<br/>(cloud save, leaderboards, friends)"]
    end

    subgraph Vendor["Platform holder (NOT engine)"]
        PlatformSDK["Steamworks / EOS / console SDK + backend"]
    end

    subgraph ServerHost["Operator host (owner-local, same OS user)"]
        Gateway["SparkGateway<br/>GatewayCoordinator"]
        AreaA["SparkServer area A<br/>LocalAreaControlService"]
        AreaB["SparkServer area B<br/>LocalAreaControlService"]
        Daemon["SparkDaemon / SparkOrchestrator"]
        Health[("health snapshot files")]
    end

    OSM -- "B1: adapter calls" --> PlatformSDK
    OSM -- "B1: adapter calls" --> PlayerData
    Game -- "B2: credential request" --> Identity
    Identity -. "issues admission credential" .-> Game
    Game -- "B3: admission (credential)" --> Gateway
    Gateway -- "B4: Authenticate()" --> Identity
    Gateway -- "B4: Authenticate()" --> Entitlement
    Gateway -- "B4: Authenticate()" --> Moderation
    Game -- "B5: gameplay UDP (sealed, server-authenticated, loopback/LAN)" --> AreaA
    Gateway -- "B6: HMAC area control (named pipe / Unix socket)" --> AreaA
    Gateway -- "B6: HMAC area control" --> AreaB
    Daemon -- "B7: supervise (single host)" --> AreaA
    AreaA --> Health
    AreaB --> Health
    Fleet -- "B8: reads health, launches processes" --> Health
    Fleet -- "B8: launch / drain" --> Daemon
    Match -- "B9: placement" --> Gateway
    Billing -. "no engine interface" .-> Entitlement
```

The diagram draws the whole Product and Vendor subgraphs as external. The engine repository contains no code in
either subgraph. `NullOnlinePlatform` stands in for B1 during local development and does not contact either one.

## 4. Trust boundaries

| Id | Boundary | Crosses | Who is trusted | Current mechanism | Status |
|---|---|---|---|---|---|
| B1 | Game ↔ platform/product adapter | Process → vendor SDK or product API | The adapter. Its results are advisory client state and are never proof of identity to a server | Synchronous `IOnlinePlatform` calls on the game thread | Only `NullOnlinePlatform` (local, deterministic) and fail-closed stubs exist |
| B2 | Client ↔ identity service | Device → product | Product identity service | None in the engine | Product-owned |
| B3 | Client ↔ gateway admission | Untrusted network → `SparkGateway` | Nothing on the client side. The credential is opaque and bounded (`GatewayMaximumCredentialSize` = 512 bytes, body ≤ 4096 bytes) | `LocalGatewayIngressService` (owner-local named pipe) → `GatewayCoordinator::Admit` | Owner-local reference only. There is no internet-facing ingress |
| B4 | Gateway ↔ credential issuer | `SparkGateway` → product identity, entitlement, moderation | `IGatewayAuthenticator` implementation | `KeyFileAuthenticator`: `v1.<unix-ms>.<nonce>.<hmac-sha256>` with a 60 s replay window and a 4096-entry replay ledger. `LocalFixtureAuthenticator` answers identity, entitlement and moderation from a bounded principal fixture | The local reference and the local, deterministic stand-in are complete. Product adapters are product-owned |
| B5 | Client ↔ gameplay server | Untrusted network → `SparkServer` / `DedicatedServer` | Server is authoritative. Client input is validated (`PacketValidator`) | `NetworkManager` UDP protocol v2: the client authenticates the server's identity key and every post-handshake datagram is sealed; players authenticate inside the channel. The bind policy allows loopback or a private LAN only | Experimental. SparkGateway tickets are not yet bound to the UDP session (decided: presented in the sealed `ClientFinished`); `NET-100` stays open for that and for independent review |
| B6 | Gateway ↔ area servers | Process ↔ process on one host | Same operating-system user, holder of the area-control key | HMAC-SHA256 frames, ±60 s timestamp window, nonce replay ledger, per-session epoch fence persisted to an epoch-state file, one audit record per frame | Owner-local reference |
| B7 | Supervisor ↔ servers | Process ↔ process on one host | Same host operator | `SparkDaemon` orchestration allowlist | Single host only. Not a fleet |
| B8 | Fleet ↔ servers | Product control plane → operator hosts | Product fleet | Reads `SparkServer` health JSON, launches or drains processes | Product-owned. The engine provides only the health snapshot and `GatewayCoordinator::BeginDrain`. The local stand-in is `SparkDaemon` supervision plus `BeginDrain`, with no new code |
| B9 | Matchmaking ↔ gateway placement | Product → `SparkGateway` | Product matchmaker, through the `IAreaPlacementPolicy` it gives `GatewayCoordinator` | `Admit` asks the placement policy for an area and accepts only a registered, online area below its `maxClients`. The default policy is `LocalDeterministicPlacement` | Product-owned. The local, deterministic stand-in is complete |

Invariants that hold across every boundary:

1. **No client-asserted identity.** A server trusts a principal only through `AuthenticationResult::principalId`
   from the configured `IGatewayAuthenticator`. `IOnlinePlatform::GetLocalPlayer()` is client state and MUST NOT be
   used for server-side authorization. `NullOnlinePlatform::Login` accepts any username and ignores the token, so it
   MUST NOT back an admission decision.
2. **Credentials are never logged.** `IGatewayAuthenticator::Authenticate` implementations MUST NOT log the
   credential. Credential-bearing gameplay messages use `NetworkManager::RegisterSensitiveHandler`.
3. **Source authority until acknowledgement.** During a cross-area handoff the source area stays authoritative until
   the `Acknowledge` phase applies. Every failure resolves through an explicit `Abort` before a new epoch starts.
4. **No production secrets in engine configuration** (OD-22). Key files are operator-provisioned, and the loader
   rejects files that other users can read.

## 5. Call budgets and failure semantics

### 5.1 `IOnlinePlatform` (boundary B1)

The interface is synchronous and is called from the game thread. The contract for every adapter is:

| Budget | Requirement | Enforced today |
|---|---|---|
| Per-call time on the game thread | A call MUST return within **5 ms**. An adapter that talks to a remote backend MUST do the network work off the game thread, and it MUST return cached state or queue the request. It drains completions in `OnlineServiceManager::Update` | Measured, not preempted. `GuardedOnlinePlatform` times every capability call that reaches the adapter against `OnlineCircuitPolicy::callBudget` (5 ms). It counts over-budget calls per capability with the slowest call time, logs one warning per capability at most every 10 s, and reports them in `Console_GetStatus()` (`Budget: ...`). With `tripOnBudgetOverrun` set, an over-budget call also counts as a failure of its capability and feeds the circuit breaker, so a stalling backend is cut off after 5 slow calls. The slow call itself cannot be interrupted, and its result is still returned. Tested by `OnlineServices_Degraded_*` |
| Remote request timeout (adapter-internal) | **10 s** per remote request, then the operation fails | Adapter responsibility. None exists in the tree |
| Retries | Idempotent reads (`FindSessions`, `QueryScores`, `QueryAchievements`, `ListCloudSaves`, `LoadFromCloud`, `GetFriendsList`) MAY retry at most **2** times with exponential backoff starting at **500 ms** and capped at **4 s**. Mutations (`Login`, `CreateSession`, `JoinSession`, `SubmitScore`, `UnlockAchievement`, `SetAchievementProgress`, `SaveToCloud`, `DeleteCloudSave`, `SetPresence`, `InviteToSession`) MUST NOT retry automatically unless the backend deduplicates them | Adapter responsibility |
| Circuit breaker | After **5** consecutive failures of one capability, calls to that capability fail immediately for **30 s**, then one probe call is allowed | Yes, for every adapter installed with `SetPlatform()`. `OnlineServiceManager::GetPlatform()` returns `GuardedOnlinePlatform`, which counts failures per capability and runs the cooldown on the `Update()` clock. A probe that fails reopens the circuit. `Logout()` and `LeaveSession()` always reach the adapter. The circuit is disabled for the in-process `NullOnlinePlatform`: it has no remote dependency, so its failures are caller errors, and they are only counted. Tested by `OnlineServices_Degraded_*` |

Failure semantics that every adapter MUST follow. The `OnlineServices_Contract_*` conformance suite (section 8) checks
the first three rules on every shipped adapter: fail-closed capabilities, a failure reason that is present and never
echoes the login token, and `Logout()` / `LeaveSession()` in any state. On the Null adapter it also checks that each
failed call sets its own exact reason and each successful call clears it. The stubs return one constant
`GetLastError()` string, so for them the suite checks only that the reason is present. The suite does not test the
no-throw and no-local-corruption rules:

- A capability that `GetCapabilities()` reports as `false` MUST fail every call. Mutations return `false`, queries
  return an empty value, and no call may fabricate success.
- Every failed call MUST leave a non-empty, human-readable, secret-free `GetLastError()`.
- `Logout()` and `LeaveSession()` MUST be safe to call in any state, including when no session exists.
- An adapter MUST NOT throw out of an interface call. `GuardedOnlinePlatform` contains any exception that an
  adapter throws anyway, so it never reaches a caller that goes through `GetPlatform()`. The call fails, it counts
  as a failure of its capability, and `GetLastError()` reports `<call> failed: adapter threw: <what>`. A login token in
  the exception text is replaced with `<redacted>`.
- A failure MUST NOT corrupt local game state. Callers treat online results as optional. Local saves go through the
  [Save System](../../wiki/gameplay-tools/Save-System.md), not through `SaveToCloud`.

Observability: `OnlineServiceManager::Console_GetStatus()` reports the active adapter name, whether it has any
capability, the last error, per-capability health, and the logged-in display name. The health field is `ok`, or it lists
each capability that has failed since its last success, with its consecutive-failure count and circuit state (for example
`leaderboards 5 consecutive failures (circuit open, retry in 30.0s)`). `GetCapabilityHealth()` returns the consecutive,
total and rejected-call counters for one capability. A mutation counts as failed when it returns `false`. A query counts as
failed when it throws, or when it returns nothing and the adapter reports a `GetLastError()` reason for that call. An empty
result with no reason, such as an empty leaderboard, is a success. Opening and closing a circuit is logged. The budget
field is `ok`, or it lists each capability with over-budget calls (for example
`leaderboards 2 calls over 5.0 ms (max 31.4 ms)`); `GetCapabilityHealth()` also returns `budgetOverruns` and
`maxCallMicroseconds`. In a running engine the `online_status` console command (registered by
`RegisterEngineConsoleCommands()`) prints this status.

### 5.2 Gateway admission, authentication and placement (boundaries B3, B4, B9)

| Call | Budget | Failure semantics |
|---|---|---|
| `GatewayCoordinator::Admit` | Bounded input: body ≤ 4096 bytes, credential ≤ 512 bytes. Local ingress I/O deadline **2 s** per frame | Fails closed with a `RouteFailure` (`NotReady`, `InvalidRequest`, `AuthenticationFailed`, `DuplicateSession`, `CapacityReached`, `NoAreaAvailable`). Rejected requests create no session. `NoAreaAvailable` also covers a placement the coordinator refuses (section 5.2, `IAreaPlacementPolicy::Place`). `BeginDrain` rejects new admissions |
| `IGatewayAuthenticator::Authenticate` | MUST be thread-safe and MUST complete within **2 s**, the ingress deadline. A product authenticator that calls a remote identity service MUST NOT retry inside that window, and it SHOULD open a circuit (reject immediately with a reason) after **5** consecutive failures of the backend for **30 s** | Any failure returns `accepted = false` with a `reason`. An unready authenticator (`IsReady() == false`) makes the coordinator not ready, so every admission is rejected. `GatewayCoordinator` wraps every authenticator in `GuardedGatewayAuthenticator`, which enforces these budgets for any adapter: an exception becomes the rejection `Authentication backend fault` (the exception text, which may hold the credential, is never surfaced or logged); a call over the 2 s budget is rejected even if the adapter accepted it; exceptions and overruns are faults, and 5 consecutive faults open a 30 s circuit that rejects with `Authentication backend unavailable (circuit open)` without calling the adapter, followed by one probe. A rejection is a healthy answer and resets the fault streak. A reason that echoes the credential is redacted. `GatewayCoordinator::GetAuthenticationHealth()` returns the accepted, rejected, fault, overrun and fail-fast counters, and opening or closing the circuit is logged. `SparkGateway` publishes them, with the circuit state and slowest call, as the `authentication` object of its health JSON (`--health-file` and the status line), and `GatewayCoordinator::IsReady()` (the health `ready` field) is false while the open circuit is failing admissions fast. It turns true again when the cooldown ends, so the probe admission can arrive. Tested by `SparkGateway_GuardedAuthenticator_*` |
| `KeyFileAuthenticator` | Timestamp within the 60 s replay window. Replay ledger bounded at 4096 entries | Wrong MAC, a stale or future timestamp, or a replayed nonce is rejected. A full ledger fails closed |
| `LocalFixtureAuthenticator` | Fixture read once at startup: at most 64 KiB, strict JSON (`Json::ParseBounded`, depth 4, repeated keys rejected). One in-memory lookup per call, no clock, no randomness, no I/O | A malformed, oversized or inconsistent fixture (unknown key, repeated credential or principal, empty or non-printable field, `moderation` other than `none` / `banned`) leaves the authenticator not ready, so the gateway admits nobody. Rejections use fixed reasons, checked in this order: `Unknown credential`, `Principal is banned`, `Principal is not entitled`. Neither the load errors nor the reasons contain a credential. `SparkGateway --admission-fixture <path>` (or `[Security] admission_fixture`) selects it instead of `KeyFileAuthenticator` |
| `IAreaPlacementPolicy::Place` (boundary B9) | Called under the coordinator lock on the transport thread, so it MUST NOT block or call back into the coordinator. It receives the authenticated principal, the session id, the spawn position and every registered area with its online flag, session count and `maxClients`. It never receives the credential | `GatewayCoordinator` accepts the answer only if it is a registered, online area below capacity, and otherwise fails the admission with `NoAreaAvailable`. An exception from the policy is contained as `NoAreaAvailable` with the reason `Area placement policy fault`. The default `LocalDeterministicPlacement` picks the area with the fewest sessions and breaks ties by the lowest `AreaID`, whatever order the areas are listed in |

### 5.3 Area control plane (boundary B6)

| Call | Budget | Failure semantics |
|---|---|---|
| `IAreaControlPlane::Prepare` / `Transfer` / `Commit` / `Acknowledge` / `Abort` | One request per connection. Local I/O deadline **2 s** (`LocalIoTimeout`). Frame timestamp window ±60 s | Returns `Applied`, `Duplicate` (idempotent replay of the same `(sessionId, epoch, phase)`), `Rejected`, or `Unavailable`. The coordinator maps `Unavailable` to `WaitingForRetry` and keeps the phase. `Rejected` moves the session to `Aborting` |
| Retry policy | The caller of `GatewayCoordinator::AdvanceHandoff` owns retries, and every phase is idempotent, so a retry is always safe. Nothing in the engine retries automatically today. A product driver SHOULD retry `Unavailable` at most **3** times with backoff starting at **100 ms**, and then drive `Abort` | Stale epochs return `StaleEpoch`. The epoch fence persists across `SparkServer` restarts |
| `LocalAreaControlService::Start` | Listener startup deadline **5 s** | Start fails with `GetLastError()` set. The server is not ready |
| Observability | One audit record per frame (`AreaControlAuditReason`) plus per-reason counters (`GetAuditCount`) | Secret-free. Session identifiers are sanitized |

### 5.4 Gameplay transport and supervision (boundaries B5, B7, B8)

| Surface | Budget | Failure semantics |
|---|---|---|
| `NetworkManager` connection | Client considered timed out after **10 s** of silence (`m_connectionTimeout`). Adaptive retransmit timeout for reliable messages | `SetTimeoutHandler` callback. Reliable messages retransmit under the RTO |
| `DedicatedServer` | Kick after `ServerConfig::clientTimeoutSeconds` (default **30 s**). LAN discovery listens for `timeoutMs` (default **2000 ms**) | Graceful `Stop()` notifies clients and flushes |
| `SparkServer` health | Periodic JSON snapshot with build identity, tick latency, and memory | A missing or stale snapshot means the server is unhealthy. The fleet (product) decides what to replace |
| Version compatibility | The gateway protocol is `GatewayProtocolMajor.Minor` = 1.0. `NetworkManager` negotiates `NETWORK_PROTOCOL_VERSION` (2) inside the signed handshake | A mismatched gameplay build gets a typed `ProtocolTooOld`/`ProtocolTooNew`/`ProtocolMismatch` refusal; a version-1 client gets an unframed `ProtocolTooOld` |

## 6. Adapter status register

No adapter in this repository is production. The labels below are the only permitted descriptions.

| Adapter | Boundary | Label | Notes |
|---|---|---|---|
| `NullOnlinePlatform` | B1 | **local, deterministic** | In-process memory only. Nothing persists or leaves the process. Accepts any login. Results come from ordered containers, so a fixed call sequence gives a fixed result on every standard library. It has no friends list, so `InviteToSession()` always fails with a reason |
| `SteamPlatform` | B1 | **stub** | Reports no capabilities and fails every call. `GetLastError()` = "Steamworks SDK unavailable in this build" |
| `EpicPlatform` | B1 | **stub** | Reports no capabilities and fails every call. `GetLastError()` = "EOS SDK unavailable in this build" |
| `ConsolePlatform` | B1 | **stub** | Reports no capabilities and fails every call. The console SDKs are NDA-gated |
| `SteamTransport` | B5 | **stub** | `Send` / `Receive` fail with "transport unavailable (Steamworks SDK not linked)" |
| `UDPTransport` | B5 | **local/LAN, experimental** | Loopback or private LAN only; NET-100 v2 sealed transport, not independently reviewed |
| `KeyFileAuthenticator` | B4 | **local reference** | Owner-local key file. Not an identity service |
| `LocalFixtureAuthenticator` | B4 | **local, deterministic** | Stands in for identity, entitlement and moderation from a bounded principal fixture file. Not a secret store and not an identity service |
| `LocalDeterministicPlacement` | B9 | **local, deterministic** | The default `IAreaPlacementPolicy`: fewest sessions, then lowest `AreaID`. Stands in for a matchmaker. It is not a matchmaking service |
| `LocalAreaControlPlane` / `LocalAreaControlService` | B6 | **local reference** | Same host, same OS user |
| `LocalGatewayIngressService` | B3 | **local reference** | Owner-local named pipe, not internet-facing |
| `SparkDaemon` orchestration | B7 | **local reference** | Single host |
| `GuardedGatewayAuthenticator` | B4 | **engine guard** | Not an adapter. `GatewayCoordinator` installs it around whichever authenticator it is given, to enforce the section 5.2 budgets |
| `GuardedOnlinePlatform` | B1 | **engine guard** | Not an adapter. `OnlineServiceManager::GetPlatform()` returns it in front of whichever adapter is active, to enforce the section 5.1 failure semantics |

A new adapter is labeled **production** only when all of these hold:

1. It lives in a product or integration layer, not in this repository (OD-08).
2. It passes the `OnlineServices_Contract_*` conformance suite.
3. Its degraded-dependency behaviour is covered by `OnlineServices_Degraded` tests.
4. Its budgets in section 5 are measured against the real backend.

Production adapter admission checklist. The engine guards, not the adapter, make a failing adapter safe and observable,
so a product adapter is admitted only through them:

- An `IOnlinePlatform` adapter is installed with `OnlineServiceManager::SetPlatform()` and called only through
  `GetPlatform()`, so `GuardedOnlinePlatform` contains its exceptions, runs its circuit breaker and measures its
  5 ms budget. The product sets `tripOnBudgetOverrun` when a stalling backend must be cut off rather than only reported.
- An `IGatewayAuthenticator` adapter is passed to `GatewayCoordinator` (directly or through `GatewayApplication`), so
  `GuardedGatewayAuthenticator` contains its exceptions, rejects over-budget answers and runs its circuit breaker.
- The adapter passes `OnlineServices_Contract_*` (added as one more `RunOnlinePlatformContract<Adapter>()` case) and
  the product runs the `OnlineServices_Degraded_*` and `SparkGateway_GuardedAuthenticator_*` scenarios against it
  with its real backend stopped, stalled and throwing.
- `Console_GetStatus()` (Health and Budget fields) and the gateway health JSON `authentication` object are wired into
  the product's telemetry before the adapter takes production traffic.

### 6.1 Local deterministic stack

OD-08 keeps identity, matchmaking, fleet, entitlement and moderation out of engine scope, so the engine runs locally
by standing a local, deterministic adapter in at each engine seam those services plug into:

- B1 (platform services): `NullOnlinePlatform`.
- B2 and B4 (identity, entitlement, moderation): `LocalFixtureAuthenticator`, selected with
  `SparkGateway --admission-fixture <path>`.
- B9 (matchmaking): `LocalDeterministicPlacement`, the policy `GatewayCoordinator` uses unless a product policy is
  passed to it.
- B8 (fleet): `SparkDaemon` supervision on one host plus `GatewayCoordinator::BeginDrain`. No other code stands in
  for a fleet.
- Administration (acting on a sanction, such as a kick): the trusted in-process administration of `DedicatedServer`,
  which is already local. A ban takes effect at the next admission through the fixture's `moderation` field.

None of these reads a clock or a random source for its decisions, and `WorldServer` allocates area ids in
registration order, so a fixed admission script on a fresh stack gives a byte-identical result. The
`OnlineServicesLocalStack` CTest runs that script twice and compares both runs with one fixed transcript (section 8).

## 7. Versioning

This document is contract version 1.1. Version 1.1 added the `IAreaPlacementPolicy` placement seam, per-area
capacity at admission, and the local, deterministic adapters of section 6.1. A change that alters a budget, a failure result, or a boundary in a way an
existing adapter could observe bumps the major version. Additive clarifications bump the minor version. Changes to
`IOnlinePlatform`, `IGatewayAuthenticator`, or `IAreaControlPlane` virtual signatures MUST update this document in the
same change.

## 8. Verification and open work

| Requirement | Evidence today | Open |
|---|---|---|
| No hosted-service claim on public surfaces | `python3 tools/site-data/validate.py` (`validate_online_service_boundary`) governs this document, the two wiki pages, `docs/site/readiness.json`, and every public claim surface. `Tests/Tools/test_site_data_contract.py` `OnlineServiceBoundaryTests` | none |
| No local store, demo service or reference process marketed as production | `local_store_production_claim_errors` rejects any non-negated sentence or table row that pairs a production term (`production` followed by ready, grade, quality, infrastructure, database, backend, service or server; scalable backend, enterprise-grade, battle-tested) with a local store or reference process (`TFDatabase`, `TFWorldSave`, `TFOutfitStore`, JSON stores, MMO persistence, `AsyncDatabase`, `NullOnlinePlatform`, `KeyFileAuthenticator`, `SparkGateway`, `SparkDaemon`, demo servers). It scans tracked Markdown under `wiki/` and `docs/` (except the readiness ledger and handoff, which quote the criterion), `README.md`, module READMEs and DESIGN documents, `module.json` descriptions and `docs/site/*.json`. `adapter_name_production_errors` rejects an adapter whose `GetPlatformName()` or `GetLastError()` literal says production | none |
| This document names every boundary and adapter | `validate_online_service_boundary` also runs `online_service_spec_contract_errors`: section 3 must hold a Mermaid diagram whose `B<n>` edge labels equal the section 4 rows, which run contiguously from B1 and each name who is trusted and the enforcing mechanism. Every section 2.1 symbol must still occur in the source path the row names, and every tracked class outside `Tests/` that implements `IOnlinePlatform`, `IGatewayAuthenticator`, `IAreaControlPlane` or `ITransport` must be in the section 6 register with a permitted label. `OnlineServiceBoundaryTests` covers each rejection | none |
| Null adapter deterministic, stubs fail closed | `Tests/TestOnlineServices.cpp` `OnlineServices_Null*` and stub tests. The `OnlineServices_Contract_*` conformance suite (ctest `OnlineServicesContract`, label `online-services`, exact count 5) runs the section 5.1 failure semantics and the section 6 labels against `NullOnlinePlatform`, `SteamPlatform`, `EpicPlatform` and `ConsolePlatform`, checks `Console_GetStatus()` for each, and compares two fresh runs of the Null adapter against one fixed expected transcript. Friends and presence are checked for success and failure only: the Null adapter has no friends to read back and `SetPresence()` has no getter | A new adapter must be added to the suite before it is shipped |
| Degraded-dependency budgets (section 5.1 circuit breaker) | `Tests/TestOnlineServices.cpp` `OnlineServices_Degraded_*` (ctest `OnlineServicesDegraded`, label `online-services`, exact count 11) drives a fault-injecting adapter through `SetPlatform()` / `GetPlatform()`. It covers exception containment and token redaction (including a one-character token), opening after 5 consecutive failures, fail-fast without reaching the adapter, per-capability isolation, the probe after the cooldown, reset on success and on platform change, the Logout bypass, the Null-adapter exemption, budget-overrun counting with a rate-limited warning, the trip-on-overrun circuit, the `Console_GetStatus()` health and budget fields, and the `online_status` console command that prints them. `Tests/TestSparkGatewayCoordinator.cpp` `SparkGateway_GuardedAuthenticator_*` (ctest `GatewayGuardedAuthenticator`, labels `online-services;gateway`, exact count 7) covers the admission-side guard, and its `SparkGateway_GuardedAuthenticator_GatewayHealthReportsCircuit` case in `Tests/TestGatewayAreaControl.cpp` starts a real `GatewayApplication`, opens the circuit through its ingress and asserts the counters and `ready: false` in the health JSON | A slow call is measured, not preempted. The 10 s remote timeout and retry budgets remain adapter responsibilities with no production adapter to measure |
| Engine runs locally with deterministic adapters (section 6.1) | `Tests/TestOnlineServicesLocalStack.cpp` `OnlineServices_LocalStack_*` (ctest `OnlineServicesLocalStack`, labels `online-services;gateway`, exact count 6). A `GatewayCoordinator` + `WorldServer` + `NullOnlinePlatform` stack with `LocalFixtureAuthenticator` and `LocalDeterministicPlacement` runs one admission script (accepts, unknown, banned and unentitled rejections, every area full, a disconnect, drain) twice from fresh state, and both transcripts must equal one fixed expected transcript. The suite also checks that no credential reaches a log or a reason, that a placement tie resolves to the lowest `AreaID` in any input order, that the coordinator refuses an unknown, full or throwing placement, that 14 kinds of oversized or malformed fixture fail closed, and that `SparkGateway` options select the fixture authenticator | none for the local stack. Product adapters are product-owned (OD-08) |
| Versioned client/server compatibility | Gateway protocol constants; `NETWORK_PROTOCOL_VERSION` 2 bound into the handshake transcript | `SessionCompatibility_*` (CTest `NetworkSessionCompatibility`) |
| Hosted CI | none | `service-contract` and `network-integration` jobs (planned) |

## Source & Freshness

Written 2026-09-25 for `NET-110` from `OnlineServices.h`, `SparkGateway/src/GatewayCoordinator.h`,
`GatewaySecurity.h`, `GatewayAreaControl.h/.cpp`, `NetworkManager.h`, `DedicatedServer.h`, `NetworkBindPolicy.h`,
`SteamTransport.h`, and `SparkServer/src/ServerHealth.h` at that date. Updated 2026-09-27 for the placement seam and
the local, deterministic adapters in `SparkGateway/src/GatewayLocalAdapters.h`. Update it when any of those interfaces, the
budgets above, or owner decision OD-08 change.
