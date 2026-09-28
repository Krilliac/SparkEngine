#!/usr/bin/env python3
"""TF-110: dedicated server + two headless clients convergence harness.

Launches one TERRAFRONT dedicated server and two headless clients as separate
SparkEngine processes on a free loopback UDP port, drives every client through
the real onboarding path (register, login, character create, enter world,
faction, spawn) and a scripted scenario, and has every process print its world
view with tf_observe. The comparator then proves that at each checkpoint both
clients agree with the server on the in-world players (faction, class, health,
position), the territory map and every vehicle (kind, driver, hp, position); a
per-scenario verdict then checks the authoritative outcome itself (a kill and
its attribution, a rejected and audited forgery, a restored character, a
flipped region, a vehicle's life cycle and its price). Rank, wallet and saved
loadout come from the server's per-player lines: a pure client holds no
progression, so its own self line always reads flux=0 rank=1.

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

Impaired runs (--impair, --seed). Every process applies latency, jitter, loss,
duplication and reordering through the engine's net_* commands before it hosts
or connects; settle times and the position tolerance widen with the delay
(impaired()). The run fails unless every process reports the requested values
twice through net_impair AND in the module image's "[TF-OBSERVE] net" line at
every checkpoint (impairment_problems), so impairment that was silently off can
never count as impaired convergence.

Cold restart (cold_restart, ungraceful_restart; TF-120). Phase 1 builds
non-default authoritative state (legal loadout, wallet, a kill, two flipped
regions), then the harness hard-kills the server: after an ok tf_save, or with
no save at all once the progression debounce has passed. Phase 2 starts a new
server on the same TF_SAVE_ROOT, logs the same account back in and requires the
state, the character and exactly one character row to be restored
(restart_verdict).

Continent identity (continent_mismatch; TF-120). A cindral_wastes server and a
client booted with TF_CONTINENT=veyra_highlands: the client must log the
refusal of the server's TF_ContinentIdentity, disconnect, and never own a pawn
on the server although its script asks to spawn (continent_mismatch_verdict).

Soak (--soak-seconds; TF-110). A server with bots and two looping clients is
sampled against soak_budgets.json: tick time (tf_perf), per-client downlink,
replication rate and drops (the net line), and the RSS slope of every process.
The budgets are provisional harness guards, not SLOs.

Usage:
  multiclient.py --engine <SparkEngine> --module <SparkGameMMOFPS> \
      --scenario onboard_spawn_move [territory ...] --workdir <dir> [--cwd <asset root>] \
      [--impair 80,20,0.03,2,3 --seed 20260927]
  multiclient.py ... --soak-seconds 120 --workdir <dir> [--budgets soak_budgets.json]

Exit status 0 means every run passed; the JSON summary on stdout carries the
per-checkpoint verdicts and measurements either way.
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
from dataclasses import dataclass, field, replace
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
# Impaired runs (--impair): the reorder hold is InstabilitySettings::reorderHoldMs,
# the resend allowance covers one reliable retransmit after a dropped packet, and
# the sprint speed (classes.json sprintSpeed) turns a delay into a position slack.
REORDER_HOLD_S = 0.04
RESEND_ALLOWANCE_S = 0.5
MAX_PAWN_SPEED_MPS = 7.2
CHECKPOINT_PAD_S = 0.5
CLIENT_TAIL_S = 2.5
MAX_IMPAIR_SEED = 2147483647  # net_impair_seed accepts [0, INT32_MAX]
IMPAIR_SEED_ROLES = 16
IMPAIR_MIN_PROBES = 2  # one right after configuring, one at the end of the run
IMPAIR_VALUE_TOLERANCE = 0.05  # the simulators print one decimal
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
    """Saved primary, wallet, rank, kill tally, xp and unlock set of one player.

    The server's per-player lines carry every field; a self line carries only
    the first four, so xp and unlocks keep their defaults there.
    """

    player: int
    loadout: str
    flux: int
    rank: int
    kills: int = 0
    xp: int = 0
    unlocks: str = "-"


@dataclass(frozen=True)
class NetView:
    """One process's "[TF-OBSERVE] net" line: transport counters and module-image impairment."""

    bytes_sent: int
    bytes_received: int
    packets_sent: int
    packets_received: int
    packets_dropped: int
    impaired: bool
    lag_ms: float
    jitter_ms: float
    loss_pct: float
    dup_pct: float
    reorder_pct: float
    seed: int


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
    net: NetView | None = None  # absent while the process has no live NetworkManager

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
    event_settle_s: float = EVENT_SETTLE_S
    position_tolerance_m: float = POSITION_TOLERANCE_M


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
                windows.append((at, at + scenario.event_settle_s))
    windows += [(step.start, step.end + scenario.event_settle_s) for step in scenario.server_steps
                if step.changes_world]
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


# --------------------------------------------------------------------------- impairment


@dataclass(frozen=True)
class Impairment:
    """--impair '<lagMs>,<jitterMs>,<lossFrac>,<dupPct>,<reorderPct>' plus --seed.

    Every process applies it through the engine's net_* commands before it
    hosts or connects. Process k seeds the simulator with seed + k so the
    loss/dup/reorder streams differ between processes but every run is
    reproducible.
    """

    lag_ms: float
    jitter_ms: float
    loss: float
    dup_pct: float
    reorder_pct: float
    seed: int

    @classmethod
    def parse(cls, text: str, seed: int) -> "Impairment":
        parts = text.split(",")
        if len(parts) != 5:
            raise ValueError(f"--impair needs 5 comma-separated values, got {text!r}")
        lag, jitter, loss, dup, reorder = (float(part) for part in parts)
        if not all(math.isfinite(v) for v in (lag, jitter, loss, dup, reorder)):
            raise ValueError(f"--impair values must be finite: {text!r}")
        if lag < 0 or jitter < 0 or not 0 <= loss <= 1 or not 0 <= dup <= 100 or not 0 <= reorder <= 100:
            raise ValueError(f"--impair value out of range: {text!r}")
        if lag == jitter == loss == dup == reorder == 0:
            raise ValueError("--impair that impairs nothing would prove nothing")
        if not 1 <= seed <= MAX_IMPAIR_SEED - IMPAIR_SEED_ROLES:
            raise ValueError(f"--seed must lie in [1, {MAX_IMPAIR_SEED - IMPAIR_SEED_ROLES}] (0 is nondeterministic)")
        return cls(lag, jitter, loss, dup, reorder, seed)

    def commands(self, role_index: int) -> list[str]:
        """Engine console commands that apply this impairment, ending with the first status probe."""
        return [f"net_impair_seed {self.seed + role_index}", f"net_lag {self.lag_ms:g}",
                f"net_jitter {self.jitter_ms:g}", f"net_loss {self.loss:g}", f"net_dup {self.dup_pct:g}",
                f"net_reorder {self.reorder_pct:g}", "net_impair"]

    def expected(self, role_index: int) -> tuple[float, float, float, float, float, int]:
        """(lag, jitter, loss %, dup %, reorder %, seed) as the simulators report them."""
        return (self.lag_ms, self.jitter_ms, self.loss * 100.0, self.dup_pct, self.reorder_pct,
                self.seed + role_index)

    def settle_s(self) -> float:
        """Extra settle time after any change: a round trip at the worst delay, the reorder hold, a resend."""
        resend = RESEND_ALLOWANCE_S if self.loss > 0 else 0.0
        return 2.0 * (self.lag_ms + self.jitter_ms) / 1000.0 + REORDER_HOLD_S + resend

    def position_slack_m(self) -> float:
        """How far a sprinting pawn moves during one worst-case one-way delay."""
        return MAX_PAWN_SPEED_MPS * (self.lag_ms + self.jitter_ms) / 1000.0


def impaired(scenario: Scenario, impairment: Impairment) -> Scenario:
    """@p scenario with its settle times and position tolerance widened for @p impairment.

    A checkpoint the wider settle windows no longer keep quiet moves to the
    middle of its quiet gap (or, after the last change, just past the margin);
    schedule_violations still has the last word, so a schedule with no room
    left is refused rather than run.
    """
    extra = impairment.settle_s()
    widened = replace(scenario, walk_settle_s=scenario.walk_settle_s + extra,
                      event_settle_s=scenario.event_settle_s + extra,
                      position_tolerance_m=scenario.position_tolerance_m + impairment.position_slack_m())
    windows = motion_windows(widened)
    checkpoints = []
    for checkpoint in widened.checkpoints:
        before = max((end for start, end in windows if start <= checkpoint), default=0.0)
        after = min((start for start, _ in windows if start > checkpoint), default=None)
        if checkpoint - before <= QUIET_MARGIN_S:
            checkpoint = (before + after) / 2.0 if after is not None else before + QUIET_MARGIN_S + CHECKPOINT_PAD_S
        checkpoints.append(round(checkpoint, 2))
    client_seconds = widened.client_seconds
    if checkpoints:
        client_seconds = max(client_seconds, checkpoints[-1] + CLIENT_TAIL_S)
    return replace(widened, checkpoints=tuple(checkpoints),
                   client_seconds=round(client_seconds, 2))


IMPAIR_STATUS_FIELDS = (
    ("lag", re.compile(r"Latency:\s+([\d.]+) ms")),
    ("jitter", re.compile(r"Jitter:\s+\+/-([\d.]+) ms")),
    ("loss", re.compile(r"Packet loss:\s+([\d.]+)%")),
    ("reorder", re.compile(r"Reorder:\s+([\d.]+)%")),
    ("dup", re.compile(r"Duplicate:\s+([\d.]+)%")),
    ("seed", re.compile(r"Seed:\s+(\d+)")),
)


def parse_impair_status(lines: list[str]) -> tuple[float, float, float, float, float, int] | None:
    """(lag, jitter, loss %, dup %, reorder %, seed) from one net_impair output, or None unless ENABLED."""
    text = "\n".join(lines)
    if "InstabilitySimulator: ENABLED" not in text:
        return None
    values = {}
    for name, pattern in IMPAIR_STATUS_FIELDS:
        match = pattern.search(text)
        if match is None:
            return None
        values[name] = float(match.group(1))
    return (values["lag"], values["jitter"], values["loss"], values["dup"], values["reorder"], int(values["seed"]))


def impair_matches(seen: tuple, expected: tuple) -> bool:
    close = all(abs(a - b) <= IMPAIR_VALUE_TOLERANCE for a, b in zip(seen[:5], expected[:5]))
    return close and seen[5] == expected[5]


def impairment_problems(impairment: Impairment, logs: list["RoleLog"], views: "RunViews") -> list[str]:
    """Impairment must be ON with the requested values in BOTH simulators of every process, for the whole run.

    net_impair reads the simulator the engine's net_* commands configure; the
    module's "[TF-OBSERVE] net" line reads the one compiled into the module
    image, which impairs every module-side send. A run that converged with
    either of them off proves nothing about impaired convergence. logs[0] is
    the server, logs[1:] the clients in order.
    """
    problems = []
    for index, log in enumerate(logs):
        expected = impairment.expected(index)
        probes = command_outputs(log.entries, "net_impair")
        if len(probes) < IMPAIR_MIN_PROBES:
            problems.append(f"{log.role}: {len(probes)} net_impair probes, expected at least {IMPAIR_MIN_PROBES}")
        for seconds, lines in probes:
            seen = parse_impair_status(lines)
            if seen is None or not impair_matches(seen, expected):
                problems.append(f"{log.role}: net_impair at t={seconds:.1f}s reported {seen or 'disabled'}, "
                                f"expected {expected}")
        observed = views.server if index == 0 else views.clients[index - 1]
        for checkpoint, view in enumerate(observed):
            if view is None:
                continue  # already a convergence failure
            net = view.net
            module = None
            if net is not None and net.impaired:
                module = (net.lag_ms, net.jitter_ms, net.loss_pct, net.dup_pct, net.reorder_pct, net.seed)
            if module is None or not impair_matches(module, expected):
                problems.append(f"{log.role}: checkpoint {checkpoint} module-image impairment is "
                                f"{module or 'disabled'}, expected {expected}")
    return problems


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
                            int(values["kills"]), int(values["xp"]), values["unlocks"])
        current.players[progress.player] = progress
    elif kind == "net":
        current.net = NetView(int(values["bytesSent"]), int(values["bytesReceived"]), int(values["packetsSent"]),
                              int(values["packetsReceived"]), int(values["packetsDropped"]), values["impair"] == "1",
                              float(values["lagMs"]), float(values["jitterMs"]), float(values["lossPct"]),
                              float(values["dupPct"]), float(values["reorderPct"]), int(values["seed"]))


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


def compare_vehicles(server: Observation, client: Observation, label: str,
                     tolerance: float = POSITION_TOLERANCE_M) -> list[str]:
    problems = []
    if set(client.vehicles) != set(server.vehicles):
        return [f"{label}: vehicles {sorted(client.vehicles)} != server {sorted(server.vehicles)}"]
    for net, truth in server.vehicles.items():
        seen = client.vehicles[net]
        if (seen.kind, seen.driver, seen.hp) != (truth.kind, truth.driver, truth.hp):
            problems.append(f"{label}: vehicle {net} kind/driver/hp {(seen.kind, seen.driver, seen.hp)} != server "
                            f"{(truth.kind, truth.driver, truth.hp)}")
        gap = math.dist(seen.pos, truth.pos)
        if gap > tolerance:
            problems.append(f"{label}: vehicle {net} position {seen.pos} is {gap:.2f} m from server {truth.pos}")
    return problems


def compare_views(server: Observation, client: Observation, label: str,
                  tolerance: float = POSITION_TOLERANCE_M) -> list[str]:
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
        if gap > tolerance:
            problems.append(f"{label}: player {player} position {seen.pos} is {gap:.2f} m from server {truth.pos}")
    problems += compare_regions(server, client, label)
    problems += compare_vehicles(server, client, label, tolerance)
    # No own-progression compare: a pure client's self line always reads flux=0
    # rank=1 loadout=default, because TFProgressionSystem fills its records only on
    # the authority and TFClientNet keeps no replicated wallet. Rank, wallet and
    # saved loadout are checked by the scenario verdicts from the server's player
    # lines instead.
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
                verdict["problems"].extend(compare_views(server_view, view, client.role,
                                                         scenario.position_tolerance_m))
        verdict["client_players"] = selves
    verdict["converged"] = not verdict["problems"]
    return verdict, server_view, views


def evaluate(scenario: Scenario, server: RoleLog, clients: list[RoleLog],
             impairment: Impairment | None = None) -> dict:
    """Pair every scheduled client checkpoint with a server observation, compare them, then run the verdict.

    With @p impairment the run must also prove the impairment was live in every
    process (impairment_problems).
    """
    return evaluate_views(scenario, server, clients, impairment)[0]


def evaluate_views(scenario: Scenario, server: RoleLog, clients: list[RoleLog],
                   impairment: Impairment | None = None) -> tuple[dict, RunViews]:
    """evaluate() plus the per-checkpoint views it compared (the restart comparison reuses them)."""
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
    if impairment is not None:
        problems += impairment_problems(impairment, [server, *clients], views)
    summary = {"scenario": scenario.name, "passed": not problems, "problems": problems, "checkpoints": checkpoints}
    if impairment is not None:
        summary["impairment"] = impairment.expected(0)
    return summary, views


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


# --------------------------------------------------------------------------- TF-120 cold restart

# Phase 1: client1 (MRA) saves a legal primary, has its wallet topped up and
# kills client2 (AUC) in the arena; the server flips two outposts. Phase 2 is a
# fresh server process on the same save root that client1 logs back into.
RESTART_PRIMARY = FORGED_LEGAL_PRIMARY
RESTART_FLUX_FLOOR = 300
RESTART_CAPTURES = ((3, FACTION_IDS["auc"]), (7, FACTION_IDS["mra"]))  # an MRA and an HLX outpost at boot
RESTART_CLIENT_TAIL_S = 5.0  # progression debounce (TFProgressionSystem kSaveDebounceSec = 2 s) plus margin
SAVE_CONFIRM_TIMEOUT_S = 15.0
SAVE_OK = "[TF] save: territory ok, progression ok"
CHAR_LIST_HEADER = re.compile(r"\[TF\] characters \((\d+)\):")
CHAR_LIST_ROW = re.compile(r"^\s*\[\d+\] .*\bid (\d+)\s*$")

_FACTION_TAGS = {value: key for key, value in FACTION_IDS.items()}
RESTART_PHASE1 = Scenario(
    name="cold_restart",
    client_factions=("mra", "auc"),
    client_steps=ONBOARDING,
    solo_steps=(
        (0, 14.0, f"tf_give {RESTART_PRIMARY}"),
        (0, 31.5, "tf_aim_at enemy"),
        *_volley(0, 32.0, 0.2, 25),
        (0, 45.0, "tf_char_list"),
    ),
    server_steps=(
        ServerStep(13.0, 24.0, "tf_place_faction mra {:.0f} {:.0f}".format(*ARENA_MRA)),
        ServerStep(13.0, 24.0, "tf_place_faction auc {:.0f} {:.0f}".format(*ARENA_AUC)),
        ServerStep(13.0, 24.0, f"tf_flux_floor mra {RESTART_FLUX_FLOOR}"),
        *(ServerStep(38.0, 49.0, f"tf_capture {region} {_FACTION_TAGS[owner]}") for region, owner in RESTART_CAPTURES),
        ServerStep(50.0, 61.0, "tf_save", changes_world=False),
    ),
    checkpoints=(28.0, 53.0),
    absent=((1, 1),),
    client_seconds=62.0,
)
# The same run without any explicit tf_save: only the debounced and immediate
# commits (docs/specs/persistence.md) stand between the state and the kill.
UNGRACEFUL_PHASE1 = replace(RESTART_PHASE1, name="ungraceful_restart",
                            server_steps=tuple(s for s in RESTART_PHASE1.server_steps if s.command != "tf_save"))
RESTART_RETURN = Scenario(
    name="restart_return",
    client_factions=("mra",),
    client_steps=(
        (1.0, "tf_connect 127.0.0.1:{port}"),
        (3.0, "tf_login {user} {password}"),
        (4.5, "tf_char_list"),
        (6.0, "tf_char_list"),
        (7.5, "tf_enter 0"),
        (9.0, "tf_faction {faction}"),
        (10.0, "tf_spawn"),
    ),
    checkpoints=(15.0,),
    client_seconds=18.0,
)
RESTART_SCENARIOS = {"cold_restart": RESTART_PHASE1, "ungraceful_restart": UNGRACEFUL_PHASE1}


def character_ids(entries: list[AuditEntry]) -> list[int] | None:
    """Character ids of the last tf_char_list that printed a list, or None when none did."""
    listed = None
    for _, lines in command_outputs(entries, "tf_char_list"):
        header = next((CHAR_LIST_HEADER.search(line) for line in lines if CHAR_LIST_HEADER.search(line)), None)
        if header is None:
            continue
        ids = [int(row.group(1)) for row in map(CHAR_LIST_ROW.match, lines) if row]
        # A list whose rows do not add up to its own count is a truncated read, not an answer.
        listed = ids if len(ids) == int(header.group(1)) else None
    return listed


def restart_baseline_problems(before: Progress | None, regions: dict[int, int]) -> list[str]:
    """Phase 1 must end in non-default state, or restoring it proves nothing."""
    if before is None:
        return ["restart: phase 1 server progression for client1 is missing"]
    problems = []
    if before.loadout != RESTART_PRIMARY:
        problems.append(f"restart: phase 1 loadout {before.loadout}, expected {RESTART_PRIMARY}")
    if before.flux < RESTART_FLUX_FLOOR:
        problems.append(f"restart: phase 1 flux {before.flux} is below the {RESTART_FLUX_FLOOR} floor")
    if before.kills < 1 or before.xp <= 0:
        problems.append(f"restart: phase 1 earned no kill/xp (kills {before.kills}, xp {before.xp})")
    for region, owner in RESTART_CAPTURES:
        if regions.get(region) != owner:
            problems.append(f"restart: phase 1 region {region} owner {regions.get(region)}, expected {owner}")
    return problems


def restart_verdict(before_view: Observation, before_id: int, after_view: Observation, after_id: int,
                    characters_before: list[int] | None, characters_after: list[int] | None) -> list[str]:
    """Phase 2 must restore phase 1's authoritative state: territory, character, progression, one character row."""
    before = before_view.players.get(before_id)
    problems = restart_baseline_problems(before, before_view.regions)
    after = after_view.players.get(after_id)
    if after is None or after_id not in after_view.pawns:
        return problems + [f"restart: the returning player {after_id} has no pawn/progression after the restart"]
    if before is not None:
        for name in ("loadout", "rank", "xp", "kills", "unlocks"):
            if getattr(after, name) != getattr(before, name):
                problems.append(f"restart: {name} {getattr(after, name)} was restored, expected "
                                f"{getattr(before, name)}")
        # Flux keeps ticking in: at most two continent income ticks separate the views.
        if not before.flux <= after.flux <= before.flux + 2 * FLUX_INCOME_SLACK:
            problems.append(f"restart: flux {after.flux} was restored, expected {before.flux}")
    if before_id in before_view.pawns and after_view.pawns[after_id].faction != before_view.pawns[before_id].faction:
        problems.append(f"restart: faction {after_view.pawns[after_id].faction} was restored, expected "
                        f"{before_view.pawns[before_id].faction}")
    if after_view.regions != before_view.regions:
        differing = sorted(r for r in set(before_view.regions) | set(after_view.regions)
                           if before_view.regions.get(r) != after_view.regions.get(r))
        problems.append(f"restart: region owners differ after the restart for regions {differing}")
    if characters_before is None or len(characters_before) != 1:
        problems.append(f"restart: phase 1 character list {characters_before}, expected exactly one character")
    elif characters_after != characters_before:
        problems.append(f"restart: character list {characters_after} after the restart, expected "
                        f"{characters_before} (a missing or duplicated character row)")
    return problems


# --------------------------------------------------------------------------- TF-120 continent identity

# continent_mismatch: a cindral_wastes server and one client booted on veyra_highlands (TF_CONTINENT). The server
# names its continent (TF_ContinentIdentity) just before TF_WorldWelcome; the client must refuse and disconnect, so
# it never enters the world and no pawn of it ever exists, although it asks to spawn.
MISMATCH_SERVER_CONTINENT = "cindral_wastes"
MISMATCH_CLIENT_CONTINENT = "veyra_highlands"
MISMATCH_SERVER_TAIL_S = 5.0  # the server keeps observing after the client has gone
CONTINENT_REFUSAL = re.compile(r"\[TF\] server hosts continent '([a-z0-9_-]+)' but this client loaded '([a-z0-9_-]+)'")
OBSERVED_CONTINENT = re.compile(r"\[TF-OBSERVE\] role=(\w+) .*\bcontinent=(\S+)")
SPAWN_ACCEPTED = "[TF] spawn accepted"
CONTINENT_MISMATCH = Scenario(
    name="continent_mismatch",
    client_factions=("mra",),
    client_steps=ONBOARDING,
    checkpoints=(14.0,),
    client_seconds=16.0,
)
CONTINENT_SCENARIOS = {"continent_mismatch": CONTINENT_MISMATCH}


def observed_continents(text: str, role: str) -> set[str]:
    """Every continent key @p role's tf_observe headers named in @p text."""
    return {match.group(2) for match in map(OBSERVED_CONTINENT.search, text.splitlines())
            if match and match.group(1) == role}


def continent_mismatch_verdict(server: RoleLog, client: RoleLog, client_text: str) -> list[str]:
    """The client booted on another continent than the server's must refuse the world and never spawn.

    @p client_text is everything the client printed (audit, stdout, stderr): the refusal is logged from a
    network handler, not by a scripted command.
    """
    problems = []
    for log in (server, client):
        if log.returncode != 0:
            problems.append(f"{log.role}: exited with {log.returncode}")
        if log.anchor is None:
            problems.append(f"{log.role}: no audit trail was written")
    server_views = [sample.observation for sample in observe_samples(server.entries) if sample.observation]
    if not server_views:
        problems.append("continent_mismatch: the server printed no complete observation")
    for view in server_views:
        if view.continent != MISMATCH_SERVER_CONTINENT:
            problems.append(f"continent_mismatch: the server hosts {view.continent}, expected "
                            f"{MISMATCH_SERVER_CONTINENT}")
            break
    spawned = sorted({player for view in server_views for player in view.pawns})
    if spawned:
        problems.append(f"continent_mismatch: the server holds pawns {spawned} of a client it should never admit")
    # Without this the run could pass on two processes that loaded the same continent.
    loaded = observed_continents(client_text, "client")
    if loaded != {MISMATCH_CLIENT_CONTINENT}:
        problems.append(f"continent_mismatch: the client observed continents {sorted(loaded)}, expected only "
                        f"{MISMATCH_CLIENT_CONTINENT}")
    if (MISMATCH_SERVER_CONTINENT, MISMATCH_CLIENT_CONTINENT) not in CONTINENT_REFUSAL.findall(client_text):
        problems.append(f"continent_mismatch: the client never refused the {MISMATCH_SERVER_CONTINENT} server "
                        f"while it had loaded {MISMATCH_CLIENT_CONTINENT}")
    if SPAWN_ACCEPTED in client_text:
        problems.append("continent_mismatch: the client spawned a pawn on the wrong continent")
    return problems


# --------------------------------------------------------------------------- TF-110 soak

SOAK_BUDGETS_PATH = Path(__file__).resolve().with_name("soak_budgets.json")
SOAK_BUDGETS_SCHEMA = "terrafront-soak-budgets/1"
SOAK_BUDGET_KEYS = ("bots", "sampleIntervalS", "warmupS", "tickHz", "maxTickAvgP95Ms", "maxTickPeakMs",
                    "replicationHz", "minClientPacketsPerSecond", "maxClientDownlinkKBps",
                    "maxDroppedPacketFraction", "maxRssSlopeMiBPerHour", "rssSampleIntervalS")
SOAK_SERVER_TAIL_S = 10.0
SOAK_LOOP_S = 6.0  # one walk out, one walk back, a shot after each
PERF_TOTAL = re.compile(r"measured total: ([\d.]+) ms")
PERF_PEAK = re.compile(r"\bpeak ([\d.]+) ms")
BOTS_ACTIVE = re.compile(r"\[TF\] bots active: (\d+)")

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "ops"))
from server_soak import MIN_FIT_SAMPLES, fit_rss_slope, read_process_rss  # noqa: E402  (OPS-110 slope fit)


def load_soak_budgets(path: Path) -> dict:
    """The provisional soak guards; a missing, non-positive or unknown-schema file fails closed."""
    try:
        budgets = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise HarnessError(f"{path}: {error}") from error
    if not isinstance(budgets, dict):
        raise HarnessError(f"{path}: not a JSON object")
    if budgets.get("schema") != SOAK_BUDGETS_SCHEMA or budgets.get("provisional") is not True:
        raise HarnessError(f"{path}: schema must be {SOAK_BUDGETS_SCHEMA} and provisional must be true")
    for key in SOAK_BUDGET_KEYS:
        value = budgets.get(key)
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not value > 0:
            raise HarnessError(f"{path}: budget {key!r} must be a positive number, got {value!r}")
    return budgets


def percentile(values: list[float], fraction: float) -> float:
    """Nearest-rank percentile of a non-empty sample."""
    ordered = sorted(values)
    return ordered[max(0, math.ceil(fraction * len(ordered)) - 1)]


def perf_samples(entries: list[AuditEntry]) -> list[tuple[float, float, float]]:
    """(seconds, measured tick total ms, sum of phase peaks ms) per tf_perf report that had samples.

    tf_perf keeps ~2 s rings per phase: the total is the mean tick over the last
    ring, the peaks cover everything since the previous "tf_perf reset", and
    their sum bounds the worst single tick of that window from above.
    """
    samples = []
    for seconds, lines in command_outputs(entries, "tf_perf"):
        text = "\n".join(lines)
        total = PERF_TOTAL.search(text)
        if total is not None:
            samples.append((seconds, float(total.group(1)), sum(float(p) for p in PERF_PEAK.findall(text))))
    return samples


def expected_soak_samples(budgets: dict, soak_seconds: float) -> int:
    return max(1, math.floor((soak_seconds - budgets["warmupS"]) / budgets["sampleIntervalS"]) - 1)


def client_net_problems(label: str, samples: list[Sample], budgets: dict, needed: int) -> tuple[list[str], dict]:
    """Per-client downlink, replication rate and drop fraction between consecutive post-warmup samples."""
    usable = [s for s in samples if s.seconds >= budgets["warmupS"] and s.observation is not None]
    problems = []
    if len(usable) < needed:
        problems.append(f"{label}: {len(usable)} post-warmup observations, expected at least {needed}")
    for sample in usable:
        view = sample.observation
        if view.self_id not in view.pawns or view.net is None:
            problems.append(f"{label}: at t={sample.seconds:.1f}s its own pawn or its net line is missing")
    downlinks, rates, drops = [], [], []
    for first, second in zip(usable, usable[1:]):
        a, b = first.observation.net, second.observation.net
        span = second.seconds - first.seconds
        if a is None or b is None or span <= 0:
            continue
        received = b.packets_received - a.packets_received
        downlinks.append((b.bytes_received - a.bytes_received) / 1024.0 / span)
        rates.append(received / span)
        drops.append((b.packets_dropped - a.packets_dropped) / max(received, 1))
    if len(downlinks) < needed - 1:
        problems.append(f"{label}: {len(downlinks)} traffic intervals, expected at least {needed - 1}")
    if downlinks and max(downlinks) > budgets["maxClientDownlinkKBps"]:
        problems.append(f"{label}: downlink {max(downlinks):.1f} KB/s exceeds {budgets['maxClientDownlinkKBps']}")
    if rates and min(rates) < budgets["minClientPacketsPerSecond"]:
        problems.append(f"{label}: {min(rates):.1f} packets/s received, below the "
                        f"{budgets['minClientPacketsPerSecond']} a {budgets['replicationHz']} Hz replication needs")
    if drops and max(drops) > budgets["maxDroppedPacketFraction"]:
        problems.append(f"{label}: dropped fraction {max(drops):.3f} exceeds {budgets['maxDroppedPacketFraction']}")
    metrics = {"maxDownlinkKBps": max(downlinks, default=None), "minPacketsPerSecond": min(rates, default=None),
               "maxDroppedFraction": max(drops, default=None)}
    return problems, metrics


def rss_problems(label: str, rss: list[tuple[float, int]], budgets: dict,
                 soak_seconds: float) -> tuple[list[str], dict]:
    """Least-squares RSS slope after warmup (OPS-110 fit_rss_slope) against the provisional cap."""
    slope, growth, count = fit_rss_slope([(t, value, 0) for t, value in rss], budgets["warmupS"], soak_seconds)
    metrics = {"rssSlopeMiBPerHour": None if slope is None else slope / (1024.0 * 1024.0), "rssSamples": count,
               "rssGrowthBytes": growth}
    if slope is None:
        return [f"{label}: {count} RSS samples after warmup, at least {MIN_FIT_SAMPLES} needed"], metrics
    if metrics["rssSlopeMiBPerHour"] > budgets["maxRssSlopeMiBPerHour"]:
        return [f"{label}: RSS grows {metrics['rssSlopeMiBPerHour']:.1f} MiB/h, above the provisional "
                f"{budgets['maxRssSlopeMiBPerHour']} MiB/h"], metrics
    return [], metrics


def evaluate_soak(budgets: dict, soak_seconds: float, server: RoleLog, clients: list[RoleLog],
                  rss: dict[str, list[tuple[float, int]]]) -> dict:
    """Every budget in soak_budgets.json, over the post-warmup window; any crash or short sample set fails."""
    problems: list[str] = []
    metrics: dict = {}
    for log in [server, *clients]:
        if log.returncode != 0:
            problems.append(f"{log.role}: exited with {log.returncode}")
        problems += [f"{log.role}: scripted command reported ERR: {e.command}" for e in log.entries if not e.ok]
        if log.anchor is None:
            problems.append(f"{log.role}: no audit trail was written")
    needed = expected_soak_samples(budgets, soak_seconds)

    perf = [p for p in perf_samples(server.entries) if p[0] >= budgets["warmupS"]]
    if len(perf) < needed:
        problems.append(f"server: {len(perf)} post-warmup tf_perf samples, expected at least {needed}")
    else:
        p95 = percentile([total for _, total, _ in perf], 0.95)
        worst = max(peak for _, _, peak in perf)
        metrics["server"] = {"tickAvgP95Ms": p95, "tickPeakBoundMs": worst, "perfSamples": len(perf)}
        if p95 > budgets["maxTickAvgP95Ms"]:
            problems.append(f"server: tick p95 {p95:.2f} ms exceeds {budgets['maxTickAvgP95Ms']} ms "
                            f"({budgets['tickHz']} Hz)")
        if worst > budgets["maxTickPeakMs"]:
            problems.append(f"server: a tick window peaked at {worst:.2f} ms, above {budgets['maxTickPeakMs']} ms")
    bots = []
    for seconds, lines in command_outputs(server.entries, "tf_bots"):
        if seconds >= budgets["warmupS"]:
            active = BOTS_ACTIVE.search("\n".join(lines))
            bots.append((seconds, int(active.group(1)) if active else None))
    if len(bots) < needed or any(count != budgets["bots"] for _, count in bots):
        problems.append(f"server: bot counts {[count for _, count in bots]}, expected {budgets['bots']} at "
                        f"least {needed} times")

    for client in clients:
        client_problems, metrics[client.role] = client_net_problems(client.role, observe_samples(client.entries),
                                                                    budgets, needed)
        problems += client_problems
    for log in [server, *clients]:
        memory_problems, memory = rss_problems(log.role, rss.get(log.role, []), budgets, soak_seconds)
        problems += memory_problems
        metrics.setdefault(log.role, {}).update(memory)
    return {"scenario": "soak", "passed": not problems, "problems": problems, "soakSeconds": soak_seconds,
            "provisionalBudgets": {k: budgets[k] for k in SOAK_BUDGET_KEYS}, "metrics": metrics}


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


def render_script(timeline: list[tuple[float, str]], impairment: Impairment | None, role_index: int,
                  run_seconds: float) -> str:
    """An -exec script: frame-0 setup (tf_status, then any impairment) and the timeline sorted by time.

    An impaired process probes net_impair again just before it exits, so the
    run proves the impairment stayed on, not only that it was switched on.
    """
    setup = ["tf_status"]
    if impairment is not None:
        setup += impairment.commands(role_index)
        timeline = [*timeline, (run_seconds - 0.5, "net_impair")]
    timeline = sorted(timeline, key=lambda item: item[0])
    return "".join(f"0 {command}\n" for command in setup) + \
        "".join(f"t{format_seconds(when)} {command}\n" for when, command in timeline)


def server_script(port: int, run_seconds: float, scenario: Scenario | None = None,
                  impairment: Impairment | None = None) -> str:
    timeline = [(0.5, f"tf_dedicated {port}")]
    at = 1.0
    while at < run_seconds - 0.5:
        timeline.append((at, "tf_observe"))
        at += SERVER_OBSERVE_INTERVAL_S
    for step in scenario.server_steps if scenario else ():
        timeline += [(when, step.command) for when in server_step_times(step)]
    return render_script(timeline, impairment, 0, run_seconds)


def fresh_credentials() -> dict[str, str]:
    return {"user": "mc" + secrets.token_hex(6), "password": secrets.token_urlsafe(18),
            "name": "MC" + secrets.token_hex(5)}


def client_script(scenario: Scenario, port: int, faction: str, index: int = 0,
                  credentials: dict[str, str] | None = None, impairment: Impairment | None = None) -> str:
    values = {"port": port, "faction": faction, **(credentials or fresh_credentials())}
    timeline = [(at, template.format(**values)) for at, template in timed_client_steps(scenario, index)]
    timeline += [(at, "tf_observe") for at in scenario.checkpoints]
    return render_script(timeline, impairment, index + 1, scenario.client_seconds)


def soak_server_script(port: int, budgets: dict, soak_seconds: float) -> str:
    interval = budgets["sampleIntervalS"]
    timeline = [(0.5, f"tf_dedicated {port}"), (2.0, f"tf_bots {budgets['bots']}")]
    at = interval
    while at < soak_seconds:
        timeline += [(at, "tf_observe"), (at + 0.2, "tf_perf"), (at + 0.4, "tf_perf reset"), (at + 0.6, "tf_bots")]
        at += interval
    return render_script(timeline, None, 0, soak_seconds)


def soak_client_script(port: int, faction: str, budgets: dict, soak_seconds: float) -> str:
    values = {"port": port, "faction": faction, **fresh_credentials()}
    timeline = [(at, template.format(**values)) for at, template in ONBOARDING]
    at = ONBOARDING[-1][0] + 2.5
    while at + SOAK_LOOP_S < soak_seconds:
        timeline += [(at, "tf_walk 1 0 2"), (at + 2.5, "tf_fire"), (at + 3.0, "tf_walk -1 0 2"), (at + 5.5, "tf_fire")]
        at += SOAK_LOOP_S
    at = budgets["sampleIntervalS"]
    while at < soak_seconds:
        timeline.append((at + 0.1, "tf_observe"))
        at += budgets["sampleIntervalS"]
    return render_script(timeline, None, 0, soak_seconds)


@dataclass
class Child:
    role: str
    process: subprocess.Popen
    audit: Path
    anchor: float | None = None
    started: float = field(default_factory=time.monotonic)
    killed: bool = False  # terminated by the harness on purpose (restart runs)

    def poll_anchor(self) -> None:
        if self.anchor is None and self.audit.exists():
            self.anchor = time.monotonic()

    def log(self) -> RoleLog:
        text = self.audit.read_text(encoding="utf-8", errors="replace") if self.audit.exists() else ""
        # A deliberate kill is the scenario, not a crash; every other exit status counts.
        returncode = 0 if self.killed else self.process.returncode
        return RoleLog(self.role, returncode, self.anchor if text else None, parse_audit(text))


def launch(role: str, args: argparse.Namespace, workdir: Path, script: str, seconds: float,
           save_root: Path | None = None, continent: str | None = None) -> Child:
    role_dir = workdir / role
    # The audit is append-only: a previous run's trail would replay stale views.
    shutil.rmtree(role_dir, ignore_errors=True)
    (role_dir / "saves").mkdir(parents=True)
    cfg = role_dir / f"{role}.cfg"
    cfg.write_text(script, encoding="utf-8", newline="\n")
    audit = role_dir / "exec_audit.log"
    # NET-100: the server's identity file and each client's trust-on-first-use known_hosts live
    # under the per-user data directory; give every role its own so a run never touches the real one.
    user_data = str(role_dir / "userdata")
    env = dict(os.environ, TF_SAVE_ROOT=str(save_root or role_dir / "saves"), SPARK_RHI_BACKEND="null",
               LOCALAPPDATA=user_data, XDG_DATA_HOME=user_data)
    # The continent is chosen at boot (TFDataTables: tf_continent / TF_CONTINENT). Every role loads the default
    # unless the run names one, whatever the caller's own environment says.
    env.pop("TF_CONTINENT", None)
    if continent is not None:
        env["TF_CONTINENT"] = continent
    command =[str(args.engine), "-headless", "-no-subprocess", "-require-game", "-threads", "2", "-game",
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
            raise HarnessError(f"{server.role} exited with {server.process.returncode} before it started listening")
        if server.audit.exists():
            for entry in parse_audit(server.audit.read_text(encoding="utf-8", errors="replace")):
                if entry.command.startswith("tf_dedicated"):
                    if any("dedicated server started" in line for line in entry.output):
                        return
                    raise HarnessError(f"{server.role}: tf_dedicated did not start a dedicated server")
        time.sleep(POLL_INTERVAL_S)
    raise HarnessError(f"{server.role} did not start listening within {SERVER_READY_TIMEOUT_S} s")


def wait_for_exit(children: list[Child], deadline: float, on_poll: Callable[[], None] | None = None) -> None:
    while any(child.process.poll() is None for child in children):
        for child in children:
            child.poll_anchor()
        if on_poll is not None:
            on_poll()
        if time.monotonic() > deadline:
            raise HarnessError("timed out waiting for the processes to exit")
        time.sleep(POLL_INTERVAL_S)
    for child in children:
        child.poll_anchor()


def stop_all(children: list[Child]) -> None:
    for child in children:
        if child.process.poll() is None:
            child.process.kill()
            child.process.wait()


def run(args: argparse.Namespace, name: str, workdir: Path) -> dict:
    """One scenario: a dedicated server plus its clients, optionally under args.impairment."""
    scenario = SCENARIOS[name]
    if args.impairment is not None:
        scenario = impaired(scenario, args.impairment)
    violations = schedule_violations(scenario)
    if violations:
        raise HarnessError("; ".join(violations))
    workdir.mkdir(parents=True, exist_ok=True)
    port = free_udp_port()
    deadline = time.monotonic() + args.timeout
    server_seconds = scenario.client_seconds + SERVER_TAIL_S
    children: list[Child] = []
    try:
        server = launch("server", args, workdir, server_script(port, server_seconds, scenario, args.impairment),
                        server_seconds)
        children.append(server)
        wait_for_server(server, min(deadline, time.monotonic() + SERVER_READY_TIMEOUT_S))
        for index, faction in enumerate(scenario.client_factions):
            script = client_script(scenario, port, faction, index, impairment=args.impairment)
            children.append(launch(f"client{index + 1}", args, workdir, script, scenario.client_seconds))
        wait_for_exit(children, deadline)
    finally:
        stop_all(children)
        for index in range(len(scenario.client_factions)):
            (workdir / f"client{index + 1}" / f"client{index + 1}.cfg").unlink(missing_ok=True)

    logs = [child.log() for child in children]
    for log, faction in zip(logs[1:], scenario.client_factions):
        log.faction = faction
    summary = evaluate(scenario, logs[0], logs[1:], args.impairment)
    summary["port"] = port
    summary["workdir"] = str(workdir)
    return summary


def kill_after_commit(server: Child, graceful: bool) -> list[str]:
    """Hard-kill the phase 1 server once its state is committed: after an ok tf_save, or after the debounce."""
    problems = []
    if graceful:
        deadline = time.monotonic() + SAVE_CONFIRM_TIMEOUT_S
        while not any(SAVE_OK in "\n".join(lines) for _, lines in command_outputs(server.log().entries, "tf_save")):
            if time.monotonic() > deadline:
                problems.append(f"{server.role}: no tf_save reported '{SAVE_OK}' before the kill")
                break
            time.sleep(POLL_INTERVAL_S)
    else:
        time.sleep(RESTART_CLIENT_TAIL_S)
        if command_outputs(server.log().entries, "tf_save"):
            problems.append(f"{server.role}: ran tf_save, so the kill did not test the debounced commit")
    if server.process.poll() is not None:
        problems.append(f"{server.role}: exited with {server.process.returncode} before the harness killed it")
    else:
        server.process.kill()  # TerminateProcess / SIGKILL: no shutdown path, no final flush
        server.process.wait()
        server.killed = True
    return problems


def run_restart(args: argparse.Namespace, name: str, workdir: Path) -> dict:
    """TF-120: phase 1 builds state and is hard-killed; phase 2 restarts on the same save root and compares."""
    phase1 = RESTART_SCENARIOS[name]
    violations = schedule_violations(phase1) + schedule_violations(RESTART_RETURN)
    if violations:
        raise HarnessError("; ".join(violations))
    workdir.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + args.timeout
    credentials = [fresh_credentials() for _ in phase1.client_factions]
    children: list[Child] = []
    problems: list[str] = []
    try:
        port = free_udp_port()
        server = launch("server", args, workdir, server_script(port, phase1.client_seconds + SERVER_TAIL_S, phase1),
                        phase1.client_seconds + SERVER_TAIL_S)
        children.append(server)
        wait_for_server(server, min(deadline, time.monotonic() + SERVER_READY_TIMEOUT_S))
        clients = [launch(f"client{i + 1}", args, workdir, client_script(phase1, port, faction, i, credentials[i]),
                          phase1.client_seconds) for i, faction in enumerate(phase1.client_factions)]
        children += clients
        wait_for_exit(clients, deadline, server.poll_anchor)
        problems += kill_after_commit(server, graceful=any(s.command == "tf_save" for s in phase1.server_steps))

        port = free_udp_port()
        seconds = RESTART_RETURN.client_seconds + SERVER_TAIL_S
        restarted = launch("server-restart", args, workdir, server_script(port, seconds, RESTART_RETURN), seconds,
                           save_root=workdir / "server" / "saves")
        children.append(restarted)
        wait_for_server(restarted, min(deadline, time.monotonic() + SERVER_READY_TIMEOUT_S))
        returning = launch("client1-restart", args, workdir,
                           client_script(RESTART_RETURN, port, phase1.client_factions[0], 0, credentials[0]),
                           RESTART_RETURN.client_seconds)
        children.append(returning)
        wait_for_exit([restarted, returning], deadline)
    finally:
        stop_all(children)
        for child in children:
            (workdir / child.role / f"{child.role}.cfg").unlink(missing_ok=True)

    logs = {child.role: child.log() for child in children}
    for role, faction in (("client1", "mra"), ("client2", "auc"), ("client1-restart", "mra")):
        logs[role].faction = faction
    first, first_views = evaluate_views(phase1, logs["server"], [logs["client1"], logs["client2"]])
    second, second_views = evaluate_views(RESTART_RETURN, logs["server-restart"], [logs["client1-restart"]])
    problems += [f"phase 1: {p}" for p in first["problems"]] + [f"phase 2: {p}" for p in second["problems"]]
    before_view, after_view = first_views.server[-1], second_views.server[-1]
    before_id, after_id = first_views.self_id(0, -1), second_views.self_id(0, -1)
    if before_view is None or after_view is None or before_id is None or after_id is None:
        problems.append("restart: a phase 1 or phase 2 final checkpoint view is missing")
    else:
        problems += restart_verdict(before_view, before_id, after_view, after_id,
                                    character_ids(logs["client1"].entries),
                                    character_ids(logs["client1-restart"].entries))
    return {"scenario": name, "passed": not problems, "problems": problems,
            "checkpoints": first["checkpoints"] + second["checkpoints"], "workdir": str(workdir)}


def role_text(workdir: Path, role: str) -> str:
    """Everything @p role printed: its audit trail plus its stdout and stderr logs."""
    texts = []
    for name in ("exec_audit.log", "stdout.log", "stderr.log"):
        path = workdir / role / name
        texts.append(path.read_text(encoding="utf-8", errors="replace") if path.exists() else "")
    return "\n".join(texts)


def run_continent_mismatch(args: argparse.Namespace, name: str, workdir: Path) -> dict:
    """TF-120: a client booted on another continent than the server's must refuse to enter its world."""
    scenario = CONTINENT_SCENARIOS[name]
    workdir.mkdir(parents=True, exist_ok=True)
    port = free_udp_port()
    deadline = time.monotonic() + args.timeout
    server_seconds = scenario.client_seconds + MISMATCH_SERVER_TAIL_S
    children: list[Child] = []
    try:
        server = launch("server", args, workdir, server_script(port, server_seconds), server_seconds,
                        continent=MISMATCH_SERVER_CONTINENT)
        children.append(server)
        wait_for_server(server, min(deadline, time.monotonic() + SERVER_READY_TIMEOUT_S))
        client = launch("client1", args, workdir, client_script(scenario, port, scenario.client_factions[0]),
                        scenario.client_seconds, continent=MISMATCH_CLIENT_CONTINENT)
        children.append(client)
        wait_for_exit(children, deadline)
    finally:
        stop_all(children)
        for child in children:
            (workdir / child.role / f"{child.role}.cfg").unlink(missing_ok=True)
    problems = continent_mismatch_verdict(server.log(), client.log(), role_text(workdir, client.role))
    return {"scenario": name, "passed": not problems, "problems": problems, "checkpoints": [],
            "port": port, "workdir": str(workdir)}


def read_rss(pid: int) -> int | None:
    """Resident set of a live child in bytes (Linux /proc, Windows working set), or None."""
    if sys.platform != "win32":
        return read_process_rss(pid)
    import ctypes
    from ctypes import wintypes

    class ProcessMemoryCounters(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                    *((name, ctypes.c_size_t) for name in (
                        "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage", "QuotaPagedPoolUsage",
                        "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage", "PagefileUsage",
                        "PeakPagefileUsage"))]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    kernel32.K32GetProcessMemoryInfo.argtypes = (wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD)
    kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    process_query_limited_information, process_vm_read = 0x1000, 0x0010
    handle = kernel32.OpenProcess(process_query_limited_information | process_vm_read, False, pid)
    if not handle:
        return None
    try:
        counters = ProcessMemoryCounters()
        counters.cb = ctypes.sizeof(counters)
        if not kernel32.K32GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
            return None
        return int(counters.WorkingSetSize)
    finally:
        kernel32.CloseHandle(handle)


def run_soak(args: argparse.Namespace, workdir: Path) -> dict:
    """TF-110 soak: server + bots + two looping clients; tick, traffic and RSS are sampled against the budgets."""
    budgets = load_soak_budgets(args.budgets)
    soak_seconds = args.soak_seconds
    if soak_seconds <= budgets["warmupS"] + 2 * budgets["sampleIntervalS"]:
        raise HarnessError(f"--soak-seconds {soak_seconds} leaves no post-warmup samples")
    workdir.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + args.timeout
    rss: dict[str, list[tuple[float, int]]] = {}
    children: list[Child] = []
    next_sample = [0.0]

    def sample_rss() -> None:
        now = time.monotonic()
        if now < next_sample[0]:
            return
        next_sample[0] = now + budgets["rssSampleIntervalS"]
        for child in children:
            value = read_rss(child.process.pid) if child.process.poll() is None else None
            if value is not None:
                rss.setdefault(child.role, []).append((now - child.started, value))

    try:
        port = free_udp_port()
        server = launch("server", args, workdir, soak_server_script(port, budgets, soak_seconds),
                        soak_seconds + SOAK_SERVER_TAIL_S)
        children.append(server)
        wait_for_server(server, min(deadline, time.monotonic() + SERVER_READY_TIMEOUT_S))
        for index, faction in enumerate(("mra", "auc")):
            children.append(launch(f"client{index + 1}", args, workdir,
                                   soak_client_script(port, faction, budgets, soak_seconds), soak_seconds))
        wait_for_exit(children, deadline, sample_rss)
    finally:
        stop_all(children)
        for child in children:
            (workdir / child.role / f"{child.role}.cfg").unlink(missing_ok=True)
    logs = [child.log() for child in children]
    summary = evaluate_soak(budgets, soak_seconds, logs[0], logs[1:], rss)
    summary["workdir"] = str(workdir)
    return summary


def run_all(args: argparse.Namespace) -> dict:
    """Every requested run; several scenarios each get their own sub-directory and must all pass."""
    workdir = Path(args.workdir).resolve()
    if args.soak_seconds is not None:
        runs = [lambda: run_soak(args, workdir)]
        names = ["soak"]
    else:
        names = args.scenario
        runs = []
        for name in names:
            target = workdir / name if len(names) > 1 else workdir
            runner = (run_restart if name in RESTART_SCENARIOS else
                      run_continent_mismatch if name in CONTINENT_SCENARIOS else run)
            runs.append(lambda runner=runner, name=name, target=target: runner(args, name, target))
    summaries = []
    for name, runner in zip(names, runs):
        try:
            summaries.append(runner())
        except HarnessError as error:
            summaries.append({"scenario": name, "passed": False, "problems": [str(error)], "checkpoints": []})
    if len(summaries) == 1:
        return summaries[0]
    return {"passed": all(s["passed"] for s in summaries), "runs": summaries,
            "problems": [f"{s['scenario']}: {p}" for s in summaries for p in s["problems"]]}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--engine", required=True, type=Path, help="SparkEngine executable")
    parser.add_argument("--module", required=True, type=Path, help="SparkGameMMOFPS module")
    parser.add_argument("--scenario", nargs="+",
                        choices=sorted([*SCENARIOS, *RESTART_SCENARIOS, *CONTINENT_SCENARIOS]),
                        help="one or more scenarios, run in order")
    parser.add_argument("--soak-seconds", type=float, help="run the TF-110 soak for this long instead")
    parser.add_argument("--budgets", type=Path, default=SOAK_BUDGETS_PATH, help="soak budget file")
    parser.add_argument("--impair", help="'<lagMs>,<jitterMs>,<lossFrac>,<dupPct>,<reorderPct>' for every process")
    parser.add_argument("--seed", type=int, default=1, help="impairment RNG seed (process k uses seed + k)")
    parser.add_argument("--workdir", required=True, help="per-run directory for scripts, audits and saves")
    parser.add_argument("--cwd", type=Path, help="asset root the processes run in (default: the engine's directory)")
    parser.add_argument("--timeout", type=float, default=200.0, help="wall-clock limit for each run")
    args = parser.parse_args(argv)
    if (args.scenario is None) == (args.soak_seconds is None):
        parser.error("give exactly one of --scenario or --soak-seconds")
    args.impairment = None
    if args.impair is not None:
        if args.scenario is None or any(name not in SCENARIOS for name in args.scenario):
            parser.error("--impair applies to the convergence scenarios only")
        try:
            args.impairment = Impairment.parse(args.impair, args.seed)
        except ValueError as error:
            parser.error(str(error))
    args.engine = args.engine.resolve()
    args.module = args.module.resolve()
    args.cwd = (args.cwd or args.engine.parent).resolve()
    for required in (args.engine, args.module):
        if not required.is_file():
            parser.error(f"not a file: {required}")

    try:
        summary = run_all(args)
    except HarnessError as error:
        summary = {"scenario": "soak", "passed": False, "problems": [str(error)], "checkpoints": []}
    label = "soak" if args.soak_seconds is not None else " ".join(args.scenario)
    print(json.dumps(summary, indent=2))
    if not summary["passed"]:
        print(f"TerrafrontMultiClient {label}: FAILED", file=sys.stderr)
        return 1
    print(f"TerrafrontMultiClient {label}: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
