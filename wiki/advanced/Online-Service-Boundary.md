# Online Service Boundary

> **Audience:** Programmers, operators, and anyone writing public claims about SparkEngine
>
> **Thread Context:** Mixed (network transport threads, game thread, separate server processes)
>
> **Platform/Backend Scope:** All platforms; independent of the RHI backend

## Overview

This page is the contract for where SparkEngine stops and a game's own online
services begin. It records owner decision **OD-08**
([`docs/readiness/OWNER-DECISIONS.md`](../../docs/readiness/OWNER-DECISIONS.md)):

> Identity, matchmaking, fleet, entitlement and billing services are out of
> engine scope. The engine ships no hosted online services.

SparkEngine is software you build and run yourself. It gives you a network
transport, points where you plug in authentication, and server executables you
can deploy. It does not run, host, or operate any online service for players.
Nothing in this repository is a hosted service, and no page may say or imply
otherwise. The release-readiness work item is `NET-110`. The capability row is
`services.production`, which is `unsupported`.

## What the engine provides

| Layer | What ships in the engine | Where |
|---|---|---|
| Transport | UDP client/server transport behind `ITransport`, reliability, replication, prediction, lag compensation, packet validation, and a fail-closed bind policy (loopback or private LAN only) | `SparkEngine/Source/Engine/Networking/` ([Networking](../subsystems/Networking.md)) |
| Authentication hooks | Integration points only. `IGatewayAuthenticator::Authenticate` accepts an opaque admission credential and returns a result. `KeyFileAuthenticator` is the local reference: owner-local key file, HMAC credentials, replay rejection. `NetworkManager::RegisterSensitiveHandler` erases received payload copies for credential-bearing messages. `Spark::PasswordHash` provides PBKDF2-HMAC-SHA256 helpers for any account store you build yourself | `SparkGateway/src/GatewayCoordinator.h`, `SparkGateway/src/GatewaySecurity.h`, `SparkEngine/Source/Engine/Networking/NetworkManager.h`, `SparkEngine/Source/Utils/PasswordHash.h` |
| Server processes | `SparkServer` (headless authoritative module host), `SparkGateway` (admission and fenced area handoff), `SparkDaemon` / `SparkOrchestrator` (owner-local supervision), `SparkCollabServer` (editor collaboration) | [External Services and Orchestration](../../docs/guides/External-Services-and-Orchestration.md), [Dedicated Server](../subsystems/Dedicated-Server.md), [Area Server Architecture](../subsystems/Area-Server-Architecture.md) |
| Platform service interface | `IOnlinePlatform` and `OnlineServiceManager`. The default `NullOnlinePlatform` works offline and keeps everything in process memory. `SteamPlatform`, `EpicPlatform`, and `ConsolePlatform` are compile-only stubs that report no capabilities and fail every call | `SparkEngine/Source/Engine/OnlineServices/OnlineServices.h` ([Online Services](../gameplay-tools/Online-Services.md)) |

These are building blocks you deploy and run yourself. The server processes are
development and reference executables. They sit outside the service-free
`stable-v1` profile, and they are not evidence of a production deployment.

## What the engine does not provide

The engine does not include, host, or operate any of these services. A game
that needs them must build them, buy them, or use a platform holder's service,
and connect them through the hooks above:

- **Identity and accounts:** sign-up, login, account recovery, credential
  storage, and issuing the admission credentials that `IGatewayAuthenticator`
  checks.
- **Matchmaking and lobbies:** skill rating, queues, party formation, and
  session discovery beyond LAN. `NullOnlinePlatform` session calls are local
  only.
- **Fleet management:** provisioning, scaling, placement, health-driven
  replacement, and regional routing of server processes. `SparkDaemon` looks
  after processes on a single host only. It is not a fleet control plane.
- **Entitlements, billing, and payments:** ownership checks, store integration,
  receipts, refunds, and tax.
- **Player data services:** cloud saves, leaderboards, achievements, friends, and
  presence as a service. `NullOnlinePlatform` keeps these in process memory,
  and they are lost when the process exits.
- **Operations:** secrets management for production credentials, abuse and
  moderation tooling, telemetry pipelines, backups, and incident response.

A Steam, Epic, or console build needs that vendor's SDK, agreement, and
backend. The engine ships none of them. The stub platforms and
`SteamTransport` exist so an integration can be added without changing the
interface. They do nothing on their own.

## Trust boundaries

```
 Player client  ──UDP──►  SparkServer / AreaServer   (engine: gameplay authority)
       │                         ▲
       │ admission credential    │ owner-local control link (named pipe / Unix socket)
       ▼                         │
   SparkGateway  ────────────────┘                   (engine: admission + handoff)
       ▲
       │ issues credentials, owns accounts, billing, matchmaking, fleet
   Product-owned services                            (NOT engine: your infrastructure)
```

- The engine trusts a credential only after the configured
  `IGatewayAuthenticator` accepts it. Whoever issues credentials sits outside
  the engine, and the game owns that service.
- Gateway credentials protect admission and the owner-local control plane. They
  do not authenticate the gameplay UDP path. That path is experimental and
  unauthenticated, and it binds to loopback by default. See
  [External Services and Orchestration](../../docs/guides/External-Services-and-Orchestration.md).
- Production secrets never live in the engine's shipped configuration (OD-22).

## When to Use

- Before you write a website, README, store page, or wiki claim about online
  features. Read this page first.
- When you plan a multiplayer game and need to know what you must build or buy.
- When you add a platform integration. Implement `IOnlinePlatform` or
  `IGatewayAuthenticator` in your game or integration layer instead of adding a
  hosted service to the engine.

## Threading Model

`IGatewayAuthenticator::Authenticate` can be called from any gateway transport
thread and must be thread-safe. `OnlineServiceManager` is ticked from the
gameplay lifecycle on the game thread. The rules for transport threads are on
the [Networking](../subsystems/Networking.md) page.

## Platform and Backend Support

The boundary is the same on every platform. The service-free `stable-v1`
profile ships no online services and no multiplayer support claim.

## Key APIs and Types

| Type | Role at the boundary |
|---|---|
| `Spark::OnlineServices::IOnlinePlatform` | The interface a product's platform integration implements |
| `Spark::OnlineServices::NullOnlinePlatform` | Offline, in-memory default. It never contacts a network service |
| `IGatewayAuthenticator` | Checks product-issued admission credentials |
| `KeyFileAuthenticator` | Local reference authenticator (owner-local key file) |
| `Spark::PasswordHash` | PBKDF2 helpers for product-owned account stores |

## Performance Notes

The engine sets no timeout, retry, or circuit-breaker budget for services it
does not own. Budgets for product services belong to the product that runs
them.

## Troubleshooting

- **"Cloud save" data is gone after a restart.** `NullOnlinePlatform` keeps it
  in memory on purpose. Use the local [Save System](../gameplay-tools/Save-System.md)
  or your own backend.
- **`SteamPlatform::Login` always fails.** The Steamworks SDK is not linked. The
  class is a stub.

## Enforcement

`python3 tools/site-data/validate.py` rejects any sentence on a governed public
surface that says the engine hosts, manages, or operates an online service
(identity, matchmaking, fleet, entitlement, billing, leaderboard, cloud-save, or
similar), unless the same sentence, or the same table row, negates it. The patterns are
`HOSTED_ONLINE_SERVICE_CLAIM` and `hosted_online_service_claim_errors` in
`tools/site-data/validate.py`. The governed surfaces are every public claim
surface plus this page, the Online Services page, and `docs/site/readiness.json`.
`Tests/Tools/test_site_data_contract.py` covers the rule.

## Related Pages

- [Online Services](../gameplay-tools/Online-Services.md)
- [Networking](../subsystems/Networking.md)
- [Dedicated Server](../subsystems/Dedicated-Server.md)
- [Daemon Services Architecture](Daemon-Services-Architecture.md)

## Source & Freshness

Written 2026-09-24 for `NET-110` from OD-08 and the current sources listed
above. When an online-service claim or the owner decision changes, update this
page.
