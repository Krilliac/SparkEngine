# SparkEngine — Multi-Process / Clustering Roadmap

Date: 2026-07-05. Captured while the editor ECS round-trip (sub-project 1) was in flight.
Status: **backlog** — not scheduled except where noted. Each item gets its own brainstorm → spec → plan when picked up.

> **Historical planning snapshot.** The external-service program described here
> subsequently shipped `SparkServer`, `SparkGateway`, `SparkOrchestrator`,
> `SparkCollabServer`, `SparkCooker`, `SparkWorker`, and `SparkAutomation`. See
> [External Services and Orchestration](../guides/External-Services-and-Orchestration.md)
> and [Offline Cooking and Runtime Automation](../guides/Offline-Cooking-and-Automation.md)
> for the current contracts and validation surface. The remaining aspirational
> items below are retained as design history, not as a statement of current
> implementation status.

## Existing multi-process substrate (the bones already present)
- Engine networking: `SparkEngine/Source/Engine/Networking/WorldServer.h` + `AreaServer.h` — a realmd→worldd→map-server hierarchy, already abstracted. TERRAFRONT boots **one** `AreaServer` per continent today.
- Standalone exes: `SparkDaemon`, `SparkConsole`, `SparkShaderCompiler`, `SparkBuild`, `SparkLauncher`, `SparkCrashReporter`, `SparkInstaller` (each its own `add_executable`).
- `WorldOriginSystem` origin-rebase (the primitive for seamless cross-node handoff).
- A live-edit / collaboration seed in the test suite (`LiveEditBridge`, `CollabEdit_HostAndConnect`).

## DESIGNATED NEXT (after / alongside the editor round-trip)
### Editor collaboration server (multi-user scene editing) — highest synergy
Reuses the **reflected-scene serializer** built in sub-project 1 as the on-the-wire format: a co-edit session is a stream of component-field deltas — one editor's inspector edit → serialize the changed component via reflection → broadcast → peers apply via `SetFieldFromString`. This is Unreal's Multi-User Editing; the `CollabEdit`/`LiveEditBridge` seed already exists. Depends only on sub-project 1's serializer + the live ECS `World` document; can follow SP1 directly or run parallel to SP2–4. Brainstorm + spec when SP1 lands.

## Tier 1 — high ROI when picked up
1. **Zone/AreaServer sharding + gateway** — the mangos parallel. A gateway (auth + route; the `:3724` login seam is already real) in front of **N AreaServer processes**, one per region of the 13-hex Cindral Wastes, with **boundary handoff** at region edges (cross-worldspace-portal pattern). `WorldOriginSystem` is the handoff primitive. This is the honest "MMO scale" backbone.

## Tier 2 — solid, moderate lift
2. **Distributed asset-cook / shader farm** — `SparkShaderCompiler`/`SparkBuild` become a worker pool; editor Import + packager fan work out to them.
3. **Session orchestrator / control plane** — spins AreaServers up/down on demand, matchmakes, health-checks, drains. `SparkDaemon` is the natural host. Makes Tier-1 #1 operable rather than hand-launched.
4. **Bot-swarm load nodes** — a bot-orchestrator process running many headless clients (`NetworkStress_ConcurrentClients` exists). Load testing now; PvE population later.

## Tier 3 — interesting but premature for single-box solo dev
- Server-authoritative physics/navmesh offload nodes.
- Telemetry aggregator (fed by ChromeTracing/PerformanceStats).
- Asset-streaming / content-pak CDN node (libvfs/pak parallel).
- Script sandbox subprocess (crash-isolate hot-reloaded AngelScript).

## Selection notes
- Pursue **collaboration server** first — near-free given the serializer work, and directly improves the editor.
- Then **gateway + AreaServer sharding** — meatier, most mangos-flavored, primitives already exist.
- Treat the rest as opportunistic: cook farm when asset volume hurts; orchestrator when hand-launching nodes gets old.
