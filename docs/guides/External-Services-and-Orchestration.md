# External Services and Orchestration

SparkEngine ships its production-facing server and tool responsibilities as
small, independently deployable executables. The game runtime stays focused on
simulation; process ownership, ingress, collaboration, cooking, and automation
have explicit boundaries and failure domains.

## Process map

| Executable | Responsibility | Trust boundary |
|---|---|---|
| `SparkServer` | Headless authoritative game-module host | Loads exactly one selected game module or manifest |
| `SparkGateway` | Authenticates admissions, routes sessions, coordinates fenced area handoffs | Client-facing ingress; never becomes gameplay authority |
| `SparkDaemon` | Local cache services and opt-in process supervision | Owner-local control plane with executable allow-roots |
| `SparkOrchestrator` | Stateless CLI for daemon definitions, lifecycle, drain, restart, and status | Holds no process handles and cannot bypass daemon policy |
| `SparkCollabServer` | Presence, locks, edit history, and collaboration capabilities | Separate process because editor collaboration accepts a different traffic class |
| `SparkCooker` | Deterministic project-wide asset cooking | Offline input/output roots only |
| `SparkWorker` | One digest-pinned bounded asset job | No scheduler or broad project authority |
| `SparkAutomation` | Launch, timeout, screenshot, log, JSON, and JUnit smoke checks | Owns and terminates only its launched process tree |

Build them with the default configuration, or control the groups explicitly:

```powershell
cmake -S . -B build `
  -DENABLE_SERVER_PROCESSES=ON `
  -DENABLE_ASSET_PIPELINE_TOOLS=ON `
  -DENABLE_AUTOMATION_HOST=ON
cmake --build build --config RelWithDebInfo --parallel 1
```

Installed server processes are part of the runtime component; the cooker,
worker, and automation host are installed as tools.

## Dedicated server

`SparkServer` requires dynamic game selection. It never silently links a
particular sample game into the host:

```powershell
SparkServer --module Packages/MyGame/bin/MyGame.dll --map arena `
  --port 27015 --max-clients 64 --tick-rate 60 `
  --health-file Temp/server-health.json --stop-file Temp/server.stop
```

Use `--manifest spark.modules.json` instead of `--module` for a package
manifest; supplying both is rejected. `--run-for-ms` provides a bounded smoke
run. The editor's Dedicated Server panel builds these arguments from package
metadata, displays health, and stops through the sentinel rather than killing a
healthy process.

To expose an area-control endpoint to the MMO gateway, also provide a unique
`--control-endpoint`, an owner-only `--gateway-key-file`, and optionally a
`--control-state-file`. The server persists session epoch/phase fences so a
restart cannot accept stale or out-of-order handoff commands.

## Gateway and area handoff

Start each area server first, then the gateway using a configuration derived
from [`SparkGateway/config/gateway.example.ini`](../../SparkGateway/config/gateway.example.ini):

```powershell
SparkGateway --config Config/gateway.ini `
  --health-file Temp/gateway-health.json `
  --stop-file Temp/gateway.stop
```

The gateway authenticates short-lived admission credentials, rejects replay,
registers only loopback area-control endpoints, and performs
prepare/transfer/commit/acknowledge as an idempotent epoch-fenced state machine.
Routing acceptance does not move gameplay authority until the target and source
phases have completed. On Windows the owner-local control links use named pipes;
POSIX hosts use Unix-domain sockets.

## Daemon supervision

Orchestration is disabled unless at least one executable allow-root is supplied:

```powershell
SparkDaemon --socket spark-daemon-production `
  --orchestrator-allow-root D:\SparkEngine\Packages `
  --orchestrator-max-processes 16 `
  --orchestrator-state-file Temp/orchestration.state
```

Definitions are rejected when an executable escapes every allow-root. Mutation
requests carry a client-instance/sequence idempotency key. The daemon journals
definitions and desired state, reconciles live PID plus process-start tokens,
applies drain/stop deadlines, groups descendants for teardown, backs off after
crashes, and quarantines crash loops. The operator CLI remains stateless:

```powershell
SparkOrchestrator --socket spark-daemon-production list
SparkOrchestrator --socket spark-daemon-production define area-a `
  D:\SparkEngine\Packages\AreaA\SparkServer.exe `
  D:\SparkEngine\Packages\AreaA --manifest spark.modules.json --map frontier
SparkOrchestrator --socket spark-daemon-production start area-a
SparkOrchestrator --socket spark-daemon-production status area-a
SparkOrchestrator --socket spark-daemon-production drain area-a
SparkOrchestrator --socket spark-daemon-production stop area-a
```

Use a stable `--client` value and strictly increasing `--sequence` values when
an external controller retries mutations.

## Collaboration isolation

Run collaboration separately from the trusted daemon control plane:

```powershell
SparkCollabServer --socket spark-collab-project-a
```

The service bounds every wire field, issues expiring capabilities, and owns
presence, node locks, and edit history. Compromise or overload of a collaboration
session therefore does not grant cache or process-supervision authority.

In SparkEditor, open **Collaboration**, keep **Use standalone
SparkCollabServer** selected, and enter the same endpoint plus a stable project
session ID. **Create Broker Session** creates the capability-protected session;
other editor instances use **Join Broker Session**. Existing hierarchy edits,
selection presence, viewport peer overlays, lock indicators, and callbacks all
flow through the broker-backed `CollaborativeEditSession` path. Direct
peer-hosted TCP is retained only as an explicit fallback.

CTest's `SparkCollabProcessSmoke` launches the real broker executable, waits for
IPC readiness, connects two production clients, exercises presence, locking,
ordered edits and snapshots, then requests and verifies graceful process
shutdown.

## Operational checks

The editor's Service Topology panel generates start/status/stop commands without
claiming ownership it does not have. For unattended checks, combine the tools:

```powershell
SparkAutomation --name server-smoke `
  --executable build/bin/RelWithDebInfo/SparkServer.exe `
  --working-dir Packages/MyGame --timeout-ms 30000 --expected-exit 0 `
  --captured-log TestResults/server.log --json TestResults/server.json `
  --junit TestResults/server.xml -- `
  --manifest spark.modules.json --run-for-ms 1000
```

Treat health JSON, orchestration status, JUnit, and captured logs as distinct
signals. A process existing is not the same as readiness, and a routed session
is not proof that an area has accepted gameplay authority.
