#!/usr/bin/env python3
"""
Compose the music tracks the Platformer, Racing, RPG and RTS game modules register.

A deterministic procedural composer (Python standard library only). Every track is
written from a small score description: key and mode, tempo, a chord progression,
and a style that decides the bass line, pad, lead melody/arpeggio and noise-based
percussion. Output is mono 16-bit 22050 Hz WAV, the one format the engine audio
backends decode.

Looping tracks are a whole number of bars (8-16 s). They loop without a click: the
pad voice is rendered circularly, so its sustain and release wrap past the loop point
into the first bar, and every other voice finishes before the loop point. Stingers
(victory, defeat, countdown, ...) are short non-looping cues under 4 s that fade to
silence.

All generated audio is free to use (public domain / CC0): it is procedurally
generated from this script and contains no third-party content. Output bytes are
identical across runs.

Usage:
    python3 tools/audio/compose_module_music.py [--repo .] [--output-root DIR]

``--output-root`` defaults to ``<repo>/Assets``; tracks land in
``<output-root>/Audio/<Module>/Music/<stem>.wav``.
"""
from __future__ import annotations

import argparse
import array
import math
import sys
import wave
from dataclasses import dataclass
from pathlib import Path

SAMPLE_RATE = 22050
PEAK_LEVEL = 0.89
LOOP_GUARD_SECONDS = 0.03  # non-pad voices end this far before the loop point
TRUNCATE_FADE_SECONDS = 0.005

MODES = {
    "ionian": (0, 2, 4, 5, 7, 9, 11),
    "dorian": (0, 2, 3, 5, 7, 9, 10),
    "phrygian": (0, 1, 3, 5, 7, 8, 10),
    "lydian": (0, 2, 4, 6, 7, 9, 11),
    "mixolydian": (0, 2, 4, 5, 7, 9, 10),
    "aeolian": (0, 2, 3, 5, 7, 8, 10),
    "harmonic_minor": (0, 2, 3, 5, 7, 8, 11),
}


@dataclass(frozen=True)
class LoopTrack:
    """A seamless looping track: `bars` bars of 4/4 over a repeating chord progression."""

    module: str
    stem: str
    bpm: float  # matches the bpm the module registers for the track
    root: int  # MIDI note of the tonic (lead register)
    mode: str
    progression: tuple[int, ...]  # chord roots as 0-based scale degrees
    chord_beats: int
    bars: int
    style: str
    seed: int


@dataclass(frozen=True)
class Stinger:
    """A short non-looping cue. Events are (beat, length in beats, scale degree)."""

    module: str
    stem: str
    bpm: float
    root: int
    mode: str
    lead: tuple[tuple[float, float, int], ...]
    lead_wave: str
    chord: tuple[float, float, tuple[int, ...]]  # (beat, length, degrees) held by the pad
    bass: tuple[tuple[float, float, int], ...]
    hits: tuple[tuple[float, str], ...]  # (beat, drum)
    tail_seconds: float
    seed: int


# ---------------------------------------------------------------------------
# Score: every track the four modules register
# ---------------------------------------------------------------------------

LOOPS = (
    LoopTrack("Platformer", "world_1_theme", 120.0, 72, "ionian", (0, 4, 5, 3), 4, 8, "chip", 11),
    LoopTrack("Platformer", "world_2_theme", 120.0, 67, "mixolydian", (0, 6, 3, 0, 0, 6, 4, 4), 4, 8, "chip", 12),
    LoopTrack("Platformer", "boss_theme", 160.0, 62, "harmonic_minor", (0, 0, 5, 4), 4, 8, "boss_chip", 13),
    LoopTrack("Racing", "racing_menu", 110.0, 64, "aeolian", (0, 5, 2, 6), 4, 4, "cruise", 21),
    LoopTrack("Racing", "race_track_01", 150.0, 69, "aeolian", (0, 5, 3, 4), 4, 8, "drive", 22),
    LoopTrack("Racing", "race_track_02", 145.0, 62, "dorian", (0, 3, 6, 4), 4, 8, "drive", 23),
    LoopTrack("Racing", "final_lap", 165.0, 64, "phrygian", (0, 1, 0, 6), 4, 8, "drive", 24),
    LoopTrack("RPG", "rpg_village", 95.0, 65, "ionian", (0, 3, 4, 0), 4, 4, "pastoral", 31),
    LoopTrack("RPG", "rpg_forest", 85.0, 64, "dorian", (0, 3, 0, 6), 4, 4, "pastoral", 32),
    LoopTrack("RPG", "rpg_dungeon", 70.0, 61, "phrygian", (0, 1, 0, 5), 4, 4, "dark", 33),
    LoopTrack("RPG", "rpg_boss", 140.0, 62, "harmonic_minor", (0, 5, 3, 4), 4, 8, "epic", 34),
    LoopTrack("RTS", "faction_1_theme", 120.0, 62, "dorian", (0, 3, 6, 0), 4, 8, "martial", 41),
    LoopTrack("RTS", "faction_2_theme", 120.0, 64, "phrygian", (0, 1, 5, 1), 4, 8, "martial_dark", 42),
    LoopTrack("RTS", "faction_3_theme", 120.0, 66, "lydian", (0, 1, 4, 0), 4, 8, "martial_bright", 43),
    LoopTrack("RTS", "battle_music", 140.0, 60, "harmonic_minor", (0, 5, 6, 4), 4, 8, "epic", 44),
)

STINGERS = (
    Stinger("Platformer", "victory_jingle", 120.0, 72, "ionian",
            ((0, 0.5, 0), (0.5, 0.5, 2), (1, 0.5, 4), (1.5, 0.5, 7), (2, 0.5, 6), (2.5, 0.5, 7), (3, 2.5, 9)),
            "square", (3, 2.5, (0, 2, 4)), ((0, 1, -7), (1, 1, -3), (3, 2.5, -7)),
            ((0, "kick"), (1, "snare"), (2, "kick"), (3, "crash")), 0.45, 51),
    Stinger("Platformer", "game_over", 120.0, 67, "aeolian",
            ((0, 1, 4), (1, 1, 3), (2, 1, 2), (3, 3, 0)),
            "square", (3, 3, (-7, -5, -3)), ((0, 2, -7), (2, 1, -9), (3, 3, -14)),
            ((3, "kick"),), 0.4, 52),
    Stinger("Racing", "countdown", 120.0, 69, "ionian",
            ((0, 0.4, 0), (1, 0.4, 0), (2, 0.4, 0), (3, 1.5, 7)),
            "square", (3, 1.5, (0, 4, 7)), ((3, 1.5, -7),),
            ((0, "hat"), (1, "hat"), (2, "hat"), (3, "crash")), 0.5, 53),
    Stinger("Racing", "victory", 130.0, 69, "ionian",
            ((0, 0.5, 0), (0.5, 0.5, 4), (1, 0.5, 7), (1.5, 0.5, 9), (2, 0.75, 8), (2.75, 0.25, 9), (3, 3, 11)),
            "saw", (3, 3, (0, 2, 4)), ((0, 1, -7), (1, 1, -5), (2, 1, -4), (3, 3, -7)),
            ((0, "kick"), (1, "kick"), (2, "kick"), (2.5, "snare"), (3, "crash")), 0.35, 54),
    Stinger("Racing", "defeat", 80.0, 64, "aeolian",
            ((0, 1, 4), (1, 1, 3), (2, 2, 1)),
            "saw", (2, 2, (-7, -5, -2)), ((0, 2, -7), (2, 2, -8)),
            ((0, "kick"), (2, "kick")), 0.4, 55),
    Stinger("RPG", "rpg_victory", 120.0, 67, "ionian",
            ((0, 0.25, 4), (0.25, 0.25, 4), (0.5, 0.25, 4), (1, 1, 4), (2, 1, 2), (3, 0.5, 3), (3.5, 2.5, 4)),
            "brass", (3.5, 2.5, (0, 2, 4)), ((0, 2, -7), (2, 1, -10), (3, 0.5, -9), (3.5, 2.5, -7)),
            ((0, "timpani"), (2, "timpani"), (3.5, "timpani"), (3.5, "crash")), 0.5, 56),
    Stinger("RTS", "victory", 120.0, 62, "dorian",
            ((0, 0.75, 0), (0.75, 0.25, 0), (1, 1, 4), (2, 0.5, 3), (2.5, 0.5, 4), (3, 3, 7)),
            "brass", (3, 3, (0, 2, 4)), ((0, 1, -7), (1, 1, -3), (2, 1, -4), (3, 3, -7)),
            ((0, "snare_roll"), (1, "war_drum"), (2, "war_drum"), (3, "war_drum"), (3, "crash")), 0.4, 57),
    Stinger("RTS", "defeat", 120.0, 62, "aeolian",
            ((0, 1.5, 4), (1.5, 0.5, 3), (2, 1, 1), (3, 3, 0)),
            "brass", (3, 3, (-7, -5, -2)), ((0, 2, -7), (2, 1, -8), (3, 3, -14)),
            ((0, "war_drum"), (3, "war_drum")), 0.45, 58),
)

# Per style: lead rhythm over two bars as (onset beat, length), voice timbres, bass
# pattern, arpeggio and drum steps (16 per bar; any non-'.' character is a hit).
STYLES = {
    "chip": {
        "rhythm": ((0, 0.5), (0.5, 0.5), (1, 1), (2, 0.5), (2.5, 0.5), (3, 0.75),
                   (4, 0.5), (4.5, 0.5), (5, 0.5), (5.5, 0.5), (6, 1.5)),
        "lead": "square", "lead_amp": 0.26, "bass": "triangle", "bass_steps": (0, 4), "bass_len": 0.5,
        "arp": ("pulse25", (0, 2, 4, 7), 0.25, 0.07),
        "drums": {"chip_kick": "x.......x.......", "chip_snare": "....x.......x...", "chip_hat": "x.x.x.x.x.x.x.x."},
        "cutoff": 7000.0,
    },
    "boss_chip": {
        "rhythm": ((0, 0.75), (0.75, 0.75), (1.5, 0.5), (2, 0.5), (2.5, 0.5), (3, 1),
                   (4, 0.75), (4.75, 0.75), (5.5, 0.5), (6, 0.5), (6.5, 0.5), (7, 0.75)),
        "lead": "square", "lead_amp": 0.25, "bass": "triangle", "bass_steps": (0, 7), "bass_len": 0.5,
        "arp": ("pulse25", (0, 2, 4, 2), 0.25, 0.06),
        "drums": {"chip_kick": "x...x...x...x...", "chip_snare": "....x.......x..x", "chip_hat": "xxxxxxxxxxxxxxxx"},
        "cutoff": 7000.0,
    },
    "cruise": {
        "rhythm": ((0, 1.5), (1.5, 1), (2.5, 1.5), (4, 0.5), (4.5, 0.5), (5, 1), (6, 1.75)),
        "lead": "saw", "lead_amp": 0.17, "bass": "saw", "bass_steps": (0, 0, 7, 0), "bass_len": 0.5,
        "arp": ("triangle", (0, 4, 7, 4), 0.25, 0.08),
        "drums": {"kick": "x.......x.......", "snare": "....x.......x...", "hat": "x.x.x.x.x.x.x.x."},
        "cutoff": 5000.0,
    },
    "drive": {
        "rhythm": ((0, 0.75), (0.75, 0.75), (1.5, 0.5), (2, 1), (3, 0.5), (3.5, 0.5),
                   (4, 0.75), (4.75, 0.75), (5.5, 0.5), (6, 1.75)),
        "lead": "square", "lead_amp": 0.18, "bass": "saw", "bass_steps": (0, 0, 7, 0), "bass_len": 0.25,
        "arp": ("saw", (0, 4, 7, 11), 0.25, 0.06),
        "drums": {"kick": "x...x...x...x...", "snare": "....x.......x...", "hat": "x.x.x.x.x.x.x.x.",
                  "open_hat": "..x...x...x...x."},
        "cutoff": 6000.0,
    },
    "pastoral": {
        "rhythm": ((0, 1.5), (1.5, 0.5), (2, 2), (4, 1), (5, 1), (6, 1.75)),
        "lead": "flute", "lead_amp": 0.22, "bass": "sine", "bass_steps": (0, 4), "bass_len": 2.0,
        "arp": ("pluck", (0, 4, 7, 9, 7, 4), 0.5, 0.12),
        "drums": {"shaker": "..x...x...x...x."},
        "cutoff": 4500.0,
    },
    "dark": {
        "rhythm": ((0, 1), (2, 1.5), (4, 0.5), (4.5, 0.5), (5, 2.5)),
        "lead": "bell", "lead_amp": 0.2, "bass": "sine", "bass_steps": (0,), "bass_len": 4.0,
        "arp": None,
        "drums": {"timpani": "x.......x.....x."},
        "cutoff": 3000.0,
    },
    "epic": {
        "rhythm": ((0, 1.5), (1.5, 0.5), (2, 1), (3, 1), (4, 2), (6, 0.5), (6.5, 0.5), (7, 0.75)),
        "lead": "brass", "lead_amp": 0.2, "bass": "saw", "bass_steps": (0, 0, 4, 0, 0, 0, 7, 4), "bass_len": 0.5,
        "arp": ("saw", (0, 2, 4, 2), 0.5, 0.05),
        "drums": {"timpani": "x..x..x.x..x..x.", "snare": "....x.......x...", "hat": "..x...x...x...x."},
        "cutoff": 4500.0,
    },
    "martial": {
        "rhythm": ((0, 0.75), (0.75, 0.25), (1, 1), (2, 0.75), (2.75, 0.25), (3, 1), (4, 2), (6, 1.75)),
        "lead": "brass", "lead_amp": 0.2, "bass": "triangle", "bass_steps": (0, 0, 4, 0), "bass_len": 1.0,
        "arp": None,
        "drums": {"war_drum": "x.......x.......", "snare": "x..xx.x.x.x.xxxx"},
        "cutoff": 4500.0,
    },
    "martial_dark": {
        "rhythm": ((0, 1.5), (1.5, 0.5), (2, 2), (4, 0.5), (4.5, 0.5), (5, 1), (6, 1.75)),
        "lead": "brass", "lead_amp": 0.19, "bass": "saw", "bass_steps": (0, 1, 0, 0), "bass_len": 1.0,
        "arp": None,
        "drums": {"war_drum": "x.....x...x.....", "snare": "....x..x....x.xx", "timpani": "x...............",
                  },
        "cutoff": 3500.0,
    },
    "martial_bright": {
        "rhythm": ((0, 0.5), (0.5, 0.5), (1, 1), (2, 0.5), (2.5, 0.5), (3, 1), (4, 1), (5, 1), (6, 1.75)),
        "lead": "brass", "lead_amp": 0.2, "bass": "triangle", "bass_steps": (0, 4, 0, 4), "bass_len": 1.0,
        "arp": ("pluck", (0, 4, 7, 4), 0.5, 0.07),
        "drums": {"war_drum": "x.......x...x...", "snare": "x.xxx.x.x.xxx.x.", "hat": "..x...x...x...x."},
        "cutoff": 5500.0,
    },
}


# ---------------------------------------------------------------------------
# Synthesis
# ---------------------------------------------------------------------------

class Noise:
    """Deterministic LCG noise in [-1, 1) (independent of the `random` module)."""

    def __init__(self, seed: int) -> None:
        self.state = (seed * 2654435761 + 1) & 0x7FFFFFFF

    def next(self) -> float:
        self.state = (self.state * 1103515245 + 12345) & 0x7FFFFFFF
        return self.state / 1073741824.0 - 1.0


def midi_to_hz(note: float) -> float:
    return 440.0 * 2.0 ** ((note - 69.0) / 12.0)


def degree_to_midi(root: int, mode: str, degree: int) -> int:
    scale = MODES[mode]
    return root + 12 * (degree // 7) + scale[degree % 7]


def oscillator(wave_name: str, phase: float) -> float:
    """One sample of a band-unlimited oscillator at `phase` in cycles."""
    p = phase - math.floor(phase)
    if wave_name == "sine":
        return math.sin(2.0 * math.pi * p)
    if wave_name == "triangle":
        return 4.0 * p - 1.0 if p < 0.5 else 3.0 - 4.0 * p
    if wave_name == "saw":
        return 2.0 * p - 1.0
    if wave_name == "square":
        return 1.0 if p < 0.5 else -1.0
    if wave_name == "pulse25":
        return (1.0 if p < 0.25 else -1.0) + 0.5  # DC removed
    raise ValueError(f"unknown waveform {wave_name}")


def tone(wave_name: str, freq: float, seconds: float, amp: float, release: float = 0.05) -> list[float]:
    """Render one note of a named voice, including its release tail."""
    held = max(1, int(seconds * SAMPLE_RATE))
    tail = int(release * SAMPLE_RATE)
    attack = int(0.005 * SAMPLE_RATE)
    out = []
    inc = freq / SAMPLE_RATE
    for i in range(held + tail):
        t = i / SAMPLE_RATE
        env = min(1.0, i / attack) if attack else 1.0
        if i >= held:
            env *= 1.0 - (i - held) / tail
        phase = i * inc
        if wave_name == "flute":
            vib = 0.004 * math.sin(2.0 * math.pi * 5.0 * t) * min(1.0, t / 0.3)
            env *= min(1.0, i / (0.06 * SAMPLE_RATE))
            s = math.sin(2.0 * math.pi * phase * (1.0 + vib)) + 0.15 * math.sin(4.0 * math.pi * phase)
        elif wave_name == "brass":
            env *= min(1.0, i / (0.03 * SAMPLE_RATE)) * (0.8 + 0.2 * math.exp(-t / 0.15))
            s = 0.6 * oscillator("saw", phase) + 0.4 * oscillator("triangle", phase)
        elif wave_name == "bell":
            env *= math.exp(-t / 0.9)
            s = math.sin(2.0 * math.pi * phase) + 0.35 * math.sin(2.0 * math.pi * 2.76 * phase) * math.exp(-t / 0.3)
        elif wave_name == "pluck":
            env *= math.exp(-t / 0.25)
            s = oscillator("triangle", phase)
        elif wave_name == "pad":
            env *= min(1.0, i / (0.12 * SAMPLE_RATE))
            s = 0.7 * math.sin(2.0 * math.pi * phase) + 0.3 * oscillator("triangle", phase * 1.003)
        else:
            s = oscillator(wave_name, phase)
        out.append(amp * env * s)
    return out


def drum(kind: str, noise: Noise, amp: float = 1.0) -> list[float]:
    """Noise-and-sine percussion hits."""
    if kind == "kick" or kind == "chip_kick":
        length, decay, f0, f1, body = 0.3, 0.12, 140.0, 45.0, 0.9
    elif kind == "timpani" or kind == "war_drum":
        length, decay, f0, f1, body = 0.8, 0.3 if kind == "timpani" else 0.18, 90.0, 55.0, 0.8
    else:
        length, decay, f0, f1, body = 0.0, 0.0, 0.0, 0.0, 0.0
    out = []
    if body:
        phase = 0.0
        smack = 0.08 if kind == "war_drum" else 0.02
        for i in range(int(length * SAMPLE_RATE)):
            t = i / SAMPLE_RATE
            phase += (f1 + (f0 - f1) * math.exp(-t / 0.04)) / SAMPLE_RATE
            env = min(1.0, i / 44.0) * math.exp(-t / decay)
            out.append(amp * body * env * (math.sin(2.0 * math.pi * phase) + smack * noise.next()))
        return out
    settings = {  # length, noise decay, tone Hz, tone level, high-pass amount, level
        "snare": (0.22, 0.07, 185.0, 0.4, 0.0, 0.55),
        "chip_snare": (0.15, 0.05, 220.0, 0.2, 0.0, 0.45),
        "snare_roll": (1.0, 0.8, 0.0, 0.0, 0.0, 0.35),
        "hat": (0.05, 0.012, 0.0, 0.0, 1.0, 0.22),
        "chip_hat": (0.04, 0.01, 0.0, 0.0, 1.0, 0.18),
        "open_hat": (0.2, 0.06, 0.0, 0.0, 1.0, 0.16),
        "shaker": (0.08, 0.025, 0.0, 0.0, 1.0, 0.12),
        "crash": (1.2, 0.45, 0.0, 0.0, 1.0, 0.3),
    }
    length, decay, tone_hz, tone_level, highpass, level = settings[kind]
    previous = 0.0
    for i in range(int(length * SAMPLE_RATE)):
        t = i / SAMPLE_RATE
        n = noise.next()
        s = n - previous if highpass else n
        previous = n
        if kind == "snare_roll":  # a swelling roll of 32nd-note strokes
            stroke = (i % int(0.0625 * SAMPLE_RATE)) / SAMPLE_RATE
            env = (0.3 + 0.7 * t / length) * math.exp(-stroke / 0.02) * (1.0 - t / length) ** 0.3
        else:
            env = math.exp(-t / decay)
        env *= min(1.0, i / 22.0)
        if tone_hz:
            s += tone_level / level * math.sin(2.0 * math.pi * tone_hz * t) * math.exp(-t / 0.05)
        out.append(amp * level * env * s)
    return out


class Mix:
    """A mono mix buffer. Wrapped placement renders circularly (loop voices)."""

    def __init__(self, length: int, loop: bool) -> None:
        self.buffer = [0.0] * length
        self.loop = loop
        self.limit = length - int(LOOP_GUARD_SECONDS * SAMPLE_RATE) if loop else length

    def place(self, start: int, samples: list[float], wrap: bool = False) -> None:
        length = len(self.buffer)
        if wrap and self.loop:
            for i, s in enumerate(samples):
                self.buffer[(start + i) % length] += s
            return
        end = min(len(samples), self.limit - start)
        if end <= 0:
            return
        fade = int(TRUNCATE_FADE_SECONDS * SAMPLE_RATE) if end < len(samples) else 0
        for i in range(end):
            gain = min(1.0, (end - i) / fade) if fade else 1.0
            self.buffer[start + i] += samples[i] * gain


def lowpass(signal: list[float], cutoff: float, circular: bool) -> list[float]:
    """Two cascaded one-pole low-pass filters; a circular pass primes the loop state."""
    a = 1.0 - math.exp(-2.0 * math.pi * cutoff / SAMPLE_RATE)
    y1 = y2 = 0.0
    if circular:
        for s in signal:
            y1 += a * (s - y1)
            y2 += a * (y1 - y2)
    out = []
    for s in signal:
        y1 += a * (s - y1)
        y2 += a * (y1 - y2)
        out.append(y2)
    return out


def master(signal: list[float], loop: bool) -> list[int]:
    """Remove DC, soft-limit, normalise and quantise to 16-bit."""
    if loop:
        mean = sum(signal) / len(signal)
        signal = [s - mean for s in signal]
    drive = 1.4
    signal = [math.tanh(s * drive) for s in signal]
    peak = max(abs(s) for s in signal) or 1.0
    gain = PEAK_LEVEL / peak
    if not loop:  # stingers fade to exact silence
        fade = int(0.04 * SAMPLE_RATE)
        for i in range(fade):
            signal[-1 - i] *= i / fade
    return [max(-32767, min(32767, int(round(s * gain * 32767.0)))) for s in signal]


# ---------------------------------------------------------------------------
# Arrangement
# ---------------------------------------------------------------------------

def make_motif(noise: Noise, count: int) -> list[int]:
    """Scale-degree offsets from the chord root: chord tones on the ends, steps between."""
    motif = [(0, 2, 4)[int((noise.next() + 1.0) * 1.5) % 3]]
    for _ in range(count - 1):
        step = (-2, -1, 1, 2, 2, -1)[int((noise.next() + 1.0) * 3.0) % 6]
        motif.append(max(-3, min(9, motif[-1] + step)))
    motif[-1] = (0, 2, 4)[min(range(3), key=lambda k: abs((0, 2, 4)[k] - motif[-1]))]
    return motif


def compose_loop(track: LoopTrack) -> list[int]:
    style = STYLES[track.style]
    beat = SAMPLE_RATE * 60.0 / track.bpm
    total_beats = track.bars * 4
    mix = Mix(int(round(total_beats * beat)), loop=True)
    noise = Noise(track.seed)

    def at(b: float) -> int:
        return int(round(b * beat))

    def chord_root(b: float) -> int:
        return track.progression[int(b // track.chord_beats) % len(track.progression)]

    def note(degree: int, octave: int) -> float:
        return midi_to_hz(degree_to_midi(track.root, track.mode, degree) + 12 * octave)

    # Pad: the chord triad, one octave below the lead, wrapped across the loop point.
    for start in range(0, total_beats, track.chord_beats):
        root = chord_root(start)
        seconds = track.chord_beats * beat / SAMPLE_RATE
        for offset in (0, 2, 4):
            mix.place(at(start), tone("pad", note(root + offset, -1), seconds, 0.09, release=0.25), wrap=True)

    # Bass: the style's step pattern of chord-relative degrees, two octaves down.
    steps = style["bass_steps"]
    position, index = 0.0, 0
    while position < total_beats:
        degree = chord_root(position) + steps[index % len(steps)]
        seconds = style["bass_len"] * beat / SAMPLE_RATE * 0.9
        mix.place(at(position), tone(style["bass"], note(degree, -2), seconds, 0.22, release=0.03))
        position += style["bass_len"]
        index += 1

    # Arpeggio: chord tones cycling at a fixed subdivision.
    if style["arp"]:
        wave_name, pattern, subdivision, amp = style["arp"]
        position, index = 0.0, 0
        while position < total_beats:
            degree = chord_root(position) + pattern[index % len(pattern)]
            seconds = subdivision * beat / SAMPLE_RATE * 0.8
            mix.place(at(position), tone(wave_name, note(degree, 0), seconds, amp, release=0.03))
            position += subdivision
            index += 1

    # Lead: two-bar motifs A A B A (or A B for four-bar loops), transposed onto each chord.
    rhythm = style["rhythm"]
    motif_a = make_motif(noise, len(rhythm))
    motif_b = make_motif(noise, len(rhythm))
    pairs = track.bars // 2
    order = [motif_a, motif_b] if pairs == 2 else [motif_a if p != pairs - 2 else motif_b for p in range(pairs)]
    for pair, motif in enumerate(order):
        for (onset, length), offset in zip(rhythm, motif):
            b = pair * 8 + onset
            seconds = length * beat / SAMPLE_RATE * 0.92
            mix.place(at(b), tone(style["lead"], note(chord_root(b) + offset, 0), seconds, style["lead_amp"]))

    # Percussion: 16 steps per bar.
    for bar in range(track.bars):
        for kind, pattern in style["drums"].items():
            for step, mark in enumerate(pattern):
                if mark != ".":
                    mix.place(at(bar * 4 + step * 0.25), drum(kind, noise))

    return master(lowpass(mix.buffer, style["cutoff"], circular=True), loop=True)


def compose_stinger(cue: Stinger) -> list[int]:
    beat = SAMPLE_RATE * 60.0 / cue.bpm
    events = [b + d for b, d, _ in cue.lead] + [cue.chord[0] + cue.chord[1]]
    length = int(round(max(events) * beat)) + int(cue.tail_seconds * SAMPLE_RATE)
    mix = Mix(length, loop=False)
    noise = Noise(cue.seed)

    def note(degree: int, octave: int) -> float:
        return midi_to_hz(degree_to_midi(cue.root, cue.mode, degree) + 12 * octave)

    for b, d, degree in cue.lead:
        mix.place(int(b * beat), tone(cue.lead_wave, note(degree, 0), d * beat / SAMPLE_RATE * 0.92, 0.25, 0.2))
    start, held, degrees = cue.chord
    for degree in degrees:
        mix.place(int(start * beat), tone("pad", note(degree, 0), held * beat / SAMPLE_RATE, 0.1, release=0.3))
    for b, d, degree in cue.bass:
        mix.place(int(b * beat), tone("triangle", note(degree, -1), d * beat / SAMPLE_RATE * 0.9, 0.22, 0.1))
    for b, kind in cue.hits:
        mix.place(int(b * beat), drum(kind, noise))
    return master(lowpass(mix.buffer, 6000.0, circular=False), loop=False)


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

def output_path(output_root: Path, module: str, stem: str) -> Path:
    return output_root / "Audio" / module / "Music" / f"{stem}.wav"


def write_wav(path: Path, samples: list[int]) -> None:
    data = array.array("h", samples)
    if sys.byteorder != "little":
        data.byteswap()
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(SAMPLE_RATE)
        handle.writeframes(data.tobytes())


def compose_all(output_root: Path) -> list[Path]:
    written = []
    for track in LOOPS:
        path = output_path(output_root, track.module, track.stem)
        write_wav(path, compose_loop(track))
        written.append(path)
    for cue in STINGERS:
        path = output_path(output_root, cue.module, cue.stem)
        write_wav(path, compose_stinger(cue))
        written.append(path)
    return written


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--output-root", type=Path, default=None, help="defaults to <repo>/Assets")
    args = parser.parse_args(argv)
    output_root = args.output_root or args.repo / "Assets"
    for path in compose_all(output_root):
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
