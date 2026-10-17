#!/usr/bin/env python3
"""Generate the small deterministic seed corpus for the production SoundEffect WAV parser.

The layout is RIFF/WAVE: the 12-byte "RIFF" <size> "WAVE" header, then chunks of
a 4-byte id, a u32 little-endian size and the body, padded to an even length.
SoundEffect::LoadFromMemory (SparkEngine/Source/Audio/SoundEffect.cpp) locates
'fmt ' and 'data' by walking those chunks from offset 12 and accepts only PCM
and IEEE-float fmt chunks with consistent block-align and byte-rate fields.

The committed bytes are pinned by tools/fuzz-policy/corpus-manifest.json; this
script records how each seed was built so a reviewer can regenerate and diff.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "audio-sound-effect"

WAVE_FORMAT_PCM = 1
WAVE_FORMAT_IEEE_FLOAT = 3
WAVE_FORMAT_EXTENSIBLE = 0xFFFE


def chunk(tag: bytes, body: bytes, *, declared: int | None = None) -> bytes:
    size = len(body) if declared is None else declared
    return tag + struct.pack("<I", size) + body + (b"\x00" if len(body) % 2 else b"")


def fmt_body(tag: int, channels: int, rate: int, bits: int, extension: bytes | None = None) -> bytes:
    block_align = channels * bits // 8
    body = struct.pack("<HHIIHH", tag, channels, rate, rate * block_align, block_align, bits)
    if extension is not None:
        body += struct.pack("<H", len(extension)) + extension
    return body


def riff(*chunks: bytes) -> bytes:
    body = b"WAVE" + b"".join(chunks)
    return b"RIFF" + struct.pack("<I", len(body)) + body


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    pcm16_mono = fmt_body(WAVE_FORMAT_PCM, 1, 22050, 16)
    samples16 = struct.pack("<8h", 0, 1000, 2000, 1000, 0, -1000, -2000, -1000)
    float_stereo = fmt_body(WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 32)
    samples_float = struct.pack("<8f", 0.0, 0.0, 0.5, -0.5, 1.0, -1.0, 0.25, -0.25)

    # WAVE_FORMAT_EXTENSIBLE: cbSize 22, valid bits, channel mask, PCM SubFormat GUID.
    subformat_pcm = bytes.fromhex("0100000000001000800000aa00389b71")
    extensible = fmt_body(WAVE_FORMAT_EXTENSIBLE, 1, 22050, 16, struct.pack("<HI", 16, 4) + subformat_pcm)
    # PCM with an 18-byte fmt chunk whose cbSize claims 6 extension bytes it does not carry.
    cbsize_nonzero = pcm16_mono + struct.pack("<H", 6)

    seeds = {
        "valid-pcm16-mono.wav": riff(chunk(b"fmt ", pcm16_mono), chunk(b"data", samples16)),
        "valid-float32-stereo.wav": riff(chunk(b"fmt ", float_stereo), chunk(b"data", samples_float)),
        "extensible-fmt.wav": riff(chunk(b"fmt ", extensible), chunk(b"data", samples16)),
        "fmt-cbsize-nonzero.wav": riff(chunk(b"fmt ", cbsize_nonzero), chunk(b"data", samples16)),
        # The data chunk declares 1000 bytes (whole 2-byte frames) but carries 16.
        "data-size-overclaim.wav": riff(chunk(b"fmt ", pcm16_mono), chunk(b"data", samples16, declared=1000)),
        # An odd-sized LIST chunk and its pad byte precede 'fmt '.
        "odd-chunk-padding.wav": riff(
            chunk(b"LIST", b"INFOISFT\x03\x00\x00\x00ab\x00"), chunk(b"fmt ", pcm16_mono), chunk(b"data", samples16)
        ),
        # A leading chunk whose size field is 0xFFFFFFFF.
        "chunk-size-0xFFFFFFFF.wav": riff(
            chunk(b"junk", b"\x00" * 4, declared=0xFFFFFFFF), chunk(b"fmt ", pcm16_mono), chunk(b"data", samples16)
        ),
        "no-data-chunk.wav": riff(chunk(b"fmt ", pcm16_mono), chunk(b"LIST", samples16)),
    }

    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in seeds.items():
        (args.output / name).write_bytes(payload)
    total = sum(len(payload) for payload in seeds.values())
    print(f"generated {len(seeds)} WAV seeds ({total} bytes) in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
