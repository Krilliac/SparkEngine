#!/usr/bin/env python3
"""TF-110: dedicated server + two headless clients convergence harness.

Launches one TERRAFRONT dedicated server and two headless clients as separate
SparkEngine processes on a free loopback UDP port, drives every client through
the real onboarding path (register, login, character create, enter world,
faction, spawn) and a scripted scenario, and has every process print its world
view with tf_observe. The comparator then proves that at each checkpoint both
clients agree with the server on the in-world players (faction, class, health,
position), the territory map, every vehicle (kind, driver, hp, position) and
their own rank and flux; a per-scenario verdict then checks the authoritative
outcome itself (a kill and its attribution, a rejected and audited forgery, a
restored character, a flipped region, a vehicle's life cycle).

Scenarios (SCENARIOS): onboard_spawn_move, combat_kill_respawn, forged_state,
reconnect, territory, vehicle_lifecycle.

Checkpoints are quiet windows: nothing changes the world for several seconds
around them, so the server and client views must match within
POSITION_TOLERANCE_M even though the processes observe at slightly different
wall-clock instants. The server observes every SERVER_OBSERVE_INTERVAL_S; each
client checkpoint is paired with the server observation nearest to it in
wall-clock time, anchored by when each process's -exec clock started (its audit
file's first entry). QUIET_MARGIN_S is the widest instant spread the comparator
accepts (client skew plus server pairing gap); every scenario keeps each
checkpoint further than that from any scripted change, and the harness refuses a
schedule that does not, so an accepted skew can never catch the world mid-change
and report it as divergence.

Server steps. The server's -exec script is written before any client exists, so
it can neither name player ids nor know how far behind it the clients' clocks
start. A server step therefore addresses players by faction, is idempotent, and
is written as a client-clock window [start, end]: the server repeats it every
SERVER_STEP_PERIOD_S from server time start + CLIENT_LAG_MAX_S to
end + CLIENT_LAG_MIN_S. For any client lag inside [CLIENT_LAG_MIN_S,
CLIENT_LAG_MAX_S] every repeat lands inside [start, end] on the client clock, so
the quiet-window rule covers it. The run measures the real lag and fails when it
falls outside those bounds instead of trusting a schedule it did not meet.

A check that stops checking must not pass: a child that exits non-zero, a
missing or partial audit file, a scripted command that reports ERR, fewer
complete observations than the scenario schedules, a server observation without
its per-player progression lines, or a scenario verdict that lacks the views it
needs all fail the run.

Usage:
  multiclient.py --engine <SparkEngine> --module <SparkGameMMOFPS> \
      --scenario onboard_spawn_move --workdir <dir> [--cwd <asset root>]

Exit status 0 means every checkpoint converged and the scenario verdict held;
the JSON summary on stdout carries the per-checkpoint verdicts either way.
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
from typing import Callable

POSITION_TOLERANCE_M = 1.5
MIN_MOVE_M = 1.0
MAX_CLIENT_SKEW_S = 2.0
MAX_SERVER_PAIRING_GAP_S = 1.0
# A client observes up to MAX_CLIENT_SKEW_S after the other and the server sample
# may sit MAX_SERVER_PAIRING_GAP_S further out, so a checkpoint must be quiet for
# longer than both together on each side. MOVE_SETTLE_S covers replication and
# interpolation after a scripted walk stops; EVENT_SETTLE_S after any other change.
QUIET_MARGIN_S = MAX_CLIENT_SKEW_S + MAX_SERVER_PAIRING_GAP_S
MOVE_SETTLE_S = 0.5
EVENT_SETTLE_S = 0.5
SERVER_OBSERVE_INTERVAL_S = 0.5
SERVER_TAIL_S = 30.0
SERVER_READY_TIMEOUT_S = 60.0
POLL_INTERVAL_S = 0.05
# How far a client's -exec clock may start behind the server's (it launches once
# the server listens, then boots). Server steps are scheduled to be correct for
# any lag inside these bounds; evaluate() fails a run whose lag fell outside.
CLIENT_LAG_MIN_S = 0.5
CLIENT_LAG_MAX_S = 10.5
SERVER_STEP_PERIOD_S = 1.0
# Flux income (continent fluxTickSec) can land between the server's and a
# client's observation, or between two snapshots: one tick is +1 base plus the
# held-region bonus, at most 5 at the initial territory map (regions.json).
FLUX_INCOME_SLACK = 8
FACTION_IDS = {"mra": 1, "auc": 2, "hlx": 3}
NO_PLAYER = 0xFFFFFFFF  # kInvalidPlayer: an empty seat
# World/TFSanctuaryZone.h: the no-damage staging rectangle every first spawn lands in.
SANCTUARY_MIN_X, SANCTUARY_MAX_X, SANCTUARY_MIN_Z, SANCTUARY_MAX_Z = 40.0, 600.0, 3496.0, 4056.0

OBSERVE_TAG = "[TF-OBSERVE] "
AUDIT_HEADER = re.compile(r"^frame (\d+) t=(\d+(?:\.\d+)?)s \| (ok |ERR) \| (.*)$")
KEY_VALUE = re.compile(r"(\w+)=(\S*)")
FORGED_AUDIT = re.compile(r"\[TF-AUDIT\] forged-state kind=(\S+) player=(\d+)")
CHEAT_STATS_ROW = re.compile(r"\bp(\d+)\s+moveClamps=.*\bforged=(\d+)")


@dataclass(frozen=True)
class Pawn:
    player: int
    faction: int
    cls: int
    health: int
    pos: tuple[float, float, float]


@dataclass(frozen=True)
class Vehicle:
    net: int
    kind: int
    driver: int
    hp: int
    pos: tuple[float, float, float]


@dataclass(frozen=True)
class Progress:
    """Saved primary, wallet, rank and kill tally of one player."""

    player: int
    loadout: str
    flux: int
    rank: int
    kills: int = 0


@dataclass
class Observation:
    role: str
    self_id: int
    continent: str
    expected_pawns: int
    expected_regions: int
    expected_vehicles: int
    pawns: dict[int, Pawn] = field(default_factory=dict)
    regions: dict[int, int] = field(default_factory=dict)
    vehicles: dict[int, Vehicle] = field(default_factory=dict)
    players: dict[int, Progress] = field(default_factory=dict)  # authority only
    self_progress: Progress | None = None

    def complete(self) -> bool:
        counts = (len(self.pawns) == self.expected_pawns and len(self.regions) == self.expected_regions and
                  len(self.vehicles) == self.expected_vehicles)
        # The authority prints one progression line per live pawn; a server view
        # without them could not check any progression claim.
        progression = self.role == "client" or set(self.players) == set(self.pawns)
        return self.self_progress is not None and counts and progression


@dataclass
class AuditEntry:
    frame: int
    seconds: float
    ok: bool
    command: str
    output: list[str] = field(default_factory=list)


@dataclass(frozen=True)
class ServerStep:
    """An idempotent server command repeated across a client-clock window (module docstring)."""

    start: float
    end: float
    command: str
    changes_world: bool = True


@dataclass(frozen=True)
class Scenario:
    """One scripted multi-client run.

    client_steps run on every client and solo_steps (client index, seconds,
    command) on one; both are command templates on the client clock. absent
    lists (checkpoint, client) pairs whose pawn must be missing from every view;
    offline pairs are also disconnected, so that client's own view is not read.
    """

    name: str
    client_factions: tuple[str, ...]
    client_steps: tuple[tuple[float, str], ...]
    checkpoints: tuple[float, ...]
    client_seconds: float
    moves: tuple[tuple[int, int], ...] = ()
    solo_steps: tuple[tuple[int, float, str], ...] = ()
    server_steps: tuple[ServerStep, ...] = ()
    absent: tuple[tuple[int, int], ...] = ()
    offline: tuple[tuple[int, int], ...] = ()
    walk_settle_s: float = MOVE_SETTLE_S


ONBOARDING = (
    (1.0, "tf_connect 127.0.0.1:{port}"),
    (3.0, "tf_register {user} {password}"),
    (4.5, "tf_login {user} {password}"),
    (6.0, "tf_char_create {name} {faction}"),
    (7.5, "tf_char_list"),
    (9.0, "tf_enter 0"),
    (10.5, "tf_faction {faction}"),
    (11.5, "tf_spawn"),
)

# combat_kill_respawn: open continent ground far from every region, 8 m apart.
ARENA_MRA = (1700.0, 2700.0)
ARENA_AUC = (1708.0, 2700.0)
ARENA_MAX_SEPARATION_M = 20.0
# forged_state / reconnect: a legal saved primary, then one the server must refuse.
FORGED_LEGAL_PRIMARY = "mra_rifle"
RECONNECT_LEGAL_PRIMARY = "auc_rifle"
RECONNECT_FORGED_PRIMARY = "mra_rifle"
# territory: an outpost the MRA holds at boot (regions.json initialOwnership).
TERRITORY_REGION = 3
TERRITORY_OWNERS = (FACTION_IDS["mra"], FACTION_IDS["auc"], FACTION_IDS["mra"])
# vehicle_lifecycle: next to the MRA skyanchor terminal (2048, 3520) within the
# Drifter's enter reach of the pad it spawns on; the AUC pawn watches from 13 m.
VEHICLE_TERMINAL_MRA = (2052.0, 3520.0)
VEHICLE_WATCHER_AUC = (2056.0, 3530.0)
DRIFTER_KIND = 1  # VehicleId::Drifter
DRIFTER_FLUX_COST = 50  # vehicles.json
VEHICLE_FLUX_FLOOR = 200
VEHICLE_PHASES = ("before", "purchased", "entered", "driven", "exited", "destroyed")


def _volley(client: int, start: float, spacing: float, shots: int) -> tuple[tuple[int, float, str], ...]:
    return tuple((client, round(start + spacing * i, 1), "tf_fire") for i in range(shots))


SCENARIOS = {
    "onboard_spawn_move": Scenario(
        name="onboard_spawn_move",
        client_factions=("mra", "auc"),
        client_steps=(*ONBOARDING, (20.5, "tf_walk 1 0 2"), (30.5, "tf_walk 0 1 2")),
        checkpoints=(17.0, 27.0, 37.0),
        moves=((0, 1), (1, 2)),
        client_seconds=40.0,
    ),
    # A (MRA) wounds, then kills B (AUC) on the continent; B respawns and is
    # brought back. Checkpoints: arena, wounded, dead, respawned.
    "combat_kill_respawn": Scenario(
        name="combat_kill_respawn",
        client_factions=("mra", "auc"),
        client_steps=ONBOARDING,
        solo_steps=(
            (0, 31.5, "tf_aim_at enemy"),
            *_volley(0, 32.0, 0.3, 6),
            (0, 41.0, "tf_aim_at enemy"),
            *_volley(0, 41.5, 0.2, 25),
            (1, 56.0, "tf_spawn"),
        ),
        server_steps=(
            ServerStep(13.0, 24.0, "tf_place_faction mra {:.0f} {:.0f}".format(*ARENA_MRA)),
            ServerStep(13.0, 24.0, "tf_place_faction auc {:.0f} {:.0f}".format(*ARENA_AUC)),
            ServerStep(58.0, 69.0, "tf_place_faction auc {:.0f} {:.0f}".format(*ARENA_AUC)),
        ),
        checkpoints=(28.0, 37.5, 50.0, 73.0),
        absent=((2, 1),),
        client_seconds=76.0,
    ),
    # A saves a legal primary, then sends a foreign-faction and an unknown weapon.
    "forged_state": Scenario(
        name="forged_state",
        client_factions=("mra", "auc"),
        client_steps=ONBOARDING,
        solo_steps=(
            (0, 14.0, f"tf_give {FORGED_LEGAL_PRIMARY}"),
            (0, 22.0, "tf_give auc_rifle"),
            (0, 24.0, "tf_give_raw 32766"),
        ),
        server_steps=(ServerStep(25.5, 36.0, "tf_cheat_stats", changes_world=False),),
        checkpoints=(18.5, 28.5),
        client_seconds=40.0,
    ),
    # B saves a legal primary, tries a forged one, leaves, and comes back in the
    # same process through tf_connect / tf_login / tf_enter.
    "reconnect": Scenario(
        name="reconnect",
        client_factions=("mra", "auc"),
        client_steps=ONBOARDING,
        solo_steps=(
            (1, 14.0, f"tf_give {RECONNECT_LEGAL_PRIMARY}"),
            (1, 16.0, f"tf_give {RECONNECT_FORGED_PRIMARY}"),
            (1, 24.0, "tf_disconnect"),
            (1, 32.0, "tf_connect 127.0.0.1:{port}"),
            (1, 34.0, "tf_login {user} {password}"),
            (1, 35.5, "tf_char_list"),
            (1, 37.0, "tf_enter 0"),
            (1, 38.5, "tf_faction {faction}"),
            (1, 39.5, "tf_spawn"),
        ),
        checkpoints=(20.5, 28.5, 44.0),
        offline=((1, 1),),
        client_seconds=47.0,
    ),
    # The server flips an outpost to AUC and back.
    "territory": Scenario(
        name="territory",
        client_factions=("mra", "auc"),
        client_steps=ONBOARDING,
        server_steps=(
            ServerStep(20.0, 31.0, f"tf_capture {TERRITORY_REGION} auc"),
            ServerStep(39.0, 50.0, f"tf_capture {TERRITORY_REGION} mra"),
        ),
        checkpoints=(16.0, 35.0, 54.0),
        client_seconds=57.0,
    ),
    # A buys a Drifter, drives it, gets out, and the server destroys it.
    # Checkpoints follow VEHICLE_PHASES.
    "vehicle_lifecycle": Scenario(
        name="vehicle_lifecycle",
        client_factions=("mra", "auc"),
        client_steps=ONBOARDING,
        solo_steps=(
            (0, 31.5, "tf_vehicle_buy drifter"),
            (0, 39.5, "tf_vehicle_seat enter"),
            (0, 47.5, "tf_walk 1 0 2"),
            (0, 61.5, "tf_vehicle_seat exit"),
        ),
        server_steps=(
            ServerStep(13.0, 24.0, "tf_place_faction mra {:.0f} {:.0f}".format(*VEHICLE_TERMINAL_MRA)),
            ServerStep(13.0, 24.0, "tf_place_faction auc {:.0f} {:.0f}".format(*VEHICLE_WATCHER_AUC)),
            ServerStep(13.0, 24.0, f"tf_flux_floor mra {VEHICLE_FLUX_FLOOR}"),
            ServerStep(69.5, 80.5, "tf_damage_vehicles 5000"),
        ),
        checkpoints=(28.0, 36.0, 44.0, 57.5, 66.0, 85.0),
        walk_settle_s=4.0,  # a driven hull coasts after the throttle is released
        client_seconds=88.0,
    ),
}


class HarnessError(RuntimeError):
    pass


def timed_client_steps(scenario: Scenario, client: int) -> list[tuple[float, str]]:
    steps = list(scenario.client_steps)
    steps += [(at, command) for index, at, command in scenario.solo_steps if index == client]
    return sorted(steps, key=lambda item: item[0])


def motion_windows(scenario: Scenario) -> list[tuple[float, float]]:
    """Client-clock intervals in which the world may change: every scripted step plus its settle time."""
    windows = []
    for client in range(len(scenario.client_factions)):
        for at, template in timed_client_steps(scenario, client):
            verb, *arguments = template.split()
            if verb == "tf_walk":
                windows.append((at, at + float(arguments[2]) + scenario.walk_settle_s))
            else:
                windows.append((at, at + EVENT_SETTLE_S))
    windows += [(step.start, step.end + EVENT_SETTLE_S) for step in scenario.server_steps if step.changes_world]
    return sorted(set(windows))


def schedule_violations(scenario: Scenario) -> list[str]:
    """Checkpoints an accepted skew could split, and server steps no allowed lag can place in their window."""
    violations = []
    for index, checkpoint in enumerate(scenario.checkpoints):
        for start, end in motion_windows(scenario):
            gap = max(start - checkpoint, checkpoint - end, 0.0)
            if gap <= QUIET_MARGIN_S:
                violations.append(f"{scenario.name}: checkpoint {index} (t={checkpoint}) is {gap:.1f} s from motion "
                                  f"{start}-{end}; it must be quiet for more than {QUIET_MARGIN_S} s")
    for step in scenario.server_steps:
        if step.end - step.start < CLIENT_LAG_MAX_S - CLIENT_LAG_MIN_S:
            violations.append(f"{scenario.name}: server step '{step.command}' window {step.start}-{step.end} is "
                              f"narrower than the {CLIENT_LAG_MAX_S - CLIENT_LAG_MIN_S} s client lag range")
        if step.end > scenario.client_seconds:
            violations.append(f"{scenario.name}: server step '{step.command}' ends after the clients exit")
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


def _position(text: str) -> tuple[float, float, float]:
    x, y, z = (float(v) for v in text.split(","))
    return (x, y, z)


def _parse_body_line(current: Observation, kind: str, values: dict[str, str]) -> None:
    if kind == "self":
        current.self_progress = Progress(int(values["id"]), values["loadout"], int(values["flux"]),
                                         int(values["rank"]))
    elif kind == "pawn":
        pawn = Pawn(int(values["id"]), int(values["faction"]), int(values["class"]), int(values["health"]),
                    _position(values["pos"]))
        current.pawns[pawn.player] = pawn
    elif kind == "region":
        current.regions[int(values["id"])] = int(values["owner"])
    elif kind == "vehicle":
        vehicle = Vehicle(int(values["net"]), int(values["def"]), int(values["driver"]), int(values["hp"]),
                          _position(values["pos"]))
        current.vehicles[vehicle.net] = vehicle
    elif kind == "player":
        progress = Progress(int(values["id"]), values["loadout"], int(values["flux"]), int(values["rank"]),
                            int(values["kills"]))
        current.players[progress.player] = progress


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
        try:
            if kind.startswith("role="):
                current = None
                header = _fields(body)
                current = Observation(role=header["role"], self_id=int(header["self"]),
                                      continent=header["continent"], expected_pawns=int(header["pawns"]),
                                      expected_regions=int(header["regions"]),
                                      expected_vehicles=int(header["vehicles"]))
                observations.append(current)
            elif current is not None:
                _parse_body_line(current, kind, _fields(rest))
        except (KeyError, ValueError):
            if current is not None:
                observations.pop()  # a malformed line invalidates the observation it belongs to
            current = None
    return [obs for obs in observations if obs.complete()]


def command_outputs(entries: list[AuditEntry], command: str) -> list[tuple[float, list[str]]]:
    """(seconds, output lines) for every execution of @p command, scoped to its own output.

    The audit appends the console's most recent entries after each command, so
    a block usually still holds the PREVIOUS command's output. Only lines after
    this command's own "[exec] frame N" echo belong to it; without that scoping
    a probe that printed nothing would inherit a stale result.
    """
    outputs = []
    for entry in entries:
        if entry.command.strip() != command:
            continue
        echo = f"[exec] frame {entry.frame} "
        starts = [i for i, line in enumerate(entry.output) if line.startswith(echo) and line.endswith(f": {command}")]
        outputs.append((entry.seconds, entry.output[starts[-1] + 1:] if starts else []))
    return outputs


@dataclass
class Sample:
    seconds: float
    observation: Observation | None


def observe_samples(entries: list[AuditEntry]) -> list[Sample]:
    """One sample per executed tf_observe: the complete observation it printed, or None."""
    samples = []
    for seconds, lines in command_outputs(entries, "tf_observe"):
        observed = parse_observations(lines)
        samples.append(Sample(seconds, observed[0] if observed else None))
    return samples


def forged_audit_records(entries: list[AuditEntry]) -> set[tuple[str, int]]:
    """Every (kind, player) the server's "[TF-AUDIT] forged-state" log lines named."""
    records = set()
    for entry in entries:
        for line in entry.output:
            for match in FORGED_AUDIT.finditer(line):
                records.add((match.group(1), int(match.group(2))))
    return records


def cheat_stats_snapshots(entries: list[AuditEntry]) -> list[dict[int, int]]:
    """Per tf_cheat_stats run: player -> forged-state reject count."""
    snapshots = []
    for _, lines in command_outputs(entries, "tf_cheat_stats"):
        rows = {}
        for line in lines:
            row = CHEAT_STATS_ROW.search(line)
            if row:
                rows[int(row.group(1))] = int(row.group(2))
        snapshots.append(rows)
    return snapshots


# --------------------------------------------------------------------------- comparison


@dataclass
class RoleLog:
    role: str
    returncode: int | None
    anchor: float | None  # wall-clock (monotonic) seconds at which the -exec clock started
    entries: list[AuditEntry]
    faction: str | None = None


@dataclass
class RunViews:
    """What every process saw at each checkpoint, for the scenario verdicts."""

    server: list[Observation | None]
    clients: list[list[Observation | None]]  # [client][checkpoint]
    server_entries: list[AuditEntry]

    def self_id(self, client: int, checkpoint: int) -> int | None:
        view = self.clients[client][checkpoint]
        return view.self_id if view is not None else None


def in_sanctuary(pos: tuple[float, float, float]) -> bool:
    return SANCTUARY_MIN_X <= pos[0] <= SANCTUARY_MAX_X and SANCTUARY_MIN_Z <= pos[2] <= SANCTUARY_MAX_Z


def compare_regions(server: Observation, client: Observation, label: str) -> list[str]:
    """The territory digest: every region owner the client shows must be the server's."""
    if client.regions == server.regions:
        return []
    differing = sorted(r for r in set(server.regions) | set(client.regions)
                       if client.regions.get(r) != server.regions.get(r))
    return [f"{label}: region owners differ from the server for regions {differing}"]


def compare_vehicles(server: Observation, client: Observation, label: str) -> list[str]:
    problems = []
    if set(client.vehicles) != set(server.vehicles):
        return [f"{label}: vehicles {sorted(client.vehicles)} != server {sorted(server.vehicles)}"]
    for net, truth in server.vehicles.items():
        seen = client.vehicles[net]
        if (seen.kind, seen.driver, seen.hp) != (truth.kind, truth.driver, truth.hp):
            problems.append(f"{label}: vehicle {net} kind/driver/hp {(seen.kind, seen.driver, seen.hp)} != server "
                            f"{(truth.kind, truth.driver, truth.hp)}")
        gap = math.dist(seen.pos, truth.pos)
        if gap > POSITION_TOLERANCE_M:
            problems.append(f"{label}: vehicle {net} position {seen.pos} is {gap:.2f} m from server {truth.pos}")
    return problems


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
    problems += compare_regions(server, client, label)
    problems += compare_vehicles(server, client, label)
    # The client's own rank and wallet. Its saved loadout is server-only state
    # (a pure client holds none), so the scenario verdicts read that from the
    # server's player lines instead.
    truth = server.players.get(client.self_id)
    mine = client.self_progress
    if truth is not None and mine is not None:
        if mine.rank != truth.rank:
            problems.append(f"{label}: own rank {mine.rank} != server {truth.rank}")
        if abs(mine.flux - truth.flux) > FLUX_INCOME_SLACK:
            problems.append(f"{label}: own flux {mine.flux} != server {truth.flux}")
    return problems


def check_client_lag(scenario: Scenario, server: RoleLog, clients: list[RoleLog]) -> list[str]:
    if not scenario.server_steps or server.anchor is None:
        return []
    problems = []
    for client in clients:
        if client.anchor is None:
            continue
        lag = client.anchor - server.anchor
        if not CLIENT_LAG_MIN_S <= lag <= CLIENT_LAG_MAX_S:
            problems.append(f"{client.role}: its clock started {lag:.2f} s after the server's; the server steps are "
                            f"scheduled for a lag in [{CLIENT_LAG_MIN_S}, {CLIENT_LAG_MAX_S}] s")
    return problems


def evaluate_checkpoint(scenario: Scenario, index: int, server: RoleLog, clients: list[RoleLog],
                        client_samples: list[list[Sample]], server_samples: list[Sample],
                        known_ids: list[int | None]) -> tuple[dict, Observation | None, list[Observation | None]]:
    """Compare one checkpoint; returns (verdict, server view, per-client views)."""
    verdict: dict = {"checkpoint": index, "problems": []}
    offline = {client for checkpoint, client in scenario.offline if checkpoint == index}
    absent = offline | {client for checkpoint, client in scenario.absent if checkpoint == index}
    views: list[Observation | None] = [None] * len(clients)
    walls: list[float] = []
    for number, (client, samples) in enumerate(zip(clients, client_samples)):
        if number in offline:
            continue
        if index >= len(samples) or samples[index].observation is None or client.anchor is None:
            verdict["problems"].append(f"{client.role}: checkpoint {index} was not observed")
            continue
        views[number] = samples[index].observation
        walls.append(client.anchor + samples[index].seconds)

    online = len(clients) - len(offline)
    server_view = None
    if len(walls) == online and online > 0 and server.anchor is not None:
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

    if server_view is not None:
        verdict["server_players"] = sorted(server_view.pawns)
        expected = len(clients) - len(absent)
        if len(server_view.pawns) != expected:
            verdict["problems"].append(f"server: {len(server_view.pawns)} players in world, expected {expected}")
        selves = [view.self_id for view in views if view is not None]
        if len(set(selves)) != len(selves):
            verdict["problems"].append(f"clients: player ids {selves} are not distinct")
        for number, (client, view) in enumerate(zip(clients, views)):
            player = view.self_id if view is not None else known_ids[number]
            if number in absent:
                if player is not None and player in server_view.pawns:
                    verdict["problems"].append(f"{client.role}: player {player} should be out of the world")
            elif view is not None:
                pawn = server_view.pawns.get(view.self_id)
                if pawn is None:
                    verdict["problems"].append(f"{client.role}: own player {view.self_id} has no pawn on the server")
                elif client.faction is not None and pawn.faction != FACTION_IDS[client.faction]:
                    verdict["problems"].append(f"{client.role}: server faction {pawn.faction} != chosen "
                                               f"{client.faction}")
            if view is not None:
                verdict["problems"].extend(compare_views(server_view, view, client.role))
        verdict["client_players"] = selves
    verdict["converged"] = not verdict["problems"]
    return verdict, server_view, views


def evaluate(scenario: Scenario, server: RoleLog, clients: list[RoleLog]) -> dict:
    """Pair every scheduled client checkpoint with a server observation, compare them, then run the verdict."""
    problems: list[str] = []
    for log in [server, *clients]:
        if log.returncode != 0:
            problems.append(f"{log.role}: exited with {log.returncode}")
        for entry in log.entries:
            if not entry.ok:
                problems.append(f"{log.role}: scripted command reported ERR: {entry.command}")
        if log.anchor is None:
            problems.append(f"{log.role}: no audit trail was written")
    problems += check_client_lag(scenario, server, clients)

    server_samples = [s for s in observe_samples(server.entries) if s.observation is not None]
    client_samples = [observe_samples(c.entries) for c in clients]
    checkpoints: list[dict] = []
    views = RunViews([], [[] for _ in clients], server.entries)
    known_ids: list[int | None] = [None] * len(clients)

    for index in range(len(scenario.checkpoints)):
        verdict, server_view, client_views = evaluate_checkpoint(scenario, index, server, clients, client_samples,
                                                                 server_samples, known_ids)
        checkpoints.append(verdict)
        views.server.append(server_view)
        for number, view in enumerate(client_views):
            views.clients[number].append(view)
            if view is not None:
                known_ids[number] = view.self_id

    for before, after in scenario.moves:
        first = views.server[before] if before < len(views.server) else None
        second = views.server[after] if after < len(views.server) else None
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
    outcome = VERDICTS.get(scenario.name)
    if outcome is not None:
        problems += outcome(views)
    return {"scenario": scenario.name, "passed": not problems, "problems": problems, "checkpoints": checkpoints}


# --------------------------------------------------------------------------- scenario verdicts


def _require_views(views: RunViews, clients: tuple[int, ...], name: str) -> list[str]:
    """A verdict cannot run on missing views: every server checkpoint and each named client's must exist."""
    missing = [f"{name}: server checkpoint {i} is missing" for i, v in enumerate(views.server) if v is None]
    for client in clients:
        missing += [f"{name}: client{client + 1} checkpoint {i} is missing"
                    for i, v in enumerate(views.clients[client]) if v is None]
    return missing


def combat_verdict(views: RunViews) -> list[str]:
    """client1 wounds then kills client2; the server credits the kill to client1; client2 respawns whole."""
    attacker, victim = 0, 1
    missing = _require_views(views, (attacker,), "combat")
    missing += [f"combat: client2 checkpoint {i} is missing" for i in (0, 1, 3) if views.clients[victim][i] is None]
    if missing:
        return missing
    problems = []
    a = views.self_id(attacker, 0)
    b = views.self_id(victim, 0)
    arena, wounded, dead, respawned = views.server
    if a not in arena.pawns or b not in arena.pawns:
        return [f"combat: players {a} and {b} are not both in the world at the arena checkpoint"]
    for player in (a, b):
        if in_sanctuary(arena.pawns[player].pos):
            problems.append(f"combat: player {player} never left the no-damage sanctuary")
    if math.dist(arena.pawns[a].pos, arena.pawns[b].pos) > ARENA_MAX_SEPARATION_M:
        problems.append(f"combat: players {a} and {b} are more than {ARENA_MAX_SEPARATION_M} m apart")
    full = arena.pawns[b].health
    if b not in wounded.pawns or not 0 < wounded.pawns[b].health < full:
        seen = wounded.pawns[b].health if b in wounded.pawns else "no pawn"
        problems.append(f"combat: player {b} was not wounded by the first volley (health {seen}, full {full})")
    kills = arena.players[a].kills
    if a not in dead.players or dead.players[a].kills != kills + 1:
        seen = dead.players[a].kills if a in dead.players else "no pawn"
        problems.append(f"combat: the kill was not credited to player {a} (kills {kills} -> {seen})")
    b_again = views.self_id(victim, 3)
    if b_again not in respawned.pawns or respawned.pawns[b_again].health != full:
        seen = respawned.pawns[b_again].health if b_again in respawned.pawns else "no pawn"
        problems.append(f"combat: player {b_again} did not respawn at full health (health {seen}, full {full})")
    return problems


def forged_verdict(views: RunViews) -> list[str]:
    """client1's forged loadouts are rejected (saved primary unchanged) AND audited (log line plus counter)."""
    missing = _require_views(views, (0, 1), "forged")
    if missing:
        return missing
    problems = []
    a = views.self_id(0, 0)
    b = views.self_id(1, 0)
    for index, view in enumerate(views.server):
        saved = view.players.get(a)
        if saved is None or saved.loadout != FORGED_LEGAL_PRIMARY:
            problems.append(f"forged: checkpoint {index} server loadout of player {a} is "
                            f"{saved.loadout if saved else 'missing'}, expected {FORGED_LEGAL_PRIMARY}")
        other = view.players.get(b)
        if other is None or other.loadout != "default":
            problems.append(f"forged: player {b} was affected (loadout {other.loadout if other else 'missing'})")
    audited = forged_audit_records(views.server_entries)
    for kind in ("loadout-ineligible", "loadout-unknown-weapon"):
        if (kind, a) not in audited:
            problems.append(f"forged: no [TF-AUDIT] forged-state kind={kind} line for player {a}")
    if any(player == b for _, player in audited):
        problems.append(f"forged: player {b} was audited without forging anything")
    snapshots = [rows for rows in cheat_stats_snapshots(views.server_entries) if a in rows]
    if not snapshots:
        problems.append(f"forged: no tf_cheat_stats run listed player {a}")
    elif snapshots[-1][a] != 2 or snapshots[-1].get(b, 0) != 0:
        problems.append(f"forged: forged-state counters {snapshots[-1]} (expected {a}: 2, {b}: 0)")
    return problems


def reconnect_verdict(views: RunViews) -> list[str]:
    """client2's saved state after leaving and re-entering equals its non-default pre-disconnect state."""
    returning = 1
    missing = _require_views(views, (0,), "reconnect")
    missing += [f"reconnect: client2 checkpoint {i} is missing" for i in (0, 2) if views.clients[returning][i] is None]
    if missing:
        return missing
    before_id = views.self_id(returning, 0)
    after_id = views.self_id(returning, 2)
    before = views.server[0].players.get(before_id)
    after = views.server[2].players.get(after_id)
    if before is None or after is None:
        return [f"reconnect: server progression for player {before_id} -> {after_id} is missing"]
    problems = []
    # An empty save restores default kit; a snapshot equal to it could not tell.
    if (before.loadout, before.flux, before.rank) == ("default", 0, 1):
        problems.append("reconnect: the pre-disconnect state is default kit, so restoring it proves nothing")
    if before.loadout != RECONNECT_LEGAL_PRIMARY:
        problems.append(f"reconnect: pre-disconnect loadout {before.loadout}, expected {RECONNECT_LEGAL_PRIMARY}")
    if after.loadout != before.loadout:
        problems.append(f"reconnect: loadout {after.loadout} was restored, expected {before.loadout}")
    if after.rank != before.rank:
        problems.append(f"reconnect: rank {after.rank} was restored, expected {before.rank}")
    if not before.flux <= after.flux <= before.flux + FLUX_INCOME_SLACK:
        problems.append(f"reconnect: flux {after.flux} was restored, expected {before.flux}")
    faction_before = views.server[0].pawns[before_id].faction
    faction_after = views.server[2].pawns[after_id].faction
    if faction_after != faction_before:
        problems.append(f"reconnect: faction {faction_after} was restored, expected {faction_before}")
    return problems


def territory_verdict(views: RunViews) -> list[str]:
    """The flipped region's owner follows the server commands (clients' agreement is compared per checkpoint)."""
    missing = _require_views(views, (0, 1), "territory")
    if missing:
        return missing
    problems = []
    for index, (view, owner) in enumerate(zip(views.server, TERRITORY_OWNERS)):
        if view.regions.get(TERRITORY_REGION) != owner:
            problems.append(f"territory: checkpoint {index} region {TERRITORY_REGION} owner "
                            f"{view.regions.get(TERRITORY_REGION)}, expected {owner}")
    return problems


def vehicle_phase_problems(phase: str, view: Observation, previous: Observation | None, driver: int,
                           flux_before: int | None) -> list[str]:
    """One step of the vehicle state machine. A vehicle that never appears fails every phase after "before"."""
    vehicles = list(view.vehicles.values())
    if phase == "before":
        wallet = view.players.get(driver)
        problems = [f"vehicle: {len(vehicles)} vehicles before the purchase"] if vehicles else []
        if wallet is None or wallet.flux < DRIFTER_FLUX_COST:
            problems.append(f"vehicle: player {driver} cannot afford a Drifter "
                            f"(flux {wallet.flux if wallet else 'missing'})")
        return problems
    if phase == "destroyed":
        return [f"vehicle: {len(vehicles)} vehicles left after destruction"] if vehicles else []
    if len(vehicles) != 1:
        return [f"vehicle: {phase}: expected exactly one vehicle, found {len(vehicles)}"]
    vehicle = vehicles[0]
    problems = []
    if vehicle.kind != DRIFTER_KIND or vehicle.hp <= 0:
        problems.append(f"vehicle: {phase}: kind {vehicle.kind} hp {vehicle.hp}, expected a live Drifter")
    seated = phase in ("entered", "driven")
    if vehicle.driver != (driver if seated else NO_PLAYER):
        problems.append(f"vehicle: {phase}: driver {vehicle.driver}, expected {driver if seated else 'none'}")
    if phase == "purchased":
        wallet = view.players.get(driver)
        spent = None if wallet is None or flux_before is None else flux_before - wallet.flux
        if spent is None or not DRIFTER_FLUX_COST - FLUX_INCOME_SLACK <= spent <= DRIFTER_FLUX_COST:
            problems.append(f"vehicle: the purchase took {spent} flux, expected {DRIFTER_FLUX_COST}")
    if phase == "driven" and (previous is None or vehicle.net not in previous.vehicles or
                              math.dist(vehicle.pos, previous.vehicles[vehicle.net].pos) < MIN_MOVE_M):
        problems.append(f"vehicle: the Drifter did not move {MIN_MOVE_M} m while driven")
    if phase == "exited" and driver not in view.pawns:
        problems.append(f"vehicle: player {driver} did not survive leaving the vehicle")
    return problems


def vehicle_verdict(views: RunViews) -> list[str]:
    """client1 buys, boards, drives and leaves a Drifter that the server then destroys (VEHICLE_PHASES)."""
    missing = _require_views(views, (0, 1), "vehicle")
    if missing:
        return missing
    driver = views.self_id(0, 0)
    before = views.server[0].players.get(driver)
    flux_before = before.flux if before is not None else None
    problems = []
    previous = None
    for phase, view in zip(VEHICLE_PHASES, views.server):
        problems += vehicle_phase_problems(phase, view, previous, driver, flux_before)
        previous = view
    return problems


VERDICTS: dict[str, Callable[[RunViews], list[str]]] = {
    "combat_kill_respawn": combat_verdict,
    "forged_state": forged_verdict,
    "reconnect": reconnect_verdict,
    "territory": territory_verdict,
    "vehicle_lifecycle": vehicle_verdict,
}


# --------------------------------------------------------------------------- processes


def free_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def format_seconds(seconds: float) -> str:
    return f"{seconds:.1f}"


def server_step_times(step: ServerStep) -> list[float]:
    """Server-clock times a step runs at: every repeat lands in [start, end] for any allowed client lag."""
    times = []
    at = step.start + CLIENT_LAG_MAX_S
    while at <= step.end + CLIENT_LAG_MIN_S + 1e-9:
        times.append(at)
        at += SERVER_STEP_PERIOD_S
    return times


def server_script(port: int, run_seconds: float, scenario: Scenario | None = None) -> str:
    timeline = [(0.5, f"tf_dedicated {port}")]
    at = 1.0
    while at < run_seconds - 0.5:
        timeline.append((at, "tf_observe"))
        at += SERVER_OBSERVE_INTERVAL_S
    for step in scenario.server_steps if scenario else ():
        timeline += [(when, step.command) for when in server_step_times(step)]
    timeline.sort(key=lambda item: item[0])
    return "0 tf_status\n" + "".join(f"t{format_seconds(when)} {command}\n" for when, command in timeline)


def client_script(scenario: Scenario, port: int, faction: str, index: int = 0) -> str:
    values = {
        "port": port,
        "faction": faction,
        "user": "mc" + secrets.token_hex(6),
        "password": secrets.token_urlsafe(18),
        "name": "MC" + secrets.token_hex(5),
    }
    timeline = [(at, template.format(**values)) for at, template in timed_client_steps(scenario, index)]
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
    violations = schedule_violations(scenario)
    if violations:
        raise HarnessError("; ".join(violations))
    workdir = Path(args.workdir).resolve()
    workdir.mkdir(parents=True, exist_ok=True)
    port = free_udp_port()
    deadline = time.monotonic() + args.timeout
    server_seconds = scenario.client_seconds + SERVER_TAIL_S
    children: list[Child] = []
    try:
        server = launch("server", args, workdir, server_script(port, server_seconds, scenario), server_seconds)
        children.append(server)
        wait_for_server(server, min(deadline, time.monotonic() + SERVER_READY_TIMEOUT_S))
        for index, faction in enumerate(scenario.client_factions):
            script = client_script(scenario, port, faction, index)
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
