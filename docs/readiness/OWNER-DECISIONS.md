# Release-Readiness Owner Decisions

This file records engineering and product decisions the readiness ledger could
not make on its own. Legal and governance decisions (licensing, trademark,
contribution terms, security contact, support policy, Code of Conduct) are not
here; they stay open in [`docs/governance/GOV-400-DECISIONS.md`](../governance/GOV-400-DECISIONS.md).

A decision here unblocks implementation. It does not close a work item or
promote a capability: closure still needs the item's commands, tests, and
exact-SHA CI evidence (see `statusPromotionRules` in
[`docs/site/readiness.json`](../site/readiness.json)).

## 2026-09-24 decisions

Approved by the project owner (Krilliac) in the release-readiness session on
2026-09-24 ("defaults OK"), accepting the proposed default for each row.

| ID | Work item | Decision | Consequence for implementation |
|---|---|---|---|
| OD-01 | LIFE-200 | Delete `EngineContext::InitializeAll` / `ShutdownAll`. `EngineRuntime` stays the single owner of subsystem lifecycle. | Remove the unused entry points and any tests that only exercise them; lifecycle evidence targets the `EngineRuntime` startup/shutdown path. |
| OD-02 | SDK-240 | stable-v1 module ABI is exact-match only. N-1 module loading is not supported. | Keep fail-closed ABI rejection; diagnostics must name both versions. No N-1 load or migration path is built. |
| OD-03 | SAVE-230 | Saves and scenes: read the current and the previous (N-1) schema version, write only the current one. Each game module declares its own persisted-data schema version. | Loaders accept N and N-1 with tested migration; anything older or newer fails closed with a versioned error. |
| OD-04 | BLD-100 | stable-v1 CPU floor is x86-64 with SSE4.2. AVX2 is not required. | Shipping configurations must not require AVX2; the vendored Jolt build options must match the SSE4.2 floor. |
| OD-05 | SEC-100 | Remote administration stays permanently unavailable in stable-v1. No authenticated remote-admin channel is built for it. | Remote-admin surfaces stay disabled and fail closed; public wording keeps it labeled unavailable. |
| OD-06 | NET-100 | Replace in-tree transport cryptography with a maintained, reviewed library: libsodium. | Vendoring libsodium needs a pinned source, supply-chain lock entry, and notices update; independent review is still required. |
| OD-07 | DATA-120 | SQLite remains the production persistence adapter. | The encryption-at-rest and secret owner is still unassigned; this decision does not cover it. |
| OD-08 | NET-110 | Identity, matchmaking, fleet, entitlement and billing services are out of engine scope. | Document the service boundary; the engine ships no hosted online services. |
| OD-09 | RDY-020 | The TERRAFRONT assets without recorded provenance (`NOASSERTION`) are excluded from the stable-v1 package. | Packaging and the stable-v1 asset manifest must not include them; their provenance stays an open question outside stable-v1. |
| OD-10 | PLT-210 | Linux support row is Ubuntu 24.04 LTS, x86-64 only. | Other distributions and ARM64 are unsupported for stable-v1. |
| OD-11 | PLT-220 | macOS is deferred from stable-v1. If pursued later, Apple Silicon only. | macOS stays experimental; no macOS certification work for stable-v1. |
| OD-12 | PLT-230, PLT-240, PLT-250 | Mobile, OpenXR and console support are deferred from stable-v1. | These items record a deferral; no platform work is scheduled for stable-v1. |
| OD-13 | ENG-200 | ENG-200 is scheduled after the stable-v1 items. | No stable-v1 capacity is assigned to it. |
| OD-14 | MOD-315 | FPS multiplayer shares the multiplayer primitives used by SparkGameMMOFPS instead of keeping a separate `FPSMultiplayerSystem` implementation. | Consolidate onto the shared primitives; remove the duplicate once parity is proven. |
| OD-15 | MOD-380 | Ghost/replay persistence is out of scope for the Racing slice. | The Racing completion criteria do not require persisted ghosts or replays. |
| OD-16 | TF-120 | TERRAFRONT map hops adopt the SparkGateway/SparkServer fenced handoff plane. Reconnect-based hops are not kept as the production path. | Multimap migration work targets the fenced handoff. |

## Still open

These need an explicit owner answer; "defaults OK" did not cover them because
no default was proposed:

- REL-100: retention and support periods for the stable, nightly and experimental channels.
- REL-191: the exact v0.9.0 predecessor baseline commit, and whether REL-190 is required or replaced by REL-191 in the predecessor stage.
- MOD-330: ARPG actor and frame budget values.
- SEC-120: classification of the 149 deferred parser candidates (a drafted classification can be proposed for review).
- DATA-120: owner for encryption at rest and database secrets.
- All GOV-400 decisions D1-D8, named reviewers and sign-offs, and credentials that only the owner can provision (code signing, Apple Developer ID, release immutability, Sites deployment).
