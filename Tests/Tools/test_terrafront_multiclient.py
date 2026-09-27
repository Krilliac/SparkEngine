#!/usr/bin/env python3
"""TF-110: unit tests for the TERRAFRONT multi-client harness (Tools/Terrafront/multiclient.py).

Exercises the audit parser and the convergence comparator against synthetic
exec_audit.log text in the exact format ExecScriptPlayer writes and
FormatObservation prints, so no engine build is needed. The real three-process
run is TerrafrontMultiClient_OnboardSpawnMove.
"""

from __future__ import annotations

import io
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "Tools" / "Terrafront"))

import multiclient  # noqa: E402

SCENARIO = multiclient.SCENARIOS["onboard_spawn_move"]
SERVER_ANCHOR = 100.0
CLIENT_ANCHOR = 104.0

# Server-side truth per checkpoint: player -> (faction, class, health, pos).
TRUTH = [
    {1: (2, 1, 450, (344.0, 24.0, 3808.0)), 2: (1, 1, 450, (296.0, 24.0, 3744.0))},
    {1: (2, 1, 450, (344.0, 24.0, 3823.75)), 2: (1, 1, 450, (296.0, 24.0, 3759.75))},
    {1: (2, 1, 450, (358.75, 24.0, 3823.75)), 2: (1, 1, 450, (311.0, 24.0, 3759.75))},
]


def observation(role: str, self_id: int, pawns: dict, regions: int = 2) -> list[str]:
    """FormatObservation output split into lines, as it lands in the audit (first line prefixed)."""
    lines = [f"    > [TF-OBSERVE] role={role} clock=1.000 self={self_id} continent=cindral_wastes "
             f"pawns={len(pawns)} regions={regions} vehicles=0",
             f"[TF-OBSERVE] self id={self_id} loadout=default flux=0 rank=1"]
    for player in sorted(pawns):
        faction, cls, health, (x, y, z) = pawns[player]
        lines.append(f"[TF-OBSERVE] pawn id={player} faction={faction} class={cls} health={health} "
                     f"pos={x:.2f},{y:.2f},{z:.2f}")
    lines += [f"[TF-OBSERVE] region id={r} owner=1" for r in range(regions)]
    return lines


def entry(frame: int, seconds: float, command: str, output: list[str], ok: bool = True) -> list[str]:
    header = f"frame {frame} t={seconds:.1f}s | {'ok ' if ok else 'ERR'} | {command}"
    return [header, f"    > [exec] frame {frame} (t={seconds:.1f}s): {command}", *output]


def truth_at(server_seconds: float) -> dict:
    """The world the server holds at a server-clock time (quiet windows around each checkpoint)."""
    wall = SERVER_ANCHOR + server_seconds
    index = sum(1 for cp in SCENARIO.checkpoints[1:] if wall >= CLIENT_ANCHOR + cp - 4.0)
    return TRUTH[index]


def server_audit(until: float = 70.0) -> str:
    lines = entry(0, 0.0, "tf_status", ["    > [TF] TERRAFRONT role=standalone"])
    lines += entry(15, 0.5, "tf_dedicated 23000", ["    > [TF] dedicated server started on port 23000"])
    at, frame = 1.0, 30
    while at < until:
        lines += entry(frame, at, "tf_observe", observation("server", 4294967041, truth_at(at)))
        at += multiclient.SERVER_OBSERVE_INTERVAL_S
        frame += 15
    return "\n".join(lines) + "\n"


def client_audit(self_id: int, views: list[dict | None], ok: bool = True) -> str:
    lines = entry(0, 0.0, "tf_status", ["    > [TF] TERRAFRONT role=standalone"])
    lines += entry(30, 1.0, "tf_connect 127.0.0.1:23000", ["    > [TF] connecting"], ok=ok)
    lines += entry(90, 3.0, "tf_register <arguments-redacted>", ["    > [TF] registration request sent"])
    for index, (cp, view) in enumerate(zip(SCENARIO.checkpoints, views)):
        frame = 500 + 100 * index
        lines += entry(frame, cp, "tf_observe", observation("client", self_id, view) if view is not None else [])
    return "\n".join(lines) + "\n"


def logs(client_views: tuple[list, list] | None = None, server_text: str | None = None, returncodes=(0, 0, 0),
         client_ok: bool = True) -> tuple[multiclient.RoleLog, list[multiclient.RoleLog]]:
    views = client_views or (list(TRUTH), list(TRUTH))
    server = multiclient.RoleLog("server", returncodes[0], SERVER_ANCHOR,
                                 multiclient.parse_audit(server_text if server_text is not None else server_audit()))
    clients = []
    for index, (self_id, faction) in enumerate(((2, "mra"), (1, "auc"))):
        text = client_audit(self_id, views[index], ok=client_ok)
        clients.append(multiclient.RoleLog(f"client{index + 1}", returncodes[index + 1], CLIENT_ANCHOR,
                                           multiclient.parse_audit(text), faction))
    return server, clients


def moved(view: dict, player: int, dx: float) -> dict:
    changed = dict(view)
    faction, cls, health, (x, y, z) = changed[player]
    changed[player] = (faction, cls, health, (x + dx, y, z))
    return changed


class ParserTests(unittest.TestCase):
    def test_interleaved_lines_do_not_break_an_observation(self) -> None:
        lines = observation("client", 2, TRUTH[0])
        lines.insert(3, "    > [TF] player 1 entered world as character 7 (Aurum Combine)")
        lines.insert(1, "[Net] unrelated log line")
        parsed = multiclient.parse_observations(lines)
        self.assertEqual(len(parsed), 1)
        self.assertEqual(sorted(parsed[0].pawns), [1, 2])
        self.assertEqual(parsed[0].pawns[1].pos, (344.0, 24.0, 3808.0))

    def test_truncated_observation_is_not_complete(self) -> None:
        lines = observation("client", 2, TRUTH[0])
        del lines[3]  # one pawn line lost
        self.assertEqual(multiclient.parse_observations(lines), [])

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
        summary = self.evaluate(server_text=server_audit(until=25.0))
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


class ScriptTests(unittest.TestCase):
    def test_client_script_uses_fresh_credentials_and_schedules_every_checkpoint(self) -> None:
        first = multiclient.client_script(SCENARIO, 23000, "mra")
        second = multiclient.client_script(SCENARIO, 23000, "mra")
        self.assertNotEqual(first, second)
        self.assertEqual(first.count("tf_observe"), len(SCENARIO.checkpoints))
        self.assertIn("tf_connect 127.0.0.1:23000", first)
        self.assertTrue(first.startswith("0 tf_status\n"))

    def test_server_script_observes_through_the_run(self) -> None:
        script = multiclient.server_script(23000, 20.0).splitlines()
        self.assertEqual(script[:2], ["0 tf_status", "t0.5 tf_dedicated 23000"])
        self.assertEqual(script[-1], "t19.0 tf_observe")


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


if __name__ == "__main__":
    unittest.main()
