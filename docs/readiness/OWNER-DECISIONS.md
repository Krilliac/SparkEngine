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

## 2026-09-24 delegated decisions

The owner delegated the remaining questions below ("whatever and however is
best for each item"). Each choice is made to fit the contracts already in the
repository and is recorded with its reasoning so it can be revisited.

| ID | Work item | Decision | Reasoning / consequence |
|---|---|---|---|
| OD-17 | REL-100 | **Stable:** release assets are immutable and kept permanently. Each stable release gets security and critical fixes until 6 months after the next stable release. **Nightly:** each uniquely tagged immutable nightly is kept for 30 days and is unsupported. **Experimental:** artifacts are kept for 14 days, always labeled experimental, and never presented as supported. CI build artifacts keep their existing 7- and 90-day retention. | The support window matches OD-03 (read N-1): a user on the previous stable can still load current data. Short, unsupported nightly and experimental windows keep storage bounded and stop them being mistaken for supported releases. |
| OD-18 | REL-191 / REL-190 | In the v0.9.0 predecessor stage, REL-191 replaces REL-190 (added to `predecessorRelease.qualificationSubstitutions`). REL-190 stays required for the stable-v1 release candidate. | REL-191 already covers every common qualification gate and named sign-off without N-1 claims, and the contract already substitutes REL-192→REL-191 and INST-131→INST-132 there. Requiring REL-190 too would demand N-1 evidence the predecessor cannot have. |
| OD-19 | REL-191 | The predecessor baseline is not picked by hand now. It is the first `Working` commit after the stable-v1 release branch merges at which every `predecessorRelease.requiredGateIds` gate has passing exact-SHA evidence. Krilliac is the predecessor owner; that SHA, its review, and the sign-off are recorded in `predecessorRelease` when it qualifies. | No commit qualifies today (every gate is blocked), so any SHA picked now would be a claim without evidence. A rule keeps the choice evidence-driven. |
| OD-20 | MOD-330 | ARPG dungeon slice budgets: at most 48 simultaneously active monster actors, at most 256 live gameplay entities during combat (hero, monsters, projectiles, loot), 16.67 ms p95 frame time (60 fps) on the certified Windows D3D11 row, and at most 3.0 ms p95 game-thread time for ARPG gameplay systems (AI, combat, skills, loot). | These are target values. They are verified only when PERF-100's certified hardware row and baselines exist; until then they are `pending_measurement` and cannot be claimed as met. |
| OD-21 | SEC-120 | The 149 deferred parser candidates are classified by evidence, one entry per file, using fixed rules: (1) code that parses bytes from outside the process (files a user or mod can supply, packages, network, save/scene/config files read at runtime) is an inventoried boundary and needs a fuzz target; (2) code that only parses data the engine itself generated in the same process, or build-time/developer tooling that never ships, is exempt with a written justification; (3) generic read/tokenize helpers are helper-exempt, and every parser that calls them is classified on its own. When in doubt, classify as a boundary. | Rules are fail-closed toward fuzzing. Classifications go through `tools/fuzz-policy/parser-inventory.json` with the reason recorded per file, reviewed like any other change. |
| OD-22 | DATA-120 | Krilliac is the accountable owner for encryption at rest and database secrets. stable-v1 policy: no database secret or credential is committed or written to shipped config; secrets come from the environment or the OS credential store. Password material is stored only as salted PBKDF2 hashes, and session tokens are never persisted in plaintext. Encryption at rest for SQLite files relies on host full-disk encryption, documented as the operator's responsibility; no in-database encryption (e.g. SQLCipher) for stable-v1. | This keeps OD-07 (SQLite) without adding a new crypto dependency, and puts the one real requirement (no plaintext secrets or tokens) under test instead of promising database-level encryption. |

## Still open

- All GOV-400 decisions D1-D8.
- Named independent reviewers and sign-offs (security, crypto, privacy/retention, performance budgets, module parity scores, release qualification).
- Credentials and infrastructure only the owner can provision: code signing, Apple Developer ID, release immutability policy, Sites deployment, OSV vulnerability-data access, console agreements.
