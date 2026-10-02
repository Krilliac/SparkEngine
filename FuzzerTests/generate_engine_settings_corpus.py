#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the engine settings reader.

Each seed is a settings.ini as EngineSettings::Load reads it at startup. The fuzz adapter
(FuzzEngineSettingsProduction.cpp) writes the bytes before the first NUL as settings.ini
and, when there is a NUL, the bytes after it as the settings.local.ini override.
"""

from __future__ import annotations

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "engine-settings"

TYPICAL = b"""; SparkEngine settings
[Graphics]
WindowWidth = 1920
WindowHeight = 1080
Fullscreen = false
VSync = true
RenderScale = 1.0
MaxFrameLatency = 2

[Audio]
MasterVolume = 0.8
MuteAll = off

[Game]
FieldOfView = 90
Language = en-US

[Logging]
GlobalLevel = Info
CategoryMask = 0xFFFF

[Debug]
SuppressFatalAsserts = true
BreakOnSuppressedAsserts = no
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    seeds = {
        "valid-typical.ini": TYPICAL,
        "valid-bom-crlf.ini": b"\xef\xbb\xbf[Graphics]\r\nWindowWidth=1280\r\n# comment\r\n[Camera]\r\nNearPlane=0.05\r\n",
        "valid-with-local-override.ini": TYPICAL + b"\x00[Graphics]\nWindowWidth = 2560\n[Online]\nToken = local-only\n",
        "retired-crash-keys.ini": b"[CrashReporting]\nEnabled = true\nGitHubToken = ghp_example\nSmtpPass = hunter2\n"
        b"UploadURL = https://example.invalid/upload\n\x00[CrashReporting]\nGithubToken = local-secret\n",
        "numeric-edge-values.ini": b"[Graphics]\nWindowWidth = 99999999999999999999\nRenderScale = nan\n"
        b"MaxFrameLatency = 12abc\nRefreshRate = -0x10\n[Physics]\nFixedTimestep = 1e-46\nGravityY = inf\n"
        b"[Logging]\nCategoryMask = -1\n",
        "unknown-sections-and-keys.ini": b"[NotASection]\nAnything = goes\n[Graphics]\nNotAKey = 1\nWindowWidth =\n",
        "malformed-empty-key.ini": b"[Graphics]\n = 1\n",
        "malformed-section.ini": b"[Graphics\nWindowWidth = 800\n",
        "malformed-line.ini": b"[Audio]\nMasterVolume 0.5\n",
        "malformed-local-override.ini": TYPICAL + b"\x00[]\nbroken\n",
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(seeds.items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
