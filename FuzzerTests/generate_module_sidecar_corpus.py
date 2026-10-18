#!/usr/bin/env python3
"""Generate the deterministic seed corpus for the game-module .sparkabi sidecar gate.

Each seed is a whole `<module>.sparkabi` file as cmake/SparkGameModule.cmake writes it, or a
damaged variant. The fuzz adapter (FuzzModuleSidecarProduction.cpp) writes the seed beside a
fixed module image whose SHA-256 is IMAGE_SHA256 below, and replaces the literal token
`@compiler_abi_version@` with its own build's compiler ABI version, the one descriptor field
that differs between the Clang releases that build the target.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "FuzzerTests" / "corpora" / "module-abi-sidecar"

IMAGE = b"SparkFuzzModuleSidecar image: not a loadable module.\n"
IMAGE_SHA256 = hashlib.sha256(IMAGE).hexdigest()
ABI = "@compiler_abi_version@"

# Spark/ModuleABI.h for a 64-bit Clang C++23 build against the platform runtime.
FIELDS = [
    ("struct_size", "64"),
    ("magic", str(0x4B525053)),
    ("format", "1"),
    ("sdk_version", "9"),
    ("runtime_abi_version", "1"),
    ("compiler_family", "2"),
    ("compiler_abi_version", ABI),
    ("cxx_language_level", "202302"),
    ("runtime_library", "0"),
    ("iterator_debug_level", "0"),
    ("pointer_size", "8"),
    ("binary_sha256", IMAGE_SHA256),
]


def sidecar(fields: list[tuple[str, str]], eol: str = "\n") -> bytes:
    return "".join(f"{key}={value}{eol}" for key, value in fields).encode()


def replaced(key: str, value: str) -> list[tuple[str, str]]:
    return [(k, value if k == key else v) for k, v in FIELDS]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    assert IMAGE_SHA256 == "fddf32a6021977d4a4a3d266f98035c2200e203c7d44232b8a2312997ec0ffcc"
    oversize = sidecar(FIELDS) + b"#" * (4097 - len(sidecar(FIELDS)))
    seeds = {
        "valid.sparkabi": sidecar(FIELDS),
        "valid-crlf.sparkabi": sidecar(FIELDS, "\r\n"),
        "valid-reordered-no-final-newline.sparkabi": sidecar(list(reversed(FIELDS))).rstrip(b"\n"),
        "hash-mismatch.sparkabi": sidecar(replaced("binary_sha256", "0" * 64)),
        "sdk-mismatch.sparkabi": sidecar(replaced("sdk_version", "8")),
        "compiler-mismatch.sparkabi": sidecar(replaced("compiler_family", "1")),
        "signed-and-hex.sparkabi": sidecar(replaced("struct_size", "+64")) + b"magic=0x4B525053\n",
        "uint32-overflow.sparkabi": sidecar(replaced("pointer_size", "4294967304")),
        "missing-field.sparkabi": sidecar(FIELDS[:-2] + FIELDS[-1:]),
        "duplicate-field.sparkabi": sidecar(FIELDS[:-1] + [("format", "1")]),
        "extra-field.sparkabi": sidecar(FIELDS + [("vendor", "spark")]),
        "empty-line.sparkabi": sidecar(FIELDS[:6]) + b"\n" + sidecar(FIELDS[6:]),
        "oversized-value.sparkabi": sidecar(replaced("binary_sha256", IMAGE_SHA256 + "0")),
        "oversize-file.sparkabi": oversize,
    }
    assert len(oversize) == 4097
    args.output.mkdir(parents=True, exist_ok=True)
    for name, payload in sorted(seeds.items()):
        (args.output / name).write_bytes(payload)
        print(f"{name}: {len(payload)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
