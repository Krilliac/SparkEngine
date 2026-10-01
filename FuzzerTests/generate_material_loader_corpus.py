#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the .sparkmat material reader.

Each seed is a whole .sparkmat file as MaterialLoader::LoadMaterial reads it through
Spark::Graphics::ParseSparkMatDefinition, or a damaged variant.
"""

from __future__ import annotations

import argparse
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "material-loader"

FULL = b"""// Brick wall, every key the reader knows
name = Brick_01
blendMode = AlphaTest
metallic = 0.0
roughness = 0.8
normalScale = 1.25
occlusionStrength = 0.9
emissiveFactor = 0.0
alphaCutoff = 0.33
albedoTexture = textures/brick_albedo.dds
normalTexture = textures/brick_normal.dds
metallicTexture = textures/brick_metal.dds
roughnessTexture = textures/brick_rough.dds
emissiveTexture = textures/brick_emissive.dds
occlusionTexture = textures/brick_ao.dds
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    seeds = {
        "valid-full.sparkmat": FULL,
        "valid-minimal.sparkmat": b"name = Plain\n",
        "valid-crlf-comments.sparkmat": b"// header\r\n\r\n  name =  Glass  \r\nblendMode=Transparent\r\n"
        b"unknownKey = ignored\r\nno separator line\r\nroughness = 0.05\r\n",
        "valid-no-final-newline.sparkmat": b"name = Tail\nmetallic = 1",
        "valid-override-and-equals.sparkmat": b"name = First\nname = Second=Name\n"
        b"albedoTexture = a=b.dds\nroughness = 0.25\nroughness = 0.75\n",
        "missing-name.sparkmat": b"// no name\nmetallic = 0.5\nroughness = 0.5\n",
        "unparseable-factors.sparkmat": b"name = Junk\nmetallic = 0.5abc\nroughness = \nnormalScale = 1e999999\n"
        b"alphaCutoff = 0x1p-1\n",
        "regression-nonfinite-factor.sparkmat": b"name = NaNMaterial\nmetallic = nan\nroughness = inf\n"
        b"normalScale = -inf\nemissiveFactor = NAN\n",
    }
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(seeds.items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
