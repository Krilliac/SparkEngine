#!/usr/bin/env python3
"""Fail when an x86-64 binary executes instructions above the stable-v1 CPU floor.

BLD-100 / owner decision OD-04: the stable-v1 CPU floor is x86-64 with SSE4.2
and POPCNT (x86-64-v2). cmake/SparkCpuFloor.cmake rejects above-floor compiler
flags at configure time; this tool checks the linked result, so an above-floor
instruction from one of the families below that reaches a shipped image
through any route -- a per-source COMPILE_OPTIONS entry, an inline-asm block, a
prebuilt static library, a target("avx2") attribute -- is still caught.
Instructions outside these families (for example XSAVE-family or TSX
instructions) are not classified.

The binary (ELF or PE/COFF) is disassembled with GNU objdump or llvm-objdump and
every instruction is classified. These are violations:

  AVX       any VEX/EVEX-encoded instruction (v-prefixed mnemonic on an
            xmm/ymm/zmm operand, vzeroupper, vldmxcsr, ...), which needs AVX
  AVX2/512  ymm or zmm operands, AVX-512 opmask registers
  FMA       vfmadd*/vfmsub*/vfnmadd*/vfnmsub*
  F16C      vcvtph2ps/vcvtps2ph
  BMI1/2    andn, bextr, blsi, blsmsk, blsr, bzhi, mulx, pdep, pext, rorx,
            sarx, shlx, shrx
  LZCNT     lzcnt (decodes as BSR on older CPUs, so it silently computes a
            different result instead of faulting)
  MOVBE     movbe (x86-64-v3)
  AES-NI    aesenc, aesenclast, aesdec, aesdeclast, aesimc, aeskeygenassist
  PCLMULQDQ pclmulqdq (and the pclmul*qdq aliases GNU objdump prints)
  SHA       sha1*/sha256* (SHA-NI)
  GFNI      gf2p8affineqb, gf2p8affineinvqb, gf2p8mulb
  RDRAND    rdrand
  RDSEED    rdseed
  ADX       adcx, adox

TZCNT is reported but is not a violation: it shares its encoding (F3 0F BC)
with REP BSF, which GCC and Clang emit at the SSE4.2 floor for ctz on inputs
known to be non-zero, and a pre-BMI CPU executes it as BSF with the same
result for those inputs. A build that really enables BMI also emits the BMI1
instructions above, and SparkCpuFloor.cmake rejects -mbmi at configure time.

Functions that are selected only after a CPUID check may be exempted with
--allow-symbol REGEX (matched against the demangled symbol name). Images
without a symbol table (an MSVC PE without its PDB) report addresses only and
cannot be exempted.

Exit status: 0 when no violation remains, 1 when violations remain, 2 on a
usage or tool error (missing file, no disassembler, not an x86-64 image).
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from collections import Counter
from dataclasses import dataclass, field

# VMX/SVM and the segment-verify instructions also start with 'v' but are not
# VEX-encoded; they never take a vector register, so the vector-operand test
# below already excludes them. These VEX instructions take no vector operand.
VEX_WITHOUT_VECTOR_OPERAND = {"vzeroupper", "vzeroall", "vldmxcsr", "vstmxcsr"}

BMI_MNEMONICS = {
    "andn": "BMI1",
    "bextr": "BMI1",
    "blsi": "BMI1",
    "blsmsk": "BMI1",
    "blsr": "BMI1",
    "bzhi": "BMI2",
    "mulx": "BMI2",
    "pdep": "BMI2",
    "pext": "BMI2",
    "rorx": "BMI2",
    "sarx": "BMI2",
    "shlx": "BMI2",
    "shrx": "BMI2",
}

# Legacy-encoded (non-VEX) extensions that are not part of x86-64-v2. Their VEX
# forms (vaesenc, vpclmulqdq, vgf2p8affineqb) are already caught as AVX.
LEGACY_EXTENSION_MNEMONICS = {
    "adcx": "ADX",
    "adox": "ADX",
    "rdrand": "RDRAND",
    "rdseed": "RDSEED",
}

# Mnemonic prefixes of legacy-encoded extension families. GNU objdump prints
# PCLMULQDQ as pclmullqlqdq/pclmulhqhqdq/... depending on its immediate.
LEGACY_EXTENSION_PREFIXES = (
    ("aes", "AES-NI"),
    ("pclmul", "PCLMULQDQ"),
    ("sha1", "SHA"),
    ("sha256", "SHA"),
    ("gf2p8", "GFNI"),
)

SCALAR_MNEMONICS = set(BMI_MNEMONICS) | set(LEGACY_EXTENSION_MNEMONICS) | {"lzcnt", "tzcnt", "movbe"}

# Instruction prefixes both disassemblers print as separate tokens.
PREFIXES = {
    "lock",
    "rep",
    "repe",
    "repz",
    "repne",
    "repnz",
    "notrack",
    "bnd",
    "data16",
    "addr32",
    "cs",
    "ds",
    "es",
    "fs",
    "gs",
    "ss",
    "rex",
    "rex.w",
    "xacquire",
    "xrelease",
}

SYMBOL_RE = re.compile(r"^([0-9a-fA-F]+) <(.*)>:\s*$")
INSN_RE = re.compile(r"^\s*([0-9a-fA-F]+):\s+(.*)$")
VECTOR_REG_RE = re.compile(r"%?\b([xyz])mm\d+\b")
WIDE_REG_RE = re.compile(r"%?\b([yz])mm\d+\b")
OPMASK_RE = re.compile(r"%k[1-7]\b|\{%?k[1-7]\}")
SIZE_SUFFIX_RE = re.compile(r"^([a-z]+?)[bwlq]?$")


@dataclass
class Finding:
    feature: str
    address: str
    symbol: str
    text: str


@dataclass
class ScanResult:
    path: str
    instructions: int = 0
    violations: list[Finding] = field(default_factory=list)
    allowed: Counter = field(default_factory=Counter)
    informational: Counter = field(default_factory=Counter)


def _base_mnemonic(mnemonic: str) -> str:
    """Strip an AT&T operand-size suffix (shlxq -> shlx) for the scalar sets."""
    match = SIZE_SUFFIX_RE.match(mnemonic)
    if match and match.group(1) in SCALAR_MNEMONICS:
        return match.group(1)
    return mnemonic


def classify(mnemonic: str, operands: str) -> str | None:
    """Return the above-floor feature an instruction needs, "TZCNT", or None."""
    mnemonic = mnemonic.lower()
    if mnemonic.startswith("v"):
        if mnemonic in VEX_WITHOUT_VECTOR_OPERAND or VECTOR_REG_RE.search(operands):
            if OPMASK_RE.search(operands) or re.search(r"%?\bzmm\d+", operands):
                return "AVX-512"
            if re.match(r"^vf(n)?m(add|sub)", mnemonic):
                return "FMA"
            if mnemonic in ("vcvtph2ps", "vcvtps2ph"):
                return "F16C"
            if WIDE_REG_RE.search(operands):
                return "AVX/AVX2 (ymm)"
            return "AVX (VEX)"
        return None
    if OPMASK_RE.search(operands) and mnemonic.startswith("k"):
        return "AVX-512"
    base = _base_mnemonic(mnemonic)
    if base in BMI_MNEMONICS:
        return BMI_MNEMONICS[base]
    if base == "lzcnt":
        return "LZCNT"
    if base == "movbe":
        return "MOVBE"
    if base in LEGACY_EXTENSION_MNEMONICS:
        return LEGACY_EXTENSION_MNEMONICS[base]
    for prefix, feature in LEGACY_EXTENSION_PREFIXES:
        if mnemonic.startswith(prefix):
            return feature
    if base == "tzcnt":
        return "TZCNT"
    return None


def split_instruction(text: str) -> tuple[str, str]:
    """Split disassembly text after the address into (mnemonic, operands)."""
    tokens = text.replace("\t", " ").split()
    while tokens and (tokens[0].lower() in PREFIXES or tokens[0].startswith("{")):
        tokens.pop(0)
    if not tokens:
        return "", ""
    # Drop the disassembler's trailing annotation ("# 0x1234 <sym>",
    # "# xmm0 = ..."), which can name registers the instruction does not use.
    operands = " ".join(tokens[1:]).split("#", 1)[0].strip()
    return tokens[0], operands


def find_disassembler(requested: str | None) -> list[str]:
    candidates = [requested] if requested else ["objdump", "llvm-objdump"]
    for candidate in candidates:
        resolved = shutil.which(candidate) if candidate else None
        if resolved:
            return [resolved]
    raise RuntimeError("no disassembler found (tried: " + ", ".join(c for c in candidates if c) + ")")


def image_is_x86_64(tool: list[str], path: str) -> bool:
    proc = subprocess.run(tool + ["-f", path], capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"{os.path.basename(tool[0])} cannot read {path}: {proc.stderr.strip()}")
    header = proc.stdout.lower()
    return "x86-64" in header or "x86_64" in header or "pe-x86-64" in header or "coff-x86-64" in header


def scan(tool: list[str], path: str, allow: list[re.Pattern[str]]) -> ScanResult:
    result = ScanResult(path=path)
    command = tool + ["-d", "--no-show-raw-insn", "-C", path]
    with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="replace") as proc:
        assert proc.stdout is not None
        symbol = "<unknown>"
        symbol_allowed = False
        for line in proc.stdout:
            header = SYMBOL_RE.match(line)
            if header:
                symbol = header.group(2)
                symbol_allowed = any(pattern.search(symbol) for pattern in allow)
                continue
            insn = INSN_RE.match(line)
            if not insn:
                continue
            mnemonic, operands = split_instruction(insn.group(2))
            if not mnemonic or mnemonic.startswith("("):
                continue
            result.instructions += 1
            feature = classify(mnemonic, operands)
            if feature is None:
                continue
            if feature == "TZCNT":
                result.informational[feature] += 1
            elif symbol_allowed:
                result.allowed[feature] += 1
            else:
                result.violations.append(Finding(feature, insn.group(1), symbol, insn.group(2).strip()))
        stderr = proc.stderr.read() if proc.stderr else ""
    if proc.returncode != 0:
        raise RuntimeError(f"disassembly of {path} failed: {stderr.strip()}")
    if result.instructions == 0:
        raise RuntimeError(f"no instructions disassembled from {path}")
    return result


def report(result: ScanResult, max_report: int) -> None:
    counts = Counter(finding.feature for finding in result.violations)
    status = "FAIL" if result.violations else "OK"
    print(f"{status} {result.path}: {result.instructions} instructions, {len(result.violations)} above-floor")
    for feature, count in sorted(counts.items()):
        print(f"  violation {feature}: {count}")
    for feature, count in sorted(result.allowed.items()):
        print(f"  allowed (cpuid-dispatched) {feature}: {count}")
    for feature, count in sorted(result.informational.items()):
        print(f"  informational {feature} (REP BSF encoding, floor-safe): {count}")
    for finding in result.violations[:max_report]:
        print(f"    {finding.address} [{finding.feature}] {finding.symbol}: {finding.text}")
    if len(result.violations) > max_report:
        print(f"    ... {len(result.violations) - max_report} more")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("binaries", nargs="+", help="ELF or PE/COFF images or objects to scan")
    parser.add_argument("--objdump", help="disassembler to use (default: objdump, then llvm-objdump)")
    parser.add_argument(
        "--allow-symbol",
        action="append",
        default=[],
        metavar="REGEX",
        help="exempt functions whose demangled name matches REGEX (CPUID-dispatched code only)",
    )
    parser.add_argument("--max-report", type=int, default=20, help="violations listed per binary")
    args = parser.parse_args(argv)

    try:
        tool = find_disassembler(args.objdump)
        allow = [re.compile(pattern) for pattern in args.allow_symbol]
    except (RuntimeError, re.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    failed = False
    for path in args.binaries:
        try:
            if not os.path.isfile(path) or os.path.getsize(path) == 0:
                raise RuntimeError(f"{path} does not exist or is empty")
            if not image_is_x86_64(tool, path):
                raise RuntimeError(f"{path} is not an x86-64 image; the SSE4.2 floor does not apply")
            result = scan(tool, path, allow)
        except RuntimeError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
        report(result, args.max_report)
        failed = failed or bool(result.violations)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
