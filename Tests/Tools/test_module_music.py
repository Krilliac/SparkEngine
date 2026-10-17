#!/usr/bin/env python3
"""Checks for tools/audio/compose_module_music.py and the module music it writes.

The composer must be byte-deterministic (two runs into temporary roots match each
other and the tracked Assets/Audio/<Module>/Music files), every file must be the one
format the engine audio backends decode (mono 16-bit 22050 Hz PCM WAV), looping
tracks must be a whole number of beats long and join without a click, and stingers
must stay short.
"""
from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
import wave
from array import array
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "audio" / "compose_module_music.py"
LOOP_JOIN_THRESHOLD = 1024  # |first - last| sample, about 3% of full scale


def _load_composer():
    spec = importlib.util.spec_from_file_location("compose_module_music", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


COMPOSER = _load_composer()


def _read(path: Path) -> tuple[wave._wave_params, array]:
    with wave.open(str(path), "rb") as handle:
        params = handle.getparams()
        samples = array("h", handle.readframes(params.nframes))
    if sys.byteorder != "little":
        samples.byteswap()
    return params, samples


class ModuleMusicTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        root = Path(cls.temporary.name)
        cls.first = COMPOSER.compose_all(root / "first")
        cls.second = COMPOSER.compose_all(root / "second")
        cls.first_root = root / "first"

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def test_every_registered_track_is_written(self):
        self.assertEqual(len(self.first), len(COMPOSER.LOOPS) + len(COMPOSER.STINGERS))
        for path in self.first:
            self.assertTrue(path.is_file(), path)

    def test_two_runs_are_byte_identical(self):
        for first, second in zip(self.first, self.second):
            self.assertEqual(first.read_bytes(), second.read_bytes(), first.name)

    def test_tracked_assets_match_the_composer(self):
        for path in self.first:
            tracked = REPO_ROOT / "Assets" / path.relative_to(self.first_root)
            self.assertTrue(tracked.is_file(), f"{tracked} is missing; run {SCRIPT.name}")
            self.assertEqual(path.read_bytes(), tracked.read_bytes(), f"{tracked} is stale; run {SCRIPT.name}")

    def test_wav_header_is_mono_16bit_22050(self):
        for path in self.first:
            params, samples = _read(path)
            self.assertEqual((params.nchannels, params.sampwidth, params.framerate), (1, 2, 22050), path.name)
            self.assertEqual(params.comptype, "NONE", path.name)
            self.assertEqual(len(samples), params.nframes, path.name)
            self.assertGreater(max(abs(s) for s in samples), 8000, f"{path.name} is near silent")

    def test_loops_are_whole_beats_and_seamless(self):
        for track in COMPOSER.LOOPS:
            path = COMPOSER.output_path(self.first_root, track.module, track.stem)
            params, samples = _read(path)
            beat = params.framerate * 60.0 / track.bpm
            beats = len(samples) / beat
            self.assertLess(abs(beats - round(beats)), 1.0 / beat, f"{path.name}: {beats} beats")
            self.assertEqual(round(beats) % 4, 0, f"{path.name} is not a whole number of bars")
            self.assertGreaterEqual(len(samples) / params.framerate, 8.0, path.name)
            self.assertLessEqual(len(samples) / params.framerate, 16.0, path.name)
            self.assertLessEqual(abs(samples[0] - samples[-1]), LOOP_JOIN_THRESHOLD, f"{path.name} clicks at the loop")

    def test_stingers_are_short_and_end_silent(self):
        for cue in COMPOSER.STINGERS:
            path = COMPOSER.output_path(self.first_root, cue.module, cue.stem)
            params, samples = _read(path)
            self.assertLess(len(samples) / params.framerate, 4.0, path.name)
            self.assertEqual(samples[0], 0, path.name)
            self.assertEqual(samples[-1], 0, path.name)


if __name__ == "__main__":
    unittest.main()
