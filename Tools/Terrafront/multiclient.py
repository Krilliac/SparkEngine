#!/usr/bin/env python3
"""TF-110: dedicated server + two headless clients convergence harness.

Launches one TERRAFRONT dedicated server and two headless clients as separate
SparkEngine processes on a free loopback UDP port, drives every client through
the real onboarding path (register, login, character create, enter world,
faction, spawn) and scripted movement (tf_walk), and has every process print
its world view with tf_observe. The comparator then proves that at each
checkpoint both clients agree with the server on the in-world players, each
pawn's faction/class/health and its position.

Checkpoints are quiet windows: no pawn moves for several seconds around them,
so the server and client views must match within POSITION_TOLERANCE_M even
though the processes observe at slightly different wall-clock instants. The
server observes every SERVER_OBSERVE_INTERVAL_S; each client checkpoint is
paired with the server observation nearest to it in wall-clock time, anchored
by when each process's -exec clock started (its audit file's first entry).
QUIET_MARGIN_S is the widest instant spread the comparator accepts (client skew
plus server pairing gap); every scenario keeps each checkpoint further than
that from any spawn or walk, and the harness refuses a schedule that does not,
so an accepted skew can never catch a pawn mid-move and report it as divergence.

A check that stops checking must not pass: a child that exits non-zero, a
missing or partial audit file, a scripted command that reports ERR, or fewer
complete observations than the scenario schedules all fail the run.

Usage:
  multiclient.py --engine <SparkEngine> --module <SparkGameMMOFPS> \
      --scenario onboard_spawn_move --workdir <dir> [--cwd <asset root>]

Exit status 0 means every checkpoint converged; the JSON summary on stdout
carries the per-checkpoint verdicts either way.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import secrets
import shutil
import socket
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

POSITION_TOLERANCE_M = 1.5
MIN_MOVE_M = 1.0
MAX_CLIENT_SKEW_S = 2.0
MAX_SERVER_PAIRING_GAP_S = 1.0
# A client observes up to MAX_CLIENT_SKEW_S after the other and the server sample
# may sit MAX_SERVER_PAIRING_GAP_S further out, so a checkpoint must be quiet for
# longer than both together on each side. MOVE_SETTLE_S covers replication and
# interpolation after a scripted walk stops.
QUIET_MARGIN_S = MAX_CLIENT_SKEW_S + MAX_SERVER_PAIRING_GAP_S
MOVE_SETTLE_S = 0.5
SERVER_OBSERVE_INTERVAL_S = 0.5
SERVER_TAIL_S = 30.0
SERVER_READY_TIMEOUT_S = 60.0
POLL_INTERVAL_S = 0.05
FACTION_IDS = {"mra": 1, "auc": 2, "hlx": 3}

OBSERVE_TAG = "[TF-OBSERVE] "
AUDIT_HEADER = re.compile(r"^frame (\d+) t=(\d+(?:\.\d+)?)s \| (ok |ERR) \| (.*)$")
KEY_VALUE = re.compile(r"(\w+)=(\S*)")


@dataclass(frozen=True)
class Pawn:
    player: int
    faction: int
    cls: int
    health: int
    pos: tuple[float, float, float]


@dataclass
class Observation:
    role: str
    self_id: int
    continent: str
    expected_pawns: int
    expected_regions: int
    expected_vehicles: int
    pawns: dict[int, Pawn] = field(default_factory=dict)
    regions: int = 0
    vehicles: int = 0
    has_self_line: bool = False

    def complete(self) -> bool:
        return (self.has_self_line and len(self.pawns) == self.expected_pawns and
                self.regions == self.expected_regions and self.vehicles == self.expected_vehicles)


@dataclass
class AuditEntry:
    frame: int
    seconds: float
    ok: bool
    command: str
    output: list[str] = field(default_factory=list)


@dataclass(frozen=True)
class Scenario:
    """One scripted multi-client run. Client steps are (seconds, command template)."""

    name: str
    client_factions: tuple[str, ...]
    client_steps: tuple[tuple[float, str], ...]
    checkpoints: tuple[float, ...]
    moves: tuple[tuple[int, int], ...]
    client_seconds: float


SCENARIOS = {
    "onboard_spawn_move": Scenario(
        name="onboard_spawn_move",
        client_factions=("mra", "auc"),
        client_steps=(
            (1.0, "tf_connect 127.0.0.1:{port}"),
            (3.0, "tf_register {user} {password}"),
            (4.5, "tf_login {user} {password}"),
            (6.0, "tf_char_create {name} {faction}"),
            (7.5, "tf_char_list"),
            (9.0, "tf_enter 0"),
            (10.5, "tf_faction {faction}"),
            (11.5, "tf_spawn"),
            (20.5, "tf_walk 1 0 2"),
            (30.5, "tf_walk 0 1 2"),
        ),
        checkpoints=(17.0, 27.0, 37.0),
        moves=((0, 1), (1, 2)),
        client_seconds=40.0,
    ),
}


class HarnessError(RuntimeError):
    pass


def motion_windows(scenario: Scenario) -> list[tuple[float, float]]:
    """Client-clock intervals in which a pawn appears or moves: each spawn and each walk plus its settle time."""
    windows = []
    for at, template in scenario.client_steps:
        verb, *arguments = template.split()
        if verb == "tf_spawn":
            windows.append((at, at + MOVE_SETTLE_S))
        elif verb == "tf_walk":
            windows.append((at, at + float(arguments[2]) + MOVE_SETTLE_S))
    return windows


def quiet_window_violations(scenario: Scenario) -> list[str]:
    """Checkpoints closer than QUIET_MARGIN_S to a spawn or walk, which an accepted skew could split."""
    violations = []
    for index, checkpoint in enumerate(scenario.checkpoints):
        for start, end in motion_windows(scenario):
            gap = max(start - checkpoint, checkpoint - end, 0.0)
            if gap <= QUIET_MARGIN_S:
                violations.append(f"{scenario.name}: checkpoint {index} (t={checkpoint}) is {gap:.1f} s from motion "
                                  f"{start}-{end}; it must be quiet for more than {QUIET_MARGIN_S} s")
    return violations


# --------------------------------------------------------------------------- parsing


def split_complete_lines(text: str) -> list[str]:
    """Lines of a file another process may still be writing: drop a partial last line."""
    lines = text.split("\n")
    lines.pop()  # "" after a final newline, otherwise the unterminated partial line
    return [line.rstrip("\r") for line in lines]


def parse_audit(text: str) -> list[AuditEntry]:
    entries: list[AuditEntry] = []
    for line in split_complete_lines(text):
        header = AUDIT_HEADER.match(line)
        if header:
            entries.append(AuditEntry(int(header.group(1)), float(header.group(2)), header.group(3) == "ok ",
                                      header.group(4)))
        elif entries:
            entries[-1].output.append(line[6:] if line.startswith("    > ") else line)
    return entries


def _fields(body: str) -> dict[str, str]:
    return dict(KEY_VALUE.findall(body))


def parse_observations(lines: list[str]) -> list[Observation]:
    """Every complete observation in @p lines, in order. Unrelated lines may interleave."""
    observations: list[Observation] = []
    current: Observation | None = None
    for line in lines:
        at = line.find(OBSERVE_TAG)
        if at < 0:
            continue
        body = line[at + len(OBSERVE_TAG):]
        kind, _, rest = body.partition(" ")
        values = _fields(rest)
        try:
            if kind.startswith("role="):
                current = None
                header = _fields(body)
                current = Observation(role=header["role"], self_id=int(header["self"]),
                                      continent=header["continent"], expected_pawns=int(header["pawns"]),
                                      expected_regions=int(header["regions"]),
                                      expected_vehicles=int(header["vehicles"]))
                observations.append(current)
            elif current is None:
                continue
            elif kind == "self":
                current.has_self_line = True
            elif kind == "pawn":
                x, y, z = (float(v) for v in values["pos"].split(","))
                pawn = Pawn(int(values["id"]), int(values["faction"]), int(values["class"]),
                            int(values["health"]), (x, y, z))
                current.pawns[pawn.player] = pawn
            elif kind == "region":
                current.regions += 1
            elif kind == "vehicle":
                current.vehicles += 1
        except (KeyError, ValueError):
            if current is not None:
                observations.pop()  # a malformed line invalidates the observation it belongs to
            current = None
    return [obs for obs in observations if obs.complete()]


@dataclass
class Sample:
    seconds: float
    observation: Observation | None


def observe_samples(entries: list[AuditEntry]) -> list[Sample]:
    """One sample per executed tf_observe: the complete observation it printed, or None.

    The audit appends the console's most recent entries after each command, so
    an observe's block usually still holds the PREVIOUS observe's output. Only
    lines after this command's own "[exec] frame N" echo belong to it; without
    that scoping a probe that printed nothing would inherit a stale view.
    """
    samples = []
    for entry in entries:
        if entry.command.strip() != "tf_observe":
            continue
        echo = f"[exec] frame {entry.frame} "
        starts = [i for i, line in enumerate(entry.output) if line.startswith(echo) and line.endswith(": tf_observe")]
        observed = parse_observations(entry.output[starts[-1] + 1:]) if starts else []
        samples.append(Sample(entry.seconds, observed[0] if observed else None))
    return samples


# --------------------------------------------------------------------------- comparison


@dataclass
class RoleLog:
    role: str
    returncode: int | None
    anchor: float | None  # wall-clock (monotonic) seconds at which the -exec clock started
    entries: list[AuditEntry]
    faction: str | None = None


def compare_views(server: Observation, client: Observation, label: str) -> list[str]:
    problems = []
    if client.continent != server.continent:
        problems.append(f"{label}: continent {client.continent!r} != server {server.continent!r}")
    if set(client.pawns) != set(server.pawns):
        problems.append(f"{label}: players {sorted(client.pawns)} != server {sorted(server.pawns)}")
    for player, truth in server.pawns.items():
        seen = client.pawns.get(player)
        if seen is None:
            continue
        if (seen.faction, seen.cls) != (truth.faction, truth.cls):
            problems.append(f"{label}: player {player} faction/class {(seen.faction, seen.cls)} != server "
                            f"{(truth.faction, truth.cls)}")
        if seen.health != truth.health:
            problems.append(f"{label}: player {player} health {seen.health} != server {truth.health}")
        gap = math.dist(seen.pos, truth.pos)
        if gap > POSITION_TOLERANCE_M:
            problems.append(f"{label}: player {player} position {seen.pos} is {gap:.2f} m from server {truth.pos}")
    return problems


def evaluate(scenario: Scenario, server: RoleLog, clients: list[RoleLog]) -> dict:
    """Pair every scheduled client checkpoint with a server observation and compare them."""
    problems: list[str] = []
    for log in [server, *clients]:
        if log.returncode != 0:
            problems.append(f"{log.role}: exited with {log.returncode}")
        for entry in log.entries:
            if not entry.ok:
                problems.append(f"{log.role}: scripted command reported ERR: {entry.command}")
        if log.anchor is None:
            problems.append(f"{log.role}: no audit trail was written")

    server_samples = [s for s in observe_samples(server.entries) if s.observation is not None]
    client_samples = [observe_samples(c.entries) for c in clients]
    checkpoints: list[dict] = []
    server_views: list[Observation | None] = []

    for index in range(len(scenario.checkpoints)):
        verdict: dict = {"checkpoint": index, "problems": []}
        views: list[Observation] = []
        walls: list[float] = []
        for client, samples in zip(clients, client_samples):
            if index >= len(samples) or samples[index].observation is None or client.anchor is None:
                verdict["problems"].append(f"{client.role}: checkpoint {index} was not observed")
                continue
            views.append(samples[index].observation)
            walls.append(client.anchor + samples[index].seconds)
        server_view = None
        if len(views) == len(clients) and server.anchor is not None:
            if max(walls) - min(walls) > MAX_CLIENT_SKEW_S:
                verdict["problems"].append(f"clients observed {max(walls) - min(walls):.2f} s apart "
                                           f"(> {MAX_CLIENT_SKEW_S} s): the quiet window is not shared")
            target = sum(walls) / len(walls)
            nearest = min(server_samples, key=lambda s: abs(server.anchor + s.seconds - target), default=None)
            if nearest is None or abs(server.anchor + nearest.seconds - target) > MAX_SERVER_PAIRING_GAP_S:
                verdict["problems"].append(f"server: no observation within {MAX_SERVER_PAIRING_GAP_S} s of "
                                           f"checkpoint {index}")
            else:
                server_view = nearest.observation
        server_views.append(server_view)

        if server_view is not None:
            verdict["server_players"] = sorted(server_view.pawns)
            if len(server_view.pawns) != len(clients):
                verdict["problems"].append(f"server: {len(server_view.pawns)} players in world, expected "
                                           f"{len(clients)}")
            selves = [view.self_id for view in views]
            if len(set(selves)) != len(selves):
                verdict["problems"].append(f"clients: player ids {selves} are not distinct")
            for client, view in zip(clients, views):
                pawn = server_view.pawns.get(view.self_id)
                if pawn is None:
                    verdict["problems"].append(f"{client.role}: own player {view.self_id} has no pawn on the server")
                elif client.faction is not None and pawn.faction != FACTION_IDS[client.faction]:
                    verdict["problems"].append(f"{client.role}: server faction {pawn.faction} != chosen "
                                               f"{client.faction}")
                verdict["problems"].extend(compare_views(server_view, view, client.role))
            verdict["client_players"] = selves
        verdict["converged"] = not verdict["problems"]
        checkpoints.append(verdict)

    for before, after in scenario.moves:
        first = server_views[before] if before < len(server_views) else None
        second = server_views[after] if after < len(server_views) else None
        if first is None or second is None:
            problems.append(f"movement {before}->{after}: a server checkpoint is missing")
            continue
        for player, pawn in first.pawns.items():
            moved = second.pawns.get(player)
            if moved is None or math.dist(pawn.pos, moved.pos) < MIN_MOVE_M:
                problems.append(f"movement {before}->{after}: server pawn {player} did not move {MIN_MOVE_M} m")

    converged = sum(1 for c in checkpoints if c["converged"])
    if converged != len(scenario.checkpoints) or not scenario.checkpoints:
        problems.append(f"{converged}/{len(scenario.checkpoints)} checkpoints converged")
    return {"scenario": scenario.name, "passed": not problems, "problems": problems, "checkpoints": checkpoints}


# --------------------------------------------------------------------------- processes


def free_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def format_seconds(seconds: float) -> str:
    return f"{seconds:.1f}"


def server_script(port: int, run_seconds: float) -> str:
    lines = ["0 tf_status", f"t0.5 tf_dedicated {port}"]
    at = 1.0
    while at < run_seconds - 0.5:
        lines.append(f"t{format_seconds(at)} tf_observe")
        at += SERVER_OBSERVE_INTERVAL_S
    return "\n".join(lines) + "\n"


def client_script(scenario: Scenario, port: int, faction: str) -> str:
    values = {
        "port": port,
        "faction": faction,
        "user": "mc" + secrets.token_hex(6),
        "password": secrets.token_urlsafe(18),
        "name": "MC" + secrets.token_hex(5),
    }
    timeline = [(at, template.format(**values)) for at, template in scenario.client_steps]
    timeline += [(at, "tf_observe") for at in scenario.checkpoints]
    timeline.sort(key=lambda item: item[0])
    return "0 tf_status\n" + "".join(f"t{format_seconds(at)} {command}\n" for at, command in timeline)


@dataclass
class Child:
    role: str
    process: subprocess.Popen
    audit: Path
    anchor: float | None = None

    def poll_anchor(self) -> None:
        if self.anchor is None and self.audit.exists():
            self.anchor = time.monotonic()


def launch(role: str, args: argparse.Namespace, workdir: Path, script: str, seconds: float) -> Child:
    role_dir = workdir / role
    # The audit is append-only: a previous run's trail would replay stale views.
    shutil.rmtree(role_dir, ignore_errors=True)
    (role_dir / "saves").mkdir(parents=True)
    cfg = role_dir / f"{role}.cfg"
    cfg.write_text(script, encoding="utf-8", newline="\n")
    audit = role_dir / "exec_audit.log"
    env = dict(os.environ, TF_SAVE_ROOT=str(role_dir / "saves"), SPARK_RHI_BACKEND="null")
    command = [str(args.engine), "-headless", "-no-subprocess", "-require-game", "-threads", "2", "-game",
               str(args.module), "-exec", str(cfg), "-exec-audit", str(audit), "-test-seconds",
               format_seconds(seconds)]
    with open(role_dir / "stdout.log", "wb") as stdout, open(role_dir / "stderr.log", "wb") as stderr:
        process = subprocess.Popen(command, cwd=args.cwd, env=env, stdin=subprocess.DEVNULL, stdout=stdout,
                                   stderr=stderr)
    return Child(role, process, audit)


def wait_for_server(server: Child, deadline: float) -> None:
    while time.monotonic() < deadline:
        server.poll_anchor()
        if server.process.poll() is not None:
            raise HarnessError(f"server exited with {server.process.returncode} before it started listening")
        if server.audit.exists():
            for entry in parse_audit(server.audit.read_text(encoding="utf-8", errors="replace")):
                if entry.command.startswith("tf_dedicated"):
                    if any("dedicated server started" in line for line in entry.output):
                        return
                    raise HarnessError("server: tf_dedicated did not start a dedicated server")
        time.sleep(POLL_INTERVAL_S)
    raise HarnessError(f"server did not start listening within {SERVER_READY_TIMEOUT_S} s")


def wait_for_exit(children: list[Child], deadline: float) -> None:
    while any(child.process.poll() is None for child in children):
        for child in children:
            child.poll_anchor()
        if time.monotonic() > deadline:
            raise HarnessError("timed out waiting for the processes to exit")
        time.sleep(POLL_INTERVAL_S)
    for child in children:
        child.poll_anchor()


def run(args: argparse.Namespace) -> dict:
    scenario = SCENARIOS[args.scenario]
    violations = quiet_window_violations(scenario)
    if violations:
        raise HarnessError("; ".join(violations))
    workdir = Path(args.workdir).resolve()
    workdir.mkdir(parents=True, exist_ok=True)
    port = free_udp_port()
    deadline = time.monotonic() + args.timeout
    server_seconds = scenario.client_seconds + SERVER_TAIL_S
    children: list[Child] = []
    try:
        server = launch("server", args, workdir, server_script(port, server_seconds), server_seconds)
        children.append(server)
        wait_for_server(server, min(deadline, time.monotonic() + SERVER_READY_TIMEOUT_S))
        for index, faction in enumerate(scenario.client_factions):
            script = client_script(scenario, port, faction)
            children.append(launch(f"client{index + 1}", args, workdir, script, scenario.client_seconds))
        wait_for_exit(children, deadline)
    finally:
        for child in children:
            if child.process.poll() is None:
                child.process.kill()
                child.process.wait()
        for index in range(len(scenario.client_factions)):
            (workdir / f"client{index + 1}" / f"client{index + 1}.cfg").unlink(missing_ok=True)

    logs = []
    for child in children:
        text = child.audit.read_text(encoding="utf-8", errors="replace") if child.audit.exists() else ""
        logs.append(RoleLog(child.role, child.process.returncode, child.anchor if text else None, parse_audit(text)))
    for log, faction in zip(logs[1:], scenario.client_factions):
        log.faction = faction
    summary = evaluate(scenario, logs[0], logs[1:])
    summary["port"] = port
    summary["workdir"] = str(workdir)
    return summary


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--engine", required=True, type=Path, help="SparkEngine executable")
    parser.add_argument("--module", required=True, type=Path, help="SparkGameMMOFPS module")
    parser.add_argument("--scenario", required=True, choices=sorted(SCENARIOS))
    parser.add_argument("--workdir", required=True, help="per-run directory for scripts, audits and saves")
    parser.add_argument("--cwd", type=Path, help="asset root the processes run in (default: the engine's directory)")
    parser.add_argument("--timeout", type=float, default=200.0, help="wall-clock limit for the whole run")
    args = parser.parse_args(argv)
    args.engine = args.engine.resolve()
    args.module = args.module.resolve()
    args.cwd = (args.cwd or args.engine.parent).resolve()
    for required in (args.engine, args.module):
        if not required.is_file():
            parser.error(f"not a file: {required}")

    try:
        summary = run(args)
    except HarnessError as error:
        summary = {"scenario": args.scenario, "passed": False, "problems": [str(error)], "checkpoints": []}
    print(json.dumps(summary, indent=2))
    if not summary["passed"]:
        print(f"TerrafrontMultiClient {args.scenario}: FAILED", file=sys.stderr)
        return 1
    print(f"TerrafrontMultiClient {args.scenario}: {len(summary['checkpoints'])} checkpoints converged")
    return 0


if __name__ == "__main__":
    sys.exit(main())
