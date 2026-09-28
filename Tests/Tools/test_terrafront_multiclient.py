#!/usr/bin/env python3
"""TF-110: unit tests for the TERRAFRONT multi-client harness (Tools/Terrafront/multiclient.py).

Exercises the audit parser, the convergence comparator and every scenario
verdict against synthetic exec_audit.log text in the exact format
ExecScriptPlayer writes and tf_observe prints (FormatObservation plus the
authority's per-player progression lines), so no engine build is needed. The
real three-process runs are the TerrafrontMultiClient_<Scenario> CTest entries.
"""

from __future__ import annotations

import io
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from dataclasses import replace
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "Tools" / "Terrafront"))

import multiclient  # noqa: E402

SCENARIO = multiclient.SCENARIOS["onboard_spawn_move"]
SERVER_ANCHOR = 100.0
CLIENT_ANCHOR = 104.0
SERVER_SELF = 4294967295
# The player id each client's pawn has on the server, in client order.
CLIENT_PLAYERS = (2, 1)
DEFAULT_PROGRESS = ("default", 0, 1, 0)  # loadout, flux, rank, kills
# What a pure client's self line really carries. TFProgressionSystem fills its
# per-player records only on the authority, so a client's FluxOf / RankOf /
# GetLoadout fall back to 0 / 1 / none whatever the server holds.
PURE_CLIENT_SELF = ("default", 0, 1)
DEFAULT_REGIONS = {0: 1, 1: 2, 3: 1}

# Server-side truth per checkpoint: player -> (faction, class, health, pos).
TRUTH = [
    {1: (2, 1, 450, (344.0, 24.0, 3808.0)), 2: (1, 1, 450, (296.0, 24.0, 3744.0))},
    {1: (2, 1, 450, (344.0, 24.0, 3823.75)), 2: (1, 1, 450, (296.0, 24.0, 3759.75))},
    {1: (2, 1, 450, (358.75, 24.0, 3823.75)), 2: (1, 1, 450, (311.0, 24.0, 3759.75))},
]


def fmt(pos: tuple[float, float, float]) -> str:
    return ",".join(f"{v:.2f}" for v in pos)


def net_line(impair: tuple | None = None, received: tuple[int, int] = (0, 0), dropped: int = 0) -> str:
    """A "[TF-OBSERVE] net" line. impair: (lag, jitter, loss %, dup %, reorder %, seed) or None (off);
    received: (bytesReceived, packetsReceived)."""
    lag, jitter, loss, dup, reorder, seed = impair or (0.0, 0.0, 0.0, 0.0, 0.0, 0)
    return (f"[TF-OBSERVE] net bytesSent=100 bytesReceived={received[0]} packetsSent=10 "
            f"packetsReceived={received[1]} packetsDropped={dropped} impair={1 if impair else 0} lagMs={lag:.1f} "
            f"jitterMs={jitter:.1f} lossPct={loss:.1f} dupPct={dup:.1f} reorderPct={reorder:.1f} seed={seed}")


def observation(role: str, self_id: int, pawns: dict, regions: dict | None = None, vehicles: dict | None = None,
                players: dict | None = None, net_view: str | None = None) -> list[str]:
    """tf_observe output split into lines, as it lands in the audit (first line prefixed).

    vehicles: net -> (kind, driver, hp, pos); players: id -> (loadout, flux, rank, kills[, xp, unlocks]).
    The server prints one player line per pawn. A pure client holds no progression, so its
    self line is PURE_CLIENT_SELF whatever @p players (the server's truth) says. @p net_view is an
    optional net_line().
    """
    regions = DEFAULT_REGIONS if regions is None else regions
    vehicles = vehicles or {}
    players = players or {}
    loadout, flux, rank = PURE_CLIENT_SELF if role == "client" else DEFAULT_PROGRESS[:3]
    lines = [f"    > [TF-OBSERVE] role={role} clock=1.000 self={self_id} continent=cindral_wastes "
             f"pawns={len(pawns)} regions={len(regions)} vehicles={len(vehicles)}",
             f"[TF-OBSERVE] self id={self_id} loadout={loadout} flux={flux} rank={rank}"]
    for player in sorted(pawns):
        faction, cls, health, pos = pawns[player]
        lines.append(f"[TF-OBSERVE] pawn id={player} faction={faction} class={cls} health={health} pos={fmt(pos)}")
    lines += [f"[TF-OBSERVE] region id={r} owner={owner}" for r, owner in sorted(regions.items())]
    for net in sorted(vehicles):
        kind, driver, hp, pos = vehicles[net]
        lines.append(f"[TF-OBSERVE] vehicle net={net} def={kind} driver={driver} hp={hp} pos={fmt(pos)}")
    if role == "server":
        for player in sorted(pawns):
            loadout, flux, rank, kills, *extra = players.get(player, DEFAULT_PROGRESS)
            xp, unlocks = extra or (0, "-")
            lines.append(f"[TF-OBSERVE] player id={player} loadout={loadout} flux={flux} rank={rank} xp={xp} "
                         f"unlocks={unlocks} kills={kills}")
    if net_view is not None:
        lines.append(net_view)
    return lines


def entry(frame: int, seconds: float, command: str, output: list[str], ok: bool = True) -> list[str]:
    header = f"frame {frame} t={seconds:.1f}s | {'ok ' if ok else 'ERR'} | {command}"
    return [header, f"    > [exec] frame {frame} (t={seconds:.1f}s): {command}", *output]


def world_at(wall: float, anchors: tuple[float, ...] = (CLIENT_ANCHOR, CLIENT_ANCHOR),
             scenario: multiclient.Scenario = SCENARIO) -> dict:
    """The world at a wall-clock time. Each pawn jumps to its next TRUTH position the
    instant its own client starts a walk (the worst case for a skewed observer)."""
    walks = [at for at, command in scenario.client_steps if command.startswith("tf_walk")]
    world = {}
    for anchor, player in zip(anchors, CLIENT_PLAYERS):
        world[player] = TRUTH[sum(1 for at in walks if wall >= anchor + at)][player]
    return world


def server_audit(until: float = 70.0, anchors: tuple[float, ...] = (CLIENT_ANCHOR, CLIENT_ANCHOR),
                 scenario: multiclient.Scenario = SCENARIO, net_view: str | None = None) -> str:
    lines = entry(0, 0.0, "tf_status", ["    > [TF] TERRAFRONT role=standalone"])
    lines += entry(15, 0.5, "tf_dedicated 23000", ["    > [TF] dedicated server started on port 23000"])
    at, frame = 1.0, 30
    while at < until:
        world = world_at(SERVER_ANCHOR + at, anchors, scenario)
        lines += entry(frame, at, "tf_observe", observation("server", SERVER_SELF, world, net_view=net_view))
        at += multiclient.SERVER_OBSERVE_INTERVAL_S
        frame += 15
    return "\n".join(lines) + "\n"


def client_audit(self_id: int, views: list, ok: bool = True, scenario: multiclient.Scenario = SCENARIO) -> str:
    """views: per checkpoint, a pawns dict, a full observation line list, or None (printed nothing)."""
    lines = entry(0, 0.0, "tf_status", ["    > [TF] TERRAFRONT role=standalone"])
    lines += entry(30, 1.0, "tf_connect 127.0.0.1:23000", ["    > [TF] connecting"], ok=ok)
    lines += entry(90, 3.0, "tf_register <arguments-redacted>", ["    > [TF] registration request sent"])
    for index, (cp, view) in enumerate(zip(scenario.checkpoints, views)):
        frame = 500 + 100 * index
        if isinstance(view, dict):
            view = observation("client", self_id, view)
        lines += entry(frame, cp, "tf_observe", view if view is not None else [])
    return "\n".join(lines) + "\n"


def logs(client_views: tuple[list, list] | None = None, server_text: str | None = None, returncodes=(0, 0, 0),
         client_ok: bool = True) -> tuple[multiclient.RoleLog, list[multiclient.RoleLog]]:
    views = client_views or (list(TRUTH), list(TRUTH))
    server = multiclient.RoleLog("server", returncodes[0], SERVER_ANCHOR,
                                 multiclient.parse_audit(server_text if server_text is not None else server_audit()))
    clients = []
    for index, (self_id, faction) in enumerate(zip(CLIENT_PLAYERS, ("mra", "auc"))):
        text = client_audit(self_id, views[index], ok=client_ok)
        clients.append(multiclient.RoleLog(f"client{index + 1}", returncodes[index + 1], CLIENT_ANCHOR,
                                           multiclient.parse_audit(text), faction))
    return server, clients


def skewed_run(scenario: multiclient.Scenario, skew: float) -> dict:
    """Evaluate a run whose second client's clock started @p skew seconds after the first's.

    Every view is the true world at the instant it was taken, so any problem the
    comparator reports is a false failure caused by the schedule, not divergence.
    """
    anchors = (CLIENT_ANCHOR, CLIENT_ANCHOR + skew)
    server = multiclient.RoleLog("server", 0, SERVER_ANCHOR,
                                 multiclient.parse_audit(server_audit(anchors=anchors, scenario=scenario)))
    clients = []
    for index, (anchor, self_id, faction) in enumerate(zip(anchors, CLIENT_PLAYERS, ("mra", "auc"))):
        views = [world_at(anchor + cp, anchors, scenario) for cp in scenario.checkpoints]
        text = client_audit(self_id, views, scenario=scenario)
        clients.append(multiclient.RoleLog(f"client{index + 1}", 0, anchor, multiclient.parse_audit(text), faction))
    return multiclient.evaluate(scenario, server, clients)


def moved(view: dict, player: int, dx: float) -> dict:
    changed = dict(view)
    faction, cls, health, (x, y, z) = changed[player]
    changed[player] = (faction, cls, health, (x + dx, y, z))
    return changed


def parsed(role: str, self_id: int, pawns: dict, **kwargs) -> multiclient.Observation:
    observations = multiclient.parse_observations(observation(role, self_id, pawns, **kwargs))
    assert len(observations) == 1, "fixture observation must be complete"
    return observations[0]


# --------------------------------------------------------------------------- scripted runs for the scenarios


def scripted_run(scenario: multiclient.Scenario, states: list[dict], client_ids: list[tuple[int | None, ...]],
                 server_extra: list[str] | None = None) -> tuple[multiclient.RoleLog, list[multiclient.RoleLog]]:
    """Audits for @p scenario where the server shows states[i] around checkpoint i.

    states[i]: keyword arguments for observation() (pawns, regions, vehicles,
    players). client_ids[i][c] is client c's own id at checkpoint i, or None for
    a client that printed nothing. Every client sees exactly the server's world
    minus its own progression-free fields, so the comparator converges and only
    the scenario verdict decides.
    """
    lag = CLIENT_ANCHOR - SERVER_ANCHOR

    def state_at(server_seconds: float) -> dict:
        client_time = server_seconds - lag
        return states[min(range(len(states)), key=lambda i: abs(scenario.checkpoints[i] - client_time))]

    lines = entry(0, 0.0, "tf_status", ["    > [TF] TERRAFRONT role=standalone"])
    at, frame = 1.0, 30
    while at < scenario.client_seconds + lag + 5.0:
        lines += entry(frame, at, "tf_observe", observation("server", SERVER_SELF, **state_at(at)))
        at += multiclient.SERVER_OBSERVE_INTERVAL_S
        frame += 15
    lines += server_extra or []
    server = multiclient.RoleLog("server", 0, SERVER_ANCHOR, multiclient.parse_audit("\n".join(lines) + "\n"))

    clients = []
    for number, faction in enumerate(scenario.client_factions):
        views = []
        for index, state in enumerate(states):
            self_id = client_ids[index][number]
            views.append(None if self_id is None else observation("client", self_id, **state))
        text = client_audit(0, views, scenario=scenario)
        clients.append(multiclient.RoleLog(f"client{number + 1}", 0, CLIENT_ANCHOR, multiclient.parse_audit(text),
                                           faction))
    return server, clients


def pawn(faction: int, health: int, pos: tuple[float, float, float]) -> tuple:
    return (faction, 2, health, pos)


ARENA_A = (1700.0, 30.0, 2700.0)
ARENA_B = (1708.0, 30.0, 2700.0)
SANCTUARY_PAD = (296.0, 24.0, 3744.0)


def combat_states(kills_after: int = 1, respawn_health: int = 450) -> list[dict]:
    a, b = 2, 1
    return [
        {"pawns": {a: pawn(1, 450, ARENA_A), b: pawn(2, 450, ARENA_B)}},
        {"pawns": {a: pawn(1, 450, ARENA_A), b: pawn(2, 270, ARENA_B)}},
        {"pawns": {a: pawn(1, 450, ARENA_A)}, "players": {a: ("default", 0, 1, kills_after)}},
        {"pawns": {a: pawn(1, 450, ARENA_A), b: pawn(2, respawn_health, ARENA_B)},
         "players": {a: ("default", 0, 1, kills_after)}},
    ]


COMBAT_IDS = [(2, 1), (2, 1), (2, 1), (2, 1)]


def forged_lines(kinds: tuple[str, ...], forged: int, player: int = 2) -> list[str]:
    lines = entry(2000, 40.0, "tf_cheat_stats",
                  ["    > [TF] anti-cheat violations (1 player):", f"  p{player}  moveClamps=0 (spikes=0)  "
                   f"fireRateRejects=0  fireOriginRejects=0  inputRateRejects=0  forged={forged}"])
    for index, kind in enumerate(kinds):
        lines += entry(1000 + index, 30.0 + index, "tf_observe",
                       [f"    > [Game] [TF-AUDIT] forged-state kind={kind} player={player} total={index + 1}"])
    return lines


def forged_states(saved: str = multiclient.FORGED_LEGAL_PRIMARY) -> list[dict]:
    world = {2: pawn(1, 450, SANCTUARY_PAD), 1: pawn(2, 450, (344.0, 24.0, 3744.0))}
    return [{"pawns": world, "players": {2: (multiclient.FORGED_LEGAL_PRIMARY, 0, 1, 0)}},
            {"pawns": world, "players": {2: (saved, 0, 1, 0)}}]


def reconnect_states(before: tuple = (multiclient.RECONNECT_LEGAL_PRIMARY, 12, 1, 0),
                     after: tuple = (multiclient.RECONNECT_LEGAL_PRIMARY, 12, 1, 0)) -> list[dict]:
    a_pawn = pawn(1, 450, SANCTUARY_PAD)
    b_pawn = pawn(2, 450, (344.0, 24.0, 3744.0))
    return [{"pawns": {2: a_pawn, 1: b_pawn}, "players": {1: before}},
            {"pawns": {2: a_pawn}},
            {"pawns": {2: a_pawn, 3: b_pawn}, "players": {3: after}}]


RECONNECT_IDS = [(2, 1), (2, None), (2, 3)]


def vehicle_states(appears: bool = True, driver_in_seat: bool = True, drives: bool = True) -> list[dict]:
    a, b = 2, 1
    world = {a: pawn(1, 450, (2052.0, 30.0, 3520.0)), b: pawn(2, 450, (2056.0, 30.0, 3530.0))}
    pad = (2048.0, 30.0, 3520.0)
    driven = (2048.0, 30.0, 3500.0) if drives else pad
    seated = a if driver_in_seat else multiclient.NO_PLAYER
    drifter = multiclient.DRIFTER_KIND

    def vehicles(driver: int, pos: tuple) -> dict:
        return {77: (drifter, driver, 1800, pos)} if appears else {}

    return [
        {"pawns": world, "players": {a: ("default", 200, 1, 0)}},
        {"pawns": world, "vehicles": vehicles(multiclient.NO_PLAYER, pad), "players": {a: ("default", 150, 1, 0)}},
        {"pawns": world, "vehicles": vehicles(seated, pad), "players": {a: ("default", 150, 1, 0)}},
        {"pawns": world, "vehicles": vehicles(seated, driven), "players": {a: ("default", 150, 1, 0)}},
        {"pawns": world, "vehicles": vehicles(multiclient.NO_PLAYER, driven), "players": {a: ("default", 150, 1, 0)}},
        {"pawns": world, "players": {a: ("default", 150, 1, 0)}},
    ]


VEHICLE_IDS = [(2, 1)] * 6


def territory_states(owners: tuple[int, ...] = multiclient.TERRITORY_OWNERS) -> list[dict]:
    world = {2: pawn(1, 450, SANCTUARY_PAD), 1: pawn(2, 450, (344.0, 24.0, 3744.0))}
    return [{"pawns": world, "regions": {**DEFAULT_REGIONS, multiclient.TERRITORY_REGION: owner}} for owner in owners]


def run_scenario(name: str, states: list[dict], ids: list[tuple], server_extra: list[str] | None = None) -> dict:
    scenario = multiclient.SCENARIOS[name]
    server, clients = scripted_run(scenario, states, ids, server_extra)
    return multiclient.evaluate(scenario, server, clients)


class ParserTests(unittest.TestCase):
    def test_interleaved_lines_do_not_break_an_observation(self) -> None:
        lines = observation("client", 2, TRUTH[0])
        lines.insert(3, "    > [TF] player 1 entered world as character 7 (Aurum Combine)")
        lines.insert(1, "[Net] unrelated log line")
        observations = multiclient.parse_observations(lines)
        self.assertEqual(len(observations), 1)
        self.assertEqual(sorted(observations[0].pawns), [1, 2])
        self.assertEqual(observations[0].pawns[1].pos, (344.0, 24.0, 3808.0))

    def test_truncated_observation_is_not_complete(self) -> None:
        lines = observation("client", 2, TRUTH[0])
        del lines[3]  # one pawn line lost
        self.assertEqual(multiclient.parse_observations(lines), [])

    def test_server_view_without_progression_lines_is_not_complete(self) -> None:
        # RED proof: a server that stopped printing player lines could not back any progression claim.
        lines = [line for line in observation("server", SERVER_SELF, TRUTH[0]) if "] player " not in line]
        self.assertEqual(multiclient.parse_observations(lines), [])
        self.assertEqual(len(multiclient.parse_observations(observation("server", SERVER_SELF, TRUTH[0]))), 1)

    def test_progression_vehicle_and_region_lines_parse(self) -> None:
        view = parsed("server", SERVER_SELF, TRUTH[0], vehicles={9: (1, 2, 1800, (1.0, 2.0, 3.0))},
                      players={2: ("mra_rifle", 40, 3, 1)})
        self.assertEqual(view.players[2], multiclient.Progress(2, "mra_rifle", 40, 3, 1))
        self.assertEqual(view.vehicles[9], multiclient.Vehicle(9, 1, 2, 1800, (1.0, 2.0, 3.0)))
        self.assertEqual(view.regions, DEFAULT_REGIONS)

    def test_malformed_body_line_invalidates_only_its_observation(self) -> None:
        good = observation("client", 2, TRUTH[0])
        bad = observation("client", 2, TRUTH[1])
        bad[2] = bad[2].replace("pos=", "pos=nan-ish,")
        self.assertEqual(len(multiclient.parse_observations(good + bad)), 1)

    def test_partial_last_line_is_dropped(self) -> None:
        text = "frame 1 t=0.0s | ok  | tf_status\n    > done\nframe 2 t=0.5s | ok  | tf_obs"
        entries = multiclient.parse_audit(text)
        self.assertEqual([e.command for e in entries], ["tf_status"])
        self.assertEqual(entries[0].output, ["done"])

    def test_crlf_audit_parses(self) -> None:
        entries = multiclient.parse_audit("frame 3 t=1.5s | ERR | tf_spawn\r\n    > nope\r\n")
        self.assertEqual((entries[0].frame, entries[0].seconds, entries[0].ok), (3, 1.5, False))

    def test_stale_observation_from_the_previous_command_is_ignored(self) -> None:
        # The audit window replays the previous observe; a probe that printed
        # nothing must yield no sample instead of inheriting that stale view.
        stale = ["    > [exec] frame 10 (t=1.0s): tf_observe", *observation("client", 2, TRUTH[0])]
        text = "\n".join([*entry(10, 1.0, "tf_observe", observation("client", 2, TRUTH[0])),
                          "frame 20 t=2.0s | ok  | tf_observe", *stale,
                          "    > [exec] frame 20 (t=2.0s): tf_observe"]) + "\n"
        samples = multiclient.observe_samples(multiclient.parse_audit(text))
        self.assertIsNotNone(samples[0].observation)
        self.assertIsNone(samples[1].observation)

        # The extended engine marker preserves scoping, including catch-up in
        # one frame. Older trails above remain readable by this analysis tool.
        text = text.replace("(t=1.0s):", "(t=1.0s, entry=0):")
        text = text.replace("frame 20 t=2.0s", "frame 10 t=1.0s")
        text = text.replace("frame 20 (t=2.0s):", "frame 10 (t=1.0s, entry=1):")
        samples = multiclient.observe_samples(multiclient.parse_audit(text))
        self.assertIsNotNone(samples[0].observation)
        self.assertIsNone(samples[1].observation)

    def test_cheat_stats_and_audit_lines_parse(self) -> None:
        entries = multiclient.parse_audit("\n".join(forged_lines(("loadout-ineligible",), 1)) + "\n")
        self.assertEqual(multiclient.cheat_stats_snapshots(entries), [{2: 1}])
        self.assertEqual(multiclient.forged_audit_records(entries), {("loadout-ineligible", 2)})


class ComparatorTests(unittest.TestCase):
    def evaluate(self, *args, **kwargs) -> dict:
        server, clients = logs(*args, **kwargs)
        return multiclient.evaluate(SCENARIO, server, clients)

    def test_converged_run_passes(self) -> None:
        summary = self.evaluate()
        self.assertTrue(summary["passed"], summary["problems"])
        self.assertEqual([c["converged"] for c in summary["checkpoints"]], [True, True, True])

    def test_position_divergence_fails(self) -> None:
        diverged = list(TRUTH)
        diverged[1] = moved(TRUTH[1], 1, multiclient.POSITION_TOLERANCE_M + 0.5)
        summary = self.evaluate((list(TRUTH), diverged))
        self.assertFalse(summary["passed"])
        self.assertIn("position", " ".join(summary["checkpoints"][1]["problems"]))

    def test_small_interpolation_gap_is_tolerated(self) -> None:
        near = [moved(view, 1, 0.75) for view in TRUTH]
        self.assertTrue(self.evaluate((near, list(TRUTH)))["passed"])

    def test_missing_pawn_fails(self) -> None:
        missing = list(TRUTH)
        missing[2] = {2: TRUTH[2][2]}
        summary = self.evaluate((missing, list(TRUTH)))
        self.assertFalse(summary["passed"])
        self.assertIn("players", " ".join(summary["checkpoints"][2]["problems"]))

    def test_health_or_faction_mismatch_fails(self) -> None:
        wrong = list(TRUTH)
        faction, cls, _, pos = TRUTH[0][1]
        wrong[0] = {**TRUTH[0], 1: (faction, cls, 300, pos)}
        self.assertFalse(self.evaluate((wrong, list(TRUTH)))["passed"])
        wrong[0] = {**TRUTH[0], 1: (3, cls, 450, pos)}
        self.assertFalse(self.evaluate((wrong, list(TRUTH)))["passed"])

    def test_missing_checkpoint_fails(self) -> None:
        summary = self.evaluate((list(TRUTH[:2]), list(TRUTH)))
        self.assertFalse(summary["passed"])
        self.assertIn("checkpoint 2 was not observed", " ".join(summary["checkpoints"][2]["problems"]))

    def test_zero_checkpoint_logs_fail(self) -> None:
        # RED proof of the "stopped checking" rule: no observations at all is a failure.
        summary = self.evaluate(([], []), server_text="")
        self.assertFalse(summary["passed"])
        self.assertIn("0/3 checkpoints converged", summary["problems"])

    def test_server_that_stopped_observing_fails(self) -> None:
        summary = self.evaluate(server_text=server_audit(until=35.0))
        self.assertFalse(summary["passed"])
        self.assertIn("server: no observation", " ".join(summary["checkpoints"][2]["problems"]))

    def test_pawns_that_never_moved_fail(self) -> None:
        frozen = [TRUTH[0]] * 3
        server = multiclient.RoleLog("server", 0, SERVER_ANCHOR, multiclient.parse_audit(
            "\n".join(line for at in range(2, 140) for line in
                      entry(at, at * 0.5, "tf_observe", observation("server", 7, TRUTH[0]))) + "\n"))
        clients = [multiclient.RoleLog("client1", 0, CLIENT_ANCHOR, multiclient.parse_audit(client_audit(2, frozen)),
                                       "mra"),
                   multiclient.RoleLog("client2", 0, CLIENT_ANCHOR, multiclient.parse_audit(client_audit(1, frozen)),
                                       "auc")]
        summary = multiclient.evaluate(SCENARIO, server, clients)
        self.assertFalse(summary["passed"])
        self.assertTrue(any(p.startswith("movement 0->1") for p in summary["problems"]))

    def test_wrong_server_faction_fails(self) -> None:
        server, clients = logs()
        clients[0].faction = "hlx"
        self.assertFalse(multiclient.evaluate(SCENARIO, server, clients)["passed"])

    def test_nonzero_child_exit_fails(self) -> None:
        summary = self.evaluate(returncodes=(0, 3, 0))
        self.assertFalse(summary["passed"])
        self.assertIn("client1: exited with 3", summary["problems"])

    def test_command_error_fails(self) -> None:
        summary = self.evaluate(client_ok=False)
        self.assertFalse(summary["passed"])
        self.assertTrue(any("reported ERR" in p for p in summary["problems"]))

    def test_region_digest_mismatch_fails(self) -> None:
        server = parsed("server", SERVER_SELF, TRUTH[0])
        client = parsed("client", 2, TRUTH[0], regions={**DEFAULT_REGIONS, 3: 2})
        self.assertEqual(multiclient.compare_regions(server, server, "client1"), [])
        self.assertIn("regions [3]", " ".join(multiclient.compare_regions(server, client, "client1")))
        self.assertTrue(multiclient.compare_views(server, client, "client1"))

    def test_vehicle_divergence_fails(self) -> None:
        vehicle = {5: (1, multiclient.NO_PLAYER, 1800, (10.0, 0.0, 10.0))}
        server = parsed("server", SERVER_SELF, TRUTH[0], vehicles=vehicle)
        self.assertEqual(multiclient.compare_vehicles(server, server, "client1"), [])
        missing = parsed("client", 2, TRUTH[0])
        self.assertIn("vehicles [] != server [5]", " ".join(multiclient.compare_vehicles(server, missing, "c")))
        seated = parsed("client", 2, TRUTH[0], vehicles={5: (1, 2, 1800, (10.0, 0.0, 10.0))})
        self.assertIn("driver", " ".join(multiclient.compare_vehicles(server, seated, "c")))
        drifted = parsed("client", 2, TRUTH[0], vehicles={5: (1, multiclient.NO_PLAYER, 1800, (14.0, 0.0, 10.0))})
        self.assertIn("position", " ".join(multiclient.compare_vehicles(server, drifted, "c")))

    def test_pure_client_self_line_is_not_compared_with_the_server_wallet(self) -> None:
        # A pure client prints flux=0 rank=1 on its self line whatever it owns; a
        # tf_flux_floor wallet and a higher rank on the server are not divergence.
        truth = {2: ("default", 200, 3, 0)}
        server = parsed("server", SERVER_SELF, TRUTH[0], players=truth)
        client = parsed("client", 2, TRUTH[0], players=truth)
        self.assertEqual((client.self_progress.flux, client.self_progress.rank), (0, 1))
        self.assertEqual(multiclient.compare_views(server, client, "c"), [])


class ScheduleTests(unittest.TestCase):
    # The schedule this harness first shipped: checkpoint 0 one second before a walk.
    TIGHT = replace(SCENARIO, name="tight", client_steps=(*SCENARIO.client_steps[:-2], (18.0, "tf_walk 1 0 2"),
                                                          (28.0, "tf_walk 0 1 2")),
                    checkpoints=(17.0, 25.0, 35.0), client_seconds=38.0)

    def test_every_scenario_keeps_its_checkpoints_quiet(self) -> None:
        for scenario in multiclient.SCENARIOS.values():
            self.assertEqual(multiclient.schedule_violations(scenario), [])

    def test_largest_accepted_skew_still_converges(self) -> None:
        summary = skewed_run(SCENARIO, multiclient.MAX_CLIENT_SKEW_S - 0.05)
        self.assertTrue(summary["passed"], summary["problems"])

    def test_checkpoint_near_a_walk_is_rejected(self) -> None:
        # RED proof: an accepted skew catches one pawn mid-walk and a correct run
        # reads as divergence, so a schedule like this must never reach the processes.
        self.assertIn("position", " ".join(skewed_run(self.TIGHT, 1.5)["checkpoints"][0]["problems"]))
        violations = multiclient.schedule_violations(self.TIGHT)
        self.assertTrue(any("checkpoint 0" in v for v in violations), violations)
        with mock.patch.dict(multiclient.SCENARIOS, {"onboard_spawn_move": self.TIGHT}):
            out = io.StringIO()
            with tempfile.TemporaryDirectory() as workdir, redirect_stdout(out), redirect_stderr(io.StringIO()):
                code = multiclient.main(["--engine", sys.executable, "--module", __file__, "--scenario",
                                         "onboard_spawn_move", "--workdir", workdir])
                self.assertEqual(list(Path(workdir).iterdir()), [])  # refused before launching anything
        self.assertEqual(code, 1)
        self.assertIn("must be quiet for more than", out.getvalue())

    def test_checkpoint_inside_a_server_step_window_is_rejected(self) -> None:
        territory = multiclient.SCENARIOS["territory"]
        early = replace(territory, checkpoints=(16.0, 28.0, 54.0))
        self.assertTrue(any("checkpoint 1" in v for v in multiclient.schedule_violations(early)))
        narrow = replace(territory, server_steps=(multiclient.ServerStep(20.0, 25.0, "tf_capture 3 auc"),))
        self.assertTrue(any("narrower" in v for v in multiclient.schedule_violations(narrow)))

    def test_server_steps_land_in_their_window_for_every_allowed_lag(self) -> None:
        for scenario in multiclient.SCENARIOS.values():
            for step in scenario.server_steps:
                times = multiclient.server_step_times(step)
                self.assertTrue(times, step)
                for lag in (multiclient.CLIENT_LAG_MIN_S, multiclient.CLIENT_LAG_MAX_S):
                    for at in times:
                        self.assertTrue(step.start - 1e-9 <= at - lag <= step.end + 1e-9, (step, lag, at))

    def test_client_lag_outside_the_schedule_fails(self) -> None:
        server, clients = scripted_run(multiclient.SCENARIOS["territory"], territory_states(), [(2, 1)] * 3)
        self.assertEqual(multiclient.check_client_lag(multiclient.SCENARIOS["territory"], server, clients), [])
        clients[1].anchor = SERVER_ANCHOR + multiclient.CLIENT_LAG_MAX_S + 2.0
        problems = multiclient.check_client_lag(multiclient.SCENARIOS["territory"], server, clients)
        self.assertIn("client2: its clock started", " ".join(problems))
        summary = multiclient.evaluate(multiclient.SCENARIOS["territory"], server, clients)
        self.assertIn("client2: its clock started", " ".join(summary["problems"]))


class ScenarioVerdictTests(unittest.TestCase):
    def assert_passes(self, summary: dict) -> None:
        self.assertTrue(summary["passed"], summary["problems"])

    def assert_fails_with(self, summary: dict, text: str) -> None:
        self.assertFalse(summary["passed"])
        self.assertIn(text, " ".join(summary["problems"]))

    def test_combat_kill_attributed_and_respawned_passes(self) -> None:
        self.assert_passes(run_scenario("combat_kill_respawn", combat_states(), COMBAT_IDS))

    def test_combat_kill_not_credited_fails(self) -> None:
        self.assert_fails_with(run_scenario("combat_kill_respawn", combat_states(kills_after=0), COMBAT_IDS),
                               "not credited")

    def test_combat_respawn_without_full_health_fails(self) -> None:
        self.assert_fails_with(run_scenario("combat_kill_respawn", combat_states(respawn_health=300), COMBAT_IDS),
                               "full health")

    def test_combat_victim_that_never_died_fails(self) -> None:
        states = combat_states()
        states[2]["pawns"][1] = pawn(2, 270, ARENA_B)
        summary = run_scenario("combat_kill_respawn", states, COMBAT_IDS)
        self.assertFalse(summary["passed"])
        self.assertIn("should be out of the world", " ".join(summary["checkpoints"][2]["problems"]))

    def test_combat_inside_the_sanctuary_fails(self) -> None:
        states = combat_states()
        states[0]["pawns"][2] = pawn(1, 450, SANCTUARY_PAD)
        self.assert_fails_with(run_scenario("combat_kill_respawn", states, COMBAT_IDS), "sanctuary")

    def test_forged_state_rejected_and_audited_passes(self) -> None:
        extra = forged_lines(("loadout-ineligible", "loadout-unknown-weapon"), 2)
        self.assert_passes(run_scenario("forged_state", forged_states(), [(2, 1)] * 2, extra))

    def test_forged_state_rejected_but_not_audited_fails(self) -> None:
        # RED proof: an unchanged loadout alone is "rejected", not "rejected and audited".
        extra = forged_lines(("loadout-ineligible",), 2)
        self.assert_fails_with(run_scenario("forged_state", forged_states(), [(2, 1)] * 2, extra),
                               "kind=loadout-unknown-weapon")

    def test_forged_state_audited_but_applied_fails(self) -> None:
        extra = forged_lines(("loadout-ineligible", "loadout-unknown-weapon"), 2)
        self.assert_fails_with(run_scenario("forged_state", forged_states(saved="auc_rifle"), [(2, 1)] * 2, extra),
                               "expected mra_rifle")

    def test_forged_state_without_counters_fails(self) -> None:
        extra = forged_lines(("loadout-ineligible", "loadout-unknown-weapon"), 1)
        self.assert_fails_with(run_scenario("forged_state", forged_states(), [(2, 1)] * 2, extra),
                               "forged-state counters")

    def test_reconnect_restoring_saved_state_passes(self) -> None:
        self.assert_passes(run_scenario("reconnect", reconnect_states(), RECONNECT_IDS))

    def test_reconnect_to_default_kit_fails(self) -> None:
        # RED proof: nothing persisted, so both snapshots are default kit; equal is not proof.
        default = ("default", 0, 1, 0)
        summary = run_scenario("reconnect", reconnect_states(before=default, after=default), RECONNECT_IDS)
        self.assert_fails_with(summary, "default kit")

    def test_reconnect_restoring_the_forged_loadout_fails(self) -> None:
        forged = (multiclient.RECONNECT_FORGED_PRIMARY, 12, 1, 0)
        self.assert_fails_with(run_scenario("reconnect", reconnect_states(after=forged), RECONNECT_IDS),
                               "was restored")

    def test_reconnect_losing_the_wallet_fails(self) -> None:
        poorer = (multiclient.RECONNECT_LEGAL_PRIMARY, 0, 1, 0)
        self.assert_fails_with(run_scenario("reconnect", reconnect_states(after=poorer), RECONNECT_IDS), "flux 0")

    def test_reconnect_still_in_world_while_offline_fails(self) -> None:
        states = reconnect_states()
        states[1]["pawns"][1] = pawn(2, 450, (344.0, 24.0, 3744.0))
        summary = run_scenario("reconnect", states, RECONNECT_IDS)
        self.assertFalse(summary["passed"])
        self.assertIn("should be out of the world", " ".join(summary["checkpoints"][1]["problems"]))

    def test_territory_flip_and_flip_back_passes(self) -> None:
        self.assert_passes(run_scenario("territory", territory_states(), [(2, 1)] * 3))

    def test_territory_that_never_flipped_fails(self) -> None:
        mra = multiclient.FACTION_IDS["mra"]
        self.assert_fails_with(run_scenario("territory", territory_states((mra, mra, mra)), [(2, 1)] * 3),
                               "expected 2")

    def test_vehicle_lifecycle_passes(self) -> None:
        self.assert_passes(run_scenario("vehicle_lifecycle", vehicle_states(), VEHICLE_IDS))

    def test_vehicle_that_never_appears_fails(self) -> None:
        # RED proof: a purchase that silently did nothing must fail, not skip.
        summary = run_scenario("vehicle_lifecycle", vehicle_states(appears=False), VEHICLE_IDS)
        self.assert_fails_with(summary, "purchased: expected exactly one vehicle, found 0")

    def test_vehicle_never_boarded_fails(self) -> None:
        self.assert_fails_with(run_scenario("vehicle_lifecycle", vehicle_states(driver_in_seat=False), VEHICLE_IDS),
                               "entered: driver")

    def test_vehicle_that_did_not_move_fails(self) -> None:
        self.assert_fails_with(run_scenario("vehicle_lifecycle", vehicle_states(drives=False), VEHICLE_IDS),
                               "did not move")

    def test_vehicle_purchase_without_payment_fails(self) -> None:
        states = vehicle_states()
        for state in states[1:]:
            state["players"] = {2: ("default", 200, 1, 0)}
        self.assert_fails_with(run_scenario("vehicle_lifecycle", states, VEHICLE_IDS), "took 0 flux")

    def test_verdict_without_views_fails(self) -> None:
        views = multiclient.RunViews([None, None], [[None, None], [None, None]], [])
        self.assertTrue(multiclient.forged_verdict(views))
        self.assertTrue(multiclient.territory_verdict(views))


class ScriptTests(unittest.TestCase):
    def test_client_script_uses_fresh_credentials_and_schedules_every_checkpoint(self) -> None:
        first = multiclient.client_script(SCENARIO, 23000, "mra")
        second = multiclient.client_script(SCENARIO, 23000, "mra")
        self.assertNotEqual(first, second)
        self.assertEqual(first.count("tf_observe"), len(SCENARIO.checkpoints))
        self.assertIn("tf_connect 127.0.0.1:23000", first)
        self.assertTrue(first.startswith("0 tf_status\n"))

    def test_solo_steps_reach_only_their_client_and_reuse_its_credentials(self) -> None:
        scenario = multiclient.SCENARIOS["reconnect"]
        stayer = multiclient.client_script(scenario, 23000, "mra", 0)
        returner = multiclient.client_script(scenario, 23000, "auc", 1)
        self.assertNotIn("tf_disconnect", stayer)
        self.assertIn("tf_disconnect", returner)
        logins = [line.split(" ", 1)[1] for line in returner.splitlines() if " tf_login " in line]
        self.assertEqual(len(logins), 2)
        self.assertEqual(logins[0], logins[1])

    def test_server_script_observes_through_the_run(self) -> None:
        script = multiclient.server_script(23000, 20.0).splitlines()
        self.assertEqual(script[:2], ["0 tf_status", "t0.5 tf_dedicated 23000"])
        self.assertEqual(script[-1], "t19.0 tf_observe")

    def test_server_script_repeats_each_step_across_its_window(self) -> None:
        scenario = multiclient.SCENARIOS["territory"]
        script = multiclient.server_script(23000, scenario.client_seconds + multiclient.SERVER_TAIL_S, scenario)
        for step in scenario.server_steps:
            expected = [f"t{multiclient.format_seconds(at)} {step.command}"
                        for at in multiclient.server_step_times(step)]
            for line in expected:
                self.assertIn(line, script.splitlines())


class ProcessTests(unittest.TestCase):
    def test_engine_that_exits_early_fails_the_run(self) -> None:
        # A stand-in "engine" (the interpreter) exits without ever writing an
        # audit: the run must fail, not report zero divergences.
        with tempfile.TemporaryDirectory() as workdir:
            # A previous run's append-only trail must not survive into this run.
            stale = Path(workdir) / "server" / "exec_audit.log"
            stale.parent.mkdir()
            stale.write_text(server_audit(), encoding="utf-8")
            out, err = io.StringIO(), io.StringIO()
            with redirect_stdout(out), redirect_stderr(err):
                code = multiclient.main(["--engine", sys.executable, "--module", __file__, "--scenario",
                                         "onboard_spawn_move", "--workdir", workdir, "--timeout", "60"])
            self.assertFalse(stale.exists())
        self.assertEqual(code, 1)
        self.assertIn("server exited with", out.getvalue())


# --------------------------------------------------------------------------- impaired runs

IMPAIRMENT = multiclient.Impairment.parse("80,20,0.03,2,3", 20260927)
IMPAIRED_TRIO = ("onboard_spawn_move", "combat_kill_respawn", "territory")


def impair_probe(frame: int, seconds: float, values: tuple | None) -> list[str]:
    """One net_impair entry exactly as the engine prints InstabilitySimulator::Console_GetStatus()."""
    if values is None:
        return entry(frame, seconds, "net_impair", ["    > InstabilitySimulator: disabled"])
    lag, jitter, loss, dup, reorder, seed = values
    return entry(frame, seconds, "net_impair", [
        "    > InstabilitySimulator: ENABLED", f"  Latency:     {lag:.1f} ms", f"  Jitter:      +/-{jitter:.1f} ms",
        f"  Packet loss: {loss:.1f}%", f"  Reorder:     {reorder:.1f}% (hold 40.0 ms)", f"  Duplicate:   {dup:.1f}%",
        f"  Seed:        {seed}", "  Queued pkts: 0"])


def impaired_logs(module_on: bool = True, engine_probes: tuple[bool, ...] = (True, True)) -> tuple:
    """A converged onboard_spawn_move run under IMPAIRMENT; each flag switches one proof of it off."""
    def probes(role: int) -> list[str]:
        values = IMPAIRMENT.expected(role)
        return [line for index, on in enumerate(engine_probes)
                for line in impair_probe(1 + index, 0.0 if index == 0 else 69.0, values if on else None)]

    def net(role: int) -> str:
        return net_line(IMPAIRMENT.expected(role) if module_on else None)

    server_text = "\n".join(probes(0)) + "\n" + server_audit(net_view=net(0))
    server = multiclient.RoleLog("server", 0, SERVER_ANCHOR, multiclient.parse_audit(server_text))
    clients = []
    for index, (self_id, faction) in enumerate(zip(CLIENT_PLAYERS, ("mra", "auc"))):
        views = [observation("client", self_id, truth, net_view=net(index + 1)) for truth in TRUTH]
        text = "\n".join(probes(index + 1)) + "\n" + client_audit(self_id, views)
        clients.append(multiclient.RoleLog(f"client{index + 1}", 0, CLIENT_ANCHOR, multiclient.parse_audit(text),
                                           faction))
    return server, clients


class ImpairmentTests(unittest.TestCase):
    def evaluate(self, **kwargs) -> dict:
        server, clients = impaired_logs(**kwargs)
        return multiclient.evaluate(multiclient.impaired(SCENARIO, IMPAIRMENT), server, clients, IMPAIRMENT)

    def test_bad_impairment_arguments_are_refused(self) -> None:
        for text, seed in (("80,20,0.03,2", 1), ("0,0,0,0,0", 1), ("80,20,1.5,2,3", 1), ("80,20,0.03,2,3", 0),
                           ("80,nan,0.03,2,3", 1)):
            with self.assertRaises(ValueError, msg=text):
                multiclient.Impairment.parse(text, seed)

    def test_impaired_schedules_stay_quiet_and_widen(self) -> None:
        for name in IMPAIRED_TRIO:
            base = multiclient.SCENARIOS[name]
            slowed = multiclient.impaired(base, IMPAIRMENT)
            self.assertEqual(multiclient.schedule_violations(slowed), [], name)
            self.assertEqual(slowed.name, base.name)  # the scenario verdict still runs
            self.assertGreater(slowed.event_settle_s, base.event_settle_s)
            self.assertGreater(slowed.position_tolerance_m, base.position_tolerance_m)
            self.assertEqual(len(slowed.checkpoints), len(base.checkpoints))
            self.assertGreaterEqual(slowed.client_seconds, slowed.checkpoints[-1])

    def test_converged_run_with_live_impairment_passes(self) -> None:
        summary = self.evaluate()
        self.assertTrue(summary["passed"], summary["problems"])

    def test_module_image_without_impairment_fails_although_converged(self) -> None:
        # RED proof: the engine probes say ENABLED and every checkpoint converged, but
        # the module image that impairs server sends never was: that is not impaired.
        summary = self.evaluate(module_on=False)
        self.assertTrue(all(c["converged"] for c in summary["checkpoints"]))
        self.assertFalse(summary["passed"])
        self.assertIn("module-image impairment is disabled", " ".join(summary["problems"]))

    def test_engine_simulator_switched_off_fails(self) -> None:
        summary = self.evaluate(engine_probes=(True, False))
        self.assertFalse(summary["passed"])
        self.assertIn("reported disabled", " ".join(summary["problems"]))

    def test_missing_final_probe_fails(self) -> None:
        summary = self.evaluate(engine_probes=(True,))
        self.assertFalse(summary["passed"])
        self.assertIn("1 net_impair probes", " ".join(summary["problems"]))

    def test_wrong_value_fails(self) -> None:
        wrong = multiclient.Impairment.parse("40,20,0.03,2,3", 20260927)
        server, clients = impaired_logs()
        summary = multiclient.evaluate(multiclient.impaired(SCENARIO, wrong), server, clients, wrong)
        self.assertFalse(summary["passed"])

    def test_impaired_scripts_configure_before_connecting_and_probe_at_the_end(self) -> None:
        script = multiclient.client_script(multiclient.SCENARIOS["territory"], 23000, "mra", 0,
                                           impairment=IMPAIRMENT).splitlines()
        self.assertEqual(script[:8], ["0 tf_status", "0 net_impair_seed 20260928", "0 net_lag 80", "0 net_jitter 20",
                                      "0 net_loss 0.03", "0 net_dup 2", "0 net_reorder 3", "0 net_impair"])
        self.assertTrue(script[-1].endswith(" net_impair"))
        server = multiclient.server_script(23000, 40.0, impairment=IMPAIRMENT).splitlines()
        self.assertIn("0 net_impair_seed 20260927", server)
        self.assertLess(server.index("0 net_impair"), server.index("t0.5 tf_dedicated 23000"))

    def test_impair_is_refused_outside_the_convergence_scenarios(self) -> None:
        for extra in (["--scenario", "cold_restart"], ["--soak-seconds", "120"]):
            with tempfile.TemporaryDirectory() as workdir, redirect_stderr(io.StringIO()), \
                    self.assertRaises(SystemExit):
                multiclient.main(["--engine", sys.executable, "--module", __file__, "--workdir", workdir,
                                  "--impair", "80,20,0.03,2,3", *extra])


# --------------------------------------------------------------------------- soak

SOAK_BUDGETS = multiclient.load_soak_budgets(multiclient.SOAK_BUDGETS_PATH)
SOAK_SECONDS = 120.0


def soak_logs(perf_until: float = SOAK_SECONDS, tick_ms: float = 4.0, downlink_kbps: float = 40.0,
              bots: int = 32) -> tuple:
    """A soak whose server reports tick totals and bot counts and whose clients report traffic every interval."""
    interval = SOAK_BUDGETS["sampleIntervalS"]
    lines = entry(0, 0.0, "tf_status", ["    > [TF] TERRAFRONT role=standalone"])
    at, frame = interval, 100
    while at < perf_until:
        lines += entry(frame, at + 0.2, "tf_perf", [
            "    > [TF] server tick perf (budget 16.667 ms @ 60 Hz):",
            f"  movement: avg {tick_ms / 2:.3f} ms  peak {tick_ms:.3f} ms  (n=120)",
            f"  ai: avg {tick_ms / 2:.3f} ms  peak {tick_ms:.3f} ms  (n=120)",
            f"  measured total: {tick_ms:.3f} ms   headroom: {16.667 - tick_ms:.3f} ms (76%)"])
        lines += entry(frame + 1, at + 0.6, "tf_bots", [f"    > [TF] bots active: {bots}  (tf_bots <0-32> to set)"])
        at, frame = at + interval, frame + 10
    server = multiclient.RoleLog("server", 0, SERVER_ANCHOR, multiclient.parse_audit("\n".join(lines) + "\n"))
    clients = []
    for index, self_id in enumerate(CLIENT_PLAYERS):
        lines = entry(0, 0.0, "tf_status", ["    > [TF] TERRAFRONT role=standalone"])
        at, frame, received, packets = interval, 100, 0, 0
        while at < SOAK_SECONDS:
            view = observation("client", self_id, TRUTH[0], net_view=net_line(received=(received, packets)))
            lines += entry(frame, at + 0.1, "tf_observe", view)
            received += int(downlink_kbps * 1024 * interval)
            packets += 22 * interval
            at, frame = at + interval, frame + 10
        clients.append(multiclient.RoleLog(f"client{index + 1}", 0, CLIENT_ANCHOR,
                                           multiclient.parse_audit("\n".join(lines) + "\n")))
    return server, clients


def rss_series(growth_bytes_per_s: float = 0.0) -> list[tuple[float, int]]:
    return [(float(t), int(400 * 1024 * 1024 + growth_bytes_per_s * t + (t % 3) * 4096)) for t in range(0, 121)]


def flat_rss() -> dict:
    return {role: rss_series() for role in ("server", "client1", "client2")}


class SoakTests(unittest.TestCase):
    def evaluate(self, rss: dict | None = None, **kwargs) -> dict:
        server, clients = soak_logs(**kwargs)
        return multiclient.evaluate_soak(SOAK_BUDGETS, SOAK_SECONDS, server, clients,
                                         flat_rss() if rss is None else rss)

    def test_budget_file_is_provisional_and_complete(self) -> None:
        self.assertTrue(SOAK_BUDGETS["provisional"])
        self.assertEqual(SOAK_BUDGETS["tickHz"], 60)
        self.assertLessEqual(SOAK_BUDGETS["maxTickAvgP95Ms"], round(1000.0 / SOAK_BUDGETS["tickHz"], 1))

    def test_budget_file_missing_a_key_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "budgets.json"
            broken = {k: v for k, v in SOAK_BUDGETS.items() if k != "maxRssSlopeMiBPerHour"}
            path.write_text(multiclient.json.dumps(broken), encoding="utf-8")
            with self.assertRaises(multiclient.HarnessError):
                multiclient.load_soak_budgets(path)
            path.write_text(multiclient.json.dumps({**SOAK_BUDGETS, "provisional": False}), encoding="utf-8")
            with self.assertRaises(multiclient.HarnessError):
                multiclient.load_soak_budgets(path)

    def test_p95_is_nearest_rank(self) -> None:
        self.assertEqual(multiclient.percentile([float(v) for v in range(1, 21)], 0.95), 19.0)
        self.assertEqual(multiclient.percentile([5.0], 0.95), 5.0)

    def test_quiet_soak_passes(self) -> None:
        summary = self.evaluate()
        self.assertTrue(summary["passed"], summary["problems"])
        self.assertAlmostEqual(summary["metrics"]["client1"]["maxDownlinkKBps"], 40.0, places=3)

    def test_flat_rss_passes_and_linear_growth_fails(self) -> None:
        problems, metrics = multiclient.rss_problems("server", rss_series(), SOAK_BUDGETS, SOAK_SECONDS)
        self.assertEqual(problems, [])
        self.assertLess(abs(metrics["rssSlopeMiBPerHour"]), 1.0)
        # 128 KiB/s is 450 MiB/h: a leak the provisional cap must catch.
        problems, _ = multiclient.rss_problems("server", rss_series(128 * 1024), SOAK_BUDGETS, SOAK_SECONDS)
        self.assertIn("RSS grows", " ".join(problems))

    def test_missing_samples_fail(self) -> None:
        # RED proof: a server that stopped reporting tf_perf halfway is not a passing soak.
        summary = self.evaluate(perf_until=60.0)
        self.assertFalse(summary["passed"])
        self.assertIn("post-warmup tf_perf samples", " ".join(summary["problems"]))
        summary = self.evaluate(rss={"server": rss_series()[:5]})
        self.assertIn("RSS samples after warmup", " ".join(summary["problems"]))

    def test_budget_breaches_fail(self) -> None:
        self.assertIn("tick p95", " ".join(self.evaluate(tick_ms=20.0)["problems"]))
        self.assertIn("downlink", " ".join(self.evaluate(downlink_kbps=400.0)["problems"]))
        self.assertIn("bot counts", " ".join(self.evaluate(bots=12)["problems"]))

    def test_crashed_child_fails(self) -> None:
        server, clients = soak_logs()
        clients[1].returncode = -11
        summary = multiclient.evaluate_soak(SOAK_BUDGETS, SOAK_SECONDS, server, clients, flat_rss())
        self.assertIn("client2: exited with -11", summary["problems"])

    def test_soak_scripts_spawn_bots_and_sample(self) -> None:
        server = multiclient.soak_server_script(23000, SOAK_BUDGETS, SOAK_SECONDS)
        self.assertIn("t2.0 tf_bots 32\n", server)
        self.assertEqual(server.count(" tf_perf reset\n"), 11)
        client = multiclient.soak_client_script(23000, "mra", SOAK_BUDGETS, SOAK_SECONDS)
        self.assertEqual(client.count(" tf_observe\n"), 11)
        self.assertGreater(client.count(" tf_fire\n"), 20)


# --------------------------------------------------------------------------- cold restart

RESTART_BEFORE = ("mra_rifle", 312, 2, 1, 150, "-")  # loadout, flux, rank, kills, xp, unlocks
RESTART_REGIONS = {**DEFAULT_REGIONS, 3: 2, 7: 1}


def restart_view(player: int, progress: tuple, regions: dict | None = None) -> multiclient.Observation:
    return parsed("server", SERVER_SELF, {player: pawn(1, 450, ARENA_A)}, players={player: progress},
                  regions=RESTART_REGIONS if regions is None else regions)


def char_list_audit(ids: list[int], count: int | None = None) -> list:
    rows = [f"  [{i}] MCabc  MRA  rank 2  id {cid}" for i, cid in enumerate(ids)]
    text = "\n".join(entry(40, 4.5, "tf_char_list", ["    > [TF] no characters (log in first, or none created yet)"]) +
                     entry(50, 6.0, "tf_char_list",
                           [f"    > [TF] characters ({len(ids) if count is None else count}):", *rows])) + "\n"
    return multiclient.parse_audit(text)


class RestartTests(unittest.TestCase):
    def verdict(self, after: tuple = RESTART_BEFORE, regions_after: dict | None = None,
                chars_after: list[int] | None = None, before: tuple = RESTART_BEFORE) -> list[str]:
        return multiclient.restart_verdict(restart_view(2, before), 2, restart_view(5, after, regions_after), 5,
                                           [41], [41] if chars_after is None else chars_after)

    def test_restored_state_passes(self) -> None:
        self.assertEqual(self.verdict(), [])
        income = ("mra_rifle", 312 + multiclient.FLUX_INCOME_SLACK, 2, 1, 150, "-")
        self.assertEqual(self.verdict(after=income), [])

    def test_default_state_after_restart_fails(self) -> None:
        # RED proof: a phase 2 that shares no save root comes up with default state.
        problems = " ".join(self.verdict(after=("default", 0, 1, 0, 0, "-"), regions_after=DEFAULT_REGIONS))
        for name in ("loadout default", "flux 0", "xp 0", "kills 0", "region owners differ"):
            self.assertIn(name, problems)

    def test_default_phase_one_proves_nothing(self) -> None:
        default = ("default", 0, 1, 0, 0, "-")
        problems = multiclient.restart_verdict(restart_view(2, default, DEFAULT_REGIONS), 2,
                                               restart_view(5, default, DEFAULT_REGIONS), 5, [41], [41])
        self.assertIn("restart: phase 1 loadout default", " ".join(problems))
        self.assertIn("phase 1 region 3 owner 1", " ".join(problems))

    def test_duplicate_or_missing_character_rows_fail(self) -> None:
        self.assertIn("duplicated", " ".join(self.verdict(chars_after=[41, 42])))
        self.assertIn("duplicated", " ".join(self.verdict(chars_after=[])))

    def test_unlock_or_rank_loss_fails(self) -> None:
        self.assertIn("unlocks - was restored", " ".join(self.verdict(
            before=("mra_rifle", 312, 2, 1, 150, "wpn_x"), after=("mra_rifle", 312, 2, 1, 150, "-"))))
        self.assertIn("rank 1 was restored", " ".join(self.verdict(after=("mra_rifle", 312, 1, 1, 150, "-"))))

    def test_character_list_parse(self) -> None:
        self.assertEqual(multiclient.character_ids(char_list_audit([41])), [41])
        self.assertEqual(multiclient.character_ids(char_list_audit([41, 42])), [41, 42])
        self.assertIsNone(multiclient.character_ids(char_list_audit([41], count=2)))  # truncated rows
        self.assertIsNone(multiclient.character_ids(char_list_audit([])[:1]))

    def test_restart_schedules_are_quiet_and_only_the_graceful_one_saves(self) -> None:
        for scenario in (*multiclient.RESTART_SCENARIOS.values(), multiclient.RESTART_RETURN):
            self.assertEqual(multiclient.schedule_violations(scenario), [], scenario.name)
        saves = {name: [s for s in scenario.server_steps if s.command == "tf_save"]
                 for name, scenario in multiclient.RESTART_SCENARIOS.items()}
        self.assertTrue(saves["cold_restart"])
        self.assertEqual(saves["ungraceful_restart"], [])

    def test_returning_client_reuses_its_credentials(self) -> None:
        credentials = multiclient.fresh_credentials()
        first = multiclient.client_script(multiclient.RESTART_PHASE1, 23000, "mra", 0, credentials)
        again = multiclient.client_script(multiclient.RESTART_RETURN, 23001, "mra", 0, credentials)
        login = f"tf_login {credentials['user']} {credentials['password']}"
        self.assertIn(login, first)
        self.assertIn(login, again)
        self.assertNotIn("tf_register", again)
        self.assertNotIn("tf_char_create", again)


# --------------------------------------------------------------------------- continent identity

REFUSAL = ("[TF] server hosts continent 'cindral_wastes' but this client loaded 'veyra_highlands'; "
           "restart with TF_CONTINENT=cindral_wastes")


def on_continent(lines: list[str], continent: str) -> list[str]:
    return [line.replace("continent=cindral_wastes", f"continent={continent}") for line in lines]


def mismatch_server_log(pawns: dict | None = None, observations: int = 6,
                        continent: str = "cindral_wastes") -> multiclient.RoleLog:
    lines = entry(15, 0.5, "tf_dedicated 23000", ["    > [TF] dedicated server started on port 23000"])
    for index in range(observations):
        view = on_continent(observation("server", SERVER_SELF, pawns or {}), continent)
        lines += entry(30 + 15 * index, 1.0 + 0.5 * index, "tf_observe", view)
    return multiclient.RoleLog("server", 0, SERVER_ANCHOR, multiclient.parse_audit("\n".join(lines) + "\n"))


def mismatch_client_text(refused: bool = True, spawned: bool = False,
                         continent: str = "veyra_highlands") -> str:
    lines = entry(0, 0.0, "tf_status", ["    > [TF] TERRAFRONT role=standalone"])
    lines += entry(270, 9.0, "tf_enter 0", ["    > [TF] enter-world request sent"])
    lines += entry(315, 10.5, "tf_faction mra", [f"[WARN] {REFUSAL}"] if refused else [])
    lines += entry(345, 11.5, "tf_spawn",
                   ["    > [TF] spawn accepted: entity 7 at (300 24 3800)"] if spawned else [])
    # A refused client has disconnected and observes as a standalone host, as the real run prints it.
    lines += entry(420, 14.0, "tf_observe",
                   on_continent(observation("host", 4294967041, {}), continent))
    return "\n".join(lines) + "\n"


class ContinentIdentityTests(unittest.TestCase):
    def verdict(self, server: multiclient.RoleLog | None = None, client_text: str | None = None) -> list[str]:
        text = mismatch_client_text() if client_text is None else client_text
        client = multiclient.RoleLog("client1", 0, CLIENT_ANCHOR, multiclient.parse_audit(text), "mra")
        return multiclient.continent_mismatch_verdict(server or mismatch_server_log(), client, text)

    def test_refused_client_that_never_spawned_passes(self) -> None:
        self.assertEqual(self.verdict(), [])

    def test_missing_refusal_line_fails(self) -> None:
        # RED proof: a client without the guard logs nothing and enters the wrong continent's world.
        self.assertIn("never refused", " ".join(self.verdict(client_text=mismatch_client_text(refused=False))))

    def test_spawned_pawn_fails(self) -> None:
        on_server = self.verdict(server=mismatch_server_log(pawns={2: (1, 1, 450, (300.0, 24.0, 3800.0))}))
        self.assertIn("holds pawns [2]", " ".join(on_server))
        on_client = self.verdict(client_text=mismatch_client_text(spawned=True))
        self.assertIn("spawned a pawn", " ".join(on_client))

    def test_client_on_the_server_continent_proves_nothing(self) -> None:
        same = mismatch_client_text(refused=False, continent="cindral_wastes")
        self.assertIn("observed continents ['cindral_wastes']", " ".join(self.verdict(client_text=same)))

    def test_server_that_never_observed_fails(self) -> None:
        self.assertIn("no complete observation", " ".join(self.verdict(server=mismatch_server_log(observations=0))))

    def test_server_on_another_continent_fails(self) -> None:
        wrong = mismatch_server_log(continent="veyra_highlands")
        self.assertIn("the server hosts veyra_highlands", " ".join(self.verdict(server=wrong)))

    def test_matched_continent_runs_boot_every_role_on_the_default_continent(self) -> None:
        # The convergence and restart scenarios launch without a continent, so every role loads the default
        # even when the caller's environment names another, and their clients spawn as before.
        self.assertNotIn("continent_mismatch", multiclient.SCENARIOS)
        self.assertEqual(multiclient.CONTINENT_REFUSAL.findall(client_audit(1, list(TRUTH))), [])
        with tempfile.TemporaryDirectory() as workdir, \
                mock.patch.dict(multiclient.os.environ, {"TF_CONTINENT": "veyra_highlands"}), \
                mock.patch.object(multiclient.subprocess, "Popen") as popen:
            args = mock.Mock(engine=Path("engine"), module=Path("module"), cwd=Path(workdir))
            multiclient.launch("client1", args, Path(workdir), "", 10.0)
            self.assertNotIn("TF_CONTINENT", popen.call_args.kwargs["env"])
            multiclient.launch("client2", args, Path(workdir), "", 10.0, continent="veyra_highlands")
            self.assertEqual(popen.call_args.kwargs["env"]["TF_CONTINENT"], "veyra_highlands")

    def test_mismatch_run_with_an_engine_that_exits_early_fails(self) -> None:
        with tempfile.TemporaryDirectory() as workdir:
            out = io.StringIO()
            with redirect_stdout(out), redirect_stderr(io.StringIO()):
                code = multiclient.main(["--engine", sys.executable, "--module", __file__, "--scenario",
                                         "continent_mismatch", "--workdir", workdir, "--timeout", "60"])
        self.assertEqual(code, 1)
        self.assertIn("server exited with", out.getvalue())


if __name__ == "__main__":
    unittest.main()
