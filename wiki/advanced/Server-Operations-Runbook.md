# Server Operations Runbook

> **Audience:** Operators and programmers who run `SparkServer` themselves
>
> **Thread Context:** Separate processes (the operator, a supervisor, and the server process)
>
> **Platform/Backend Scope:** Windows and Linux `SparkServer`; the wedge drill is POSIX only; independent of the RHI backend

## Overview

This runbook tells an operator how to detect, diagnose, drain, restart, and
recover a failing `SparkServer`, and how to back up and restore the TERRAFRONT
database. Every step uses a mechanism that exists in the code today, and every
section ends with the drill that rehearses it. Work item: `OPS-110`.

Scope limits:

- SparkEngine ships no hosted online service (owner decision OD-08, see
  [Online Service Boundary](Online-Service-Boundary.md)). You run these
  processes on your own hosts. Nothing here describes a service the engine
  operates for you.
- **SLOs and alert thresholds are provisional.** No versioned tick, memory, or
  latency budget exists yet. The numbers below are harness guards and
  suggested starting points, not service-level objectives.
- A local drill pass is precursor evidence only. The drill summary says so in
  its `evidenceScope` field. It is not a recorded release drill.

The whole procedure is rehearsed end to end by one command:

```
python3 tools/ops/server_recovery_drill.py --server <SparkServer> --module <game-module> --expected-sha <full-sha> --summary server-drill.json
```

From a build tree, `ctest -L recovery-drill` runs the same drill against the
built `SparkServer` and `SparkGame` (CTest `Server_RecoveryDrill`), plus the
DATA-120 backup/restore tests.

## When to Use

- A server stops answering players, or its health file stops changing.
- You need to take a server out of service for a deploy or a host reboot.
- A server keeps crashing after restarts.
- You need to roll the TERRAFRONT database back to a backup.
- Before a release, to rehearse the whole procedure on the release build.

## 1. Launch for Operability

Start every server with a health file and a stop file. Without them you can
only watch stdout and kill the process.

```
SparkServer --module <game-module> --bind-address loopback --health-file /srv/spark/health.json --stop-file /srv/spark/stop --status-interval-ms 250
```

Keep the exact argument list: a restart must reuse it unchanged. Record the
build identity once per deploy:

```
SparkServer --version
```

It prints `SparkServer <version> <commit> (<treeState>)`. The same three values
appear in every health snapshot.

## 2. Detect

The server prints one JSON health snapshot per line to stdout at startup,
every `--status-interval-ms`, and on each lifecycle change. It also replaces
the `--health-file` atomically with the same object. The fields are listed on
the [Dedicated Server](../subsystems/Dedicated-Server.md) page.

Treat a server as failed when **any** of these holds:

| Signal | Rule |
|---|---|
| Stale snapshot | The health file's modification time and its `ticks` value have not changed for the staleness window. Use at least three status intervals. The drill defaults to 3 s with a 250 ms interval. |
| Not ready | `ready` is `false` while `draining` and `stopping` are both `false`. The server is up but not serving: the listener is down, no game module is initialized, or gateway control is not ready. |
| Error set | `error` is not empty. It carries the server's last error, such as a module that vetoed shutdown. |
| Process gone | The process has exited. |

**Never trust `live` alone.** A killed server cannot rewrite its health file,
so the file keeps its last `live=true, ready=true` snapshot. Detection must come
from staleness. The drill's `crash` scenario proves this: it records
`lastSnapshotLive: true` for the killed server and still detects it.

Drill: `python3 tools/ops/server_recovery_drill.py --server <SparkServer> --module <game-module> --scenario crash`

## 3. Diagnose

Work through these in order.

1. **Is the process alive?** A stale snapshot from a live process means the
   server is wedged: it is not ticking. A stale snapshot with no process means
   it exited or crashed. On POSIX the drill's `wedge` scenario rehearses the
   wedged case with SIGSTOP.
2. **How did it exit?** `SparkServer` exit statuses:

   | Status | Meaning |
   |---|---|
   | 0 | Graceful stop after a drain |
   | 1 | Startup failed. The last health JSON, with `error`, is printed to stderr. Also: the CPU is below the SSE4.2 + POPCNT floor, or the Windows console control handler could not be installed |
   | 2 | Invalid command line. The error and the usage text go to stderr |
   | 3 | A game module vetoed graceful shutdown |
   | signal or NTSTATUS | Killed or crashed |

3. **Which build is it?** Compare `version`, `commit`, and `treeState` in the
   last snapshot with the identity you recorded at deploy time. A `dirty` or
   `unknown` tree state means the binary cannot be tied to one commit.
4. **Is it overloaded or leaking?** `tickP95Us` and `tickP99Us` are tick work
   times in microseconds, excluding the frame-budget sleep. At 60 Hz the frame
   budget is 16,666 us, so a p99 near that is a server falling behind.
   `rssBytes` is the resident set. It is `null` if the platform query fails.
   Compare it across snapshots over time; `tools/ops/server_soak.py` applies
   the same checks over a long run.
5. **What did it log?** The server logs to stderr, because stdout is reserved
   for the health stream. Keep stderr from every launch. The drill keeps
   `server-stderr-<launch>.log` per launch in `--work-dir`.

Crash dumps: `SparkServer` installs no crash handler, so it writes no
SparkCrashReporter crash manifest. Collect an OS core dump (Linux) or a
Windows Error Reporting LocalDumps minidump for a crashed server. The engine
and editor crash handlers do write crash logs. For a Linux crash log, resolve
the symbolic frames offline with
`python3 tools/ops/symbolicate_crash.py resolve --store <symbol-store> --log <crash.log>`,
and check a crash package with `python3 tools/ops/validate_crash_package.py <manifest>`.

## 4. Drain

A drain stops the server gracefully. It publishes a snapshot with
`draining=true, ready=false` **before** any teardown, and it keeps ticking
while a game module vetoes shutdown. Route players away as soon as you see
`ready=false`.

Request a drain in one of these ways:

- Create the stop file (`--stop-file`). This works the same on every
  platform, and it is what the drill uses.
- On Linux, send SIGTERM or SIGINT.
- On Windows, send CTRL_BREAK_EVENT to the server's process group. The server
  must have been started in its own process group, which is what
  `SparkOrchestrator` does. Windows never raises SIGTERM.
- Under the daemon, run `SparkOrchestrator drain <id>`. It sends SIGTERM
  (Linux) or CTRL_BREAK_EVENT (Windows) to the process group, and it kills the
  process when the graceful-stop deadline passes. The deadline is the
  definition's `gracefulStopMilliseconds`: 5 s for a definition made by
  `SparkOrchestrator define`, and never more than 60 s.

A clean drain ends with `stopping=true`, then a final `live=false,
ready=false` snapshot, and exit status 0. Exit status 3 means a module vetoed
shutdown. The `error` field names the reason.

Drill: `python3 tools/ops/server_recovery_drill.py --server <SparkServer> --module <game-module> --scenario drain`

## 5. Restart

Before a restart, make sure the old process has gone. A wedged server does not
drain, so kill it: SIGKILL on Linux, or `taskkill /F /PID <pid>` on Windows.

**Manual restart.** Relaunch with the identical argument list from step 1.
Delete the stop file first, or the new process drains at once.

**Supervised restart.** `SparkDaemon` supervises processes on one host only.
It is not a fleet control plane. Its orchestration service:

- relaunches a process whose definition has `RestartPolicy::OnFailure` or
  `RestartPolicy::Always`, after a restart backoff of 500 ms by default,
  clamped to 100 to 60,000 ms (`restartBackoffMilliseconds`);
- quarantines a process that crashes 5 times within a minute by default
  (`maximumCrashesPerMinute`) and stops restarting it;
- accepts `SparkOrchestrator restart <id>` for an operator-requested restart,
  and `SparkOrchestrator status <id>` to read the state (`running`,
  `draining`, `backoff`, `failed`, or `quarantined`).

`SparkOrchestrator define` always creates a definition with the `Never` restart
policy. It has no option to set a policy, so policy-driven restarts need a
definition built through the orchestration protocol
(`SparkDaemon/src/OrchestrationProtocol.h`). The source of these rules is
`SparkDaemon/src/OrchestrationService.cpp`. The
[Daemon Services Architecture](Daemon-Services-Architecture.md) page covers
the daemon itself.

## 6. Recover

A restart has recovered the server only when all of these hold:

1. A **new** health snapshot, written after the restart, reports
   `live=true, ready=true` with a non-empty `gameModule`. The old file's last
   snapshot must never count.
2. The snapshot reports the **same `commit`** as before the failure, unless
   you meant to deploy a different build.
3. `ticks` **advances** past the ready snapshot within the staleness window.

The drill checks all three after every scenario, and records the detection
and recovery times per scenario (`detectS`, `recoverS`) in its summary.

Drill: `python3 tools/ops/server_recovery_drill.py --server <SparkServer> --module <game-module> --expected-sha <full-sha> --summary server-drill.json`

## 7. Back Up and Restore the TERRAFRONT Database

The TERRAFRONT account and character database is one JSON file per save root.
Backup and restore are API calls in the game module, not a separate tool:
`Terrafront::TFDatabase::CreateBackup` and `RestoreFromBackup`
(`GameModules/SparkGameMMOFPS/Source/Persistence/TFDatabaseBackup.cpp`). The
full contract is in the [persistence specification](../../docs/specs/persistence.md).

- `CreateBackup` copies the committed file and writes a `<backup>.sha256`
  sidecar. It never overwrites an existing backup. Check a backup's integrity
  offline with `sha256sum -c <backup>.sha256`.
- `RestoreFromBackup` checks the digest and the schema before it touches the
  primary file.
- **Stop every authority on the save root before a restore, and restart
  them after it.** When the displaced revision is unknown this is required,
  not advisory.

Drill: `ctest -L recovery-drill` runs the `Persistence_BackupRestore` cases on
every platform and the `Persistence_RecoveryDrill` crash-inside-a-commit cases
on POSIX.

## 8. Alerts

The engine ships no alerting. Poll the health file from your own monitoring and
alert on the step 2 signals. Provisional starting points, matching the harness
guards:

| Alert | Suggested starting point |
|---|---|
| Server down | Health snapshot stale for 3 s, or three status intervals |
| Not serving | `ready=false` without `draining` for longer than your startup time |
| Tick budget | `tickP99Us` above 16,000 us at 60 Hz, the soak harness guard |
| Memory growth | RSS rising faster than 64 MiB per hour, the soak harness guard |
| Crash loop | `SparkOrchestrator status <id>` reports `quarantined` |

Tune these for your game before you rely on them. They are not SLOs.

## Threading Model

The runbook drives separate processes. The server loop thread publishes health
snapshots. The signal or console control handler only sets a stop flag that the
loop reads on its next tick, so a drain always starts on the loop thread.

## Platform and Backend Support

| Step | Linux | Windows |
|---|---|---|
| Health file and stdout stream | Yes | Yes |
| Drain by stop file | Yes | Yes |
| Drain by signal | SIGTERM, SIGINT | CTRL_BREAK_EVENT, CTRL_C_EVENT |
| Wedge drill | Yes (SIGSTOP) | No |
| Crash log symbolication | `tools/ops/symbolicate_crash.py` (engine and editor logs) | Not available |
| Persistence crash drill | Yes | No (backup/restore cases only) |

## Key APIs and Types

| Surface | Role |
|---|---|
| `SparkServer --health-file`, `--stop-file`, `--version` | Operator surface of the server process |
| `Spark::Server::ServerHealth` (`SparkServer/src/ServerHealth.h`) | Health snapshot fields |
| `tools/ops/server_recovery_drill.py` | Detect, diagnose, drain, restart, and recover drill |
| `tools/ops/server_soak.py` | Long-run memory and tick-budget soak |
| `SparkOrchestrator` | Single-host supervision: define, start, drain, restart, status |
| `Terrafront::TFDatabase::CreateBackup` / `RestoreFromBackup` | TERRAFRONT database backup and restore |

## Performance Notes

A health snapshot costs one small file write per status interval. Keep
`--status-interval-ms` at 250 ms or more in production. The server accepts
100 ms to 3,600,000 ms.

## Troubleshooting

- **The restarted server drains at once.** The old stop file is still there.
  Delete it before the restart, or give each launch its own stop file.
- **The drill fails with "health file kept changing".** Another process is
  writing the same health file. Give each server its own path.
- **A restart reports a different commit.** The binary was replaced between
  launches. Redeploy the intended build, or pass the new SHA as
  `--expected-sha`.

## Related Pages

- [Dedicated Server](../subsystems/Dedicated-Server.md)
- [Daemon Services Architecture](Daemon-Services-Architecture.md)
- [Online Service Boundary](Online-Service-Boundary.md)
- [Persistence specification](../../docs/specs/persistence.md)

## Source & Freshness

Written 2026-09-27 for `OPS-110` against `SparkServer/src/main.cpp`,
`SparkServer/src/ServerApplication.cpp`, `SparkServer/src/ServerHealth.h`,
`SparkDaemon/src/OrchestrationService.cpp`, and `tools/ops/`. The
`ServerRecoveryDrill_Harness` CTest fails when a `SparkServer` flag or script
path on this page no longer exists.
