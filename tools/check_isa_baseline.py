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

ELF functions selected only after a CPUID check may be exempted with
--allow-symbol REGEX. PE images require --pdb PDB (or repeated
--pdb IMAGE=PDB for multiple images) and llvm-pdbutil. Their RSDS GUID/age
must match. Only exact reviewed MSVC runtime procedure/module pairs receive
AVX/AVX2 exemptions inside their PDB byte ranges; regex exemptions never
apply to PE images. Other runtime versions/functions fail closed for review.

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
import struct
import importlib.util
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


@dataclass(frozen=True)
class PdbRange:
    start: int
    end: int
    symbol: str
    module: str


# Reviewed against MSVC 14.44.35207 crt/src/stl/vector_algorithms.cpp:
# _Use_avx2() (line 26) reads __isa_enabled & (1 << __ISA_AVAILABLE_AVX2).
# Swap/reverse entries guard directly (95-460); find/count/mismatch wrappers
# call the guarded implementations (2568, 2640, 2840, 4020); replace/remove
# guard directly (4665, 4703, 4873, 4915). No prefix or template exemptions.
# Unknown runtime procedures/ISA families fail until separately reviewed.
REVIEWED_MSVC_VECTOR_SYMBOLS = frozenset({
    "__std_swap_ranges_trivially_swappable_noalias",
    "__std_swap_ranges_trivially_swappable",
    "__std_reverse_trivially_swappable_1", "__std_reverse_trivially_swappable_2",
    "__std_reverse_trivially_swappable_4", "__std_reverse_trivially_swappable_8",
    "__std_reverse_copy_trivially_copyable_1", "__std_reverse_copy_trivially_copyable_2",
    "__std_reverse_copy_trivially_copyable_4", "__std_reverse_copy_trivially_copyable_8",
    "__std_find_trivial_1", "__std_find_trivial_2", "__std_find_trivial_4", "__std_find_trivial_8",
    "__std_find_last_trivial_1", "__std_find_last_trivial_2",
    "__std_find_last_trivial_4", "__std_find_last_trivial_8",
    "__std_count_trivial_1", "__std_count_trivial_2", "__std_count_trivial_4", "__std_count_trivial_8",
    "__std_mismatch_1", "__std_mismatch_2", "__std_mismatch_4", "__std_mismatch_8",
    "__std_replace_4", "__std_replace_8", "__std_remove_4", "__std_remove_8",
})
REVIEWED_MSVC_FEATURES = frozenset({"AVX (VEX)", "AVX/AVX2 (ymm)"})
REVIEWED_MSVC_MODULE_SUFFIXES = (
    "\\crt\\src\\stl\\vector_algorithms.cpp",
    "\\crt\\src\\stl\\vector_algorithms.obj",
    # Observed in a local /MT PE linked against MSVC 14.44.35207 libcpmt.lib.
    "\\crt\\github\\stl\\msbuild\\stl_base\\mt\\libcpmt_mt_kernel32.vcxproj"
    "\\objr\\amd64\\vector_algorithms.obj",
    # Observed in the corresponding /MD fixture linked with msvcprt.lib.
    "\\crt\\github\\stl\\msbuild\\stl_base\\md\\msvcp_base_md_kernel32.vcxproj"
    "\\objr\\amd64\\vector_algorithms.obj",
    "\\crt\\github\\stl\\msbuild\\stl_base\\xmd\\msvcp_base_xmd_kernel32.vcxproj"
    "\\objd\\amd64\\vector_algorithms.obj",
)
# MSVC 14.44.35207 crt/src/x64/{memcpy,memset}.asm compare __isa_available
# with __ISA_AVAILABLE_AVX and branch to NoAVX (memcpy:266-267, memset:204-205).
# These exact procedure/module pairs were observed in the /MT fixture PDB.
REVIEWED_MSVC_MEMORY_MODULES = {
    name: "\\crt\\vcruntime\\build\\base\\mt\\libvcruntime_mt_kernel32.vcxproj"
          f"\\objr\\amd64\\{name}.obj"
    for name in ("memcpy", "memset")
}


def _reviewed_runtime_symbol(module: str, symbol: str | None) -> bool:
    normalized = module.replace("/", "\\").lower()
    if symbol in REVIEWED_MSVC_VECTOR_SYMBOLS:
        return normalized.endswith(REVIEWED_MSVC_MODULE_SUFFIXES)
    suffix = REVIEWED_MSVC_MEMORY_MODULES.get(symbol)
    return suffix is not None and normalized.endswith(suffix)


def _pe_sections(path: str) -> tuple[int, list[tuple[int, int, bool]]]:
    with open(path, "rb") as handle:
        data = handle.read(64)
        if len(data) < 64 or data[:2] != b"MZ":
            raise RuntimeError(f"{path} is not a PE image")
        pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
        handle.seek(pe_offset)
        header = handle.read(24)
        if len(header) != 24 or header[:4] != b"PE\0\0":
            raise RuntimeError(f"{path} has no PE signature")
        optional_size = struct.unpack_from("<H", header, 20)[0]
        section_count = struct.unpack_from("<H", header, 6)[0]
        if (struct.unpack_from("<H", header, 4)[0] != 0x8664
                or optional_size < 112 or section_count < 1 or section_count > 96):
            raise RuntimeError(f"{path} has an invalid PE header")
        # Only headers are needed; do not load a large engine image into memory.
        data = header + handle.read(optional_size + section_count * 40)
    pe_offset = 0
    if pe_offset + 24 > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0":
        raise RuntimeError(f"{path} has no PE signature")
    coff = pe_offset + 4
    section_count = struct.unpack_from("<H", data, coff + 2)[0]
    optional_size = struct.unpack_from("<H", data, coff + 16)[0]
    optional = coff + 20
    if optional + optional_size > len(data) or struct.unpack_from("<H", data, optional)[0] != 0x20B:
        raise RuntimeError(f"{path} is not a PE32+ x86-64 image")
    image_base = struct.unpack_from("<Q", data, optional + 24)[0]
    sections = []
    section_table = optional + optional_size
    for index in range(section_count):
        offset = section_table + index * 40
        if offset + 40 > len(data):
            raise RuntimeError(f"{path} has a truncated PE section table")
        virtual_size, rva, raw_size = struct.unpack_from("<III", data, offset + 8)
        characteristics = struct.unpack_from("<I", data, offset + 36)[0]
        sections.append((rva, min(virtual_size, raw_size), bool(characteristics & 0x20000000)))
    occupied = sorted((rva, rva + size) for rva, size, executable in sections if executable and size)
    if any(start < previous_end for (start, _), (_, previous_end) in zip(occupied[1:], occupied)):
        raise RuntimeError(f"{path} has overlapping executable PE sections")
    return image_base, sections


def _pdb_ranges(pdb: str, image: str, pdbutil_path: str | None = None) -> list[PdbRange]:
    if not os.path.isfile(pdb) or os.path.getsize(pdb) == 0:
        raise RuntimeError(f"PE image {image} requires a non-empty PDB: {pdb}")
    pdbutil = pdbutil_path or shutil.which("llvm-pdbutil")
    if not pdbutil:
        raise RuntimeError("PE ISA scanning requires llvm-pdbutil")
    image_base, sections = _pe_sections(image)
    manifest_path = os.path.join(os.path.dirname(__file__), "shipping_symbol_manifest.py")
    spec = importlib.util.spec_from_file_location("spark_shipping_symbol_manifest", manifest_path)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load PE CodeView parser")
    manifest = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = manifest
    spec.loader.exec_module(manifest)
    try:
        with open(image, "rb") as image_file, open(pdb, "rb") as pdb_file:
            image_identity = manifest.inspect_pe(image_file)
            pdb_identity = manifest.inspect_pdb(pdb_file)
    except (OSError, manifest.FormatError, ValueError) as exc:
        raise RuntimeError(f"invalid PE/PDB CodeView identity: {exc}") from exc
    if (image_identity is None or image_identity.pdb_guid != pdb_identity.pdb_guid
            or image_identity.pdb_age != pdb_identity.pdb_age):
        raise RuntimeError(f"PDB {pdb} does not match PE CodeView identity")
    # PDB dumps can be much larger than the image. Parse incrementally.
    with subprocess.Popen([pdbutil, "dump", "--symbols", pdb], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, errors="replace") as proc:
        try:
            ranges = _parse_pdb_ranges(proc.stdout, image_base, sections)
        except Exception:
            proc.kill()
            proc.wait()
            raise
    if proc.returncode != 0:
        raise RuntimeError(f"llvm-pdbutil cannot read {pdb} (exit {proc.returncode})")
    return ranges


def _parse_pdb_ranges(lines, image_base: int, sections: list[tuple[int, int, bool]]) -> list[PdbRange]:
    """Read decimal section:offset procedure records from llvm-pdbutil dump."""
    module = None
    pending = None
    ranges = []
    module_re = re.compile(r"^\s*Mod\s+[0-9A-Fa-f]+\s+\|\s+`([^`]*)`\s*:")
    proc_re = re.compile(r"S_(?:G|L)PROC32(?:_ID)? .* `([^`]*)`")
    addr_re = re.compile(r"addr = ([0-9]+):([0-9]+), code size = ([0-9]+)")
    for line in lines:
        module_match = module_re.match(line)
        if module_match:
            module = module_match.group(1)
            pending = None
            continue
        proc_match = proc_re.search(line)
        if proc_match:
            pending = proc_match.group(1)
            continue
        if re.match(r"^\s*[0-9]+\s+\|\s+S_", line):
            if pending and module and _reviewed_runtime_symbol(module, pending):
                raise RuntimeError("PDB procedure has no address/length record")
            pending = None
        addr_match = addr_re.search(line)
        if not addr_match or pending is None or module is None:
            continue
        section = int(addr_match.group(1), 10)
        offset = int(addr_match.group(2), 10)
        size = int(addr_match.group(3))
        if not _reviewed_runtime_symbol(module, pending):
            pending = None
            continue
        if section < 1 or section > len(sections) or size <= 0:
            raise RuntimeError(f"PDB contains an invalid range for {pending}")
        section_rva, section_size, executable = sections[section - 1]
        if not executable or offset + size > section_size:
            raise RuntimeError(f"PDB range for {pending} exceeds executable PE section bounds")
        start = image_base + section_rva + offset
        ranges.append(PdbRange(start, start + size, pending, module))
        pending = None
    if pending and module and _reviewed_runtime_symbol(module, pending):
        raise RuntimeError("PDB procedure has no address/length record")
    return ranges


def _pdb_allows(feature: str, address: int, ranges: list[PdbRange]) -> bool:
    return feature in REVIEWED_MSVC_FEATURES and any(item.start <= address < item.end for item in ranges)


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


def scan(tool: list[str], path: str, allow: list[re.Pattern[str]], pdb_ranges: list[PdbRange] | None = None) -> ScanResult:
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
            address = int(insn.group(1), 16)
            in_reviewed_range = pdb_ranges is not None and _pdb_allows(feature, address, pdb_ranges)
            if feature == "TZCNT":
                result.informational[feature] += 1
            elif in_reviewed_range or (pdb_ranges is None and symbol_allowed):
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
    parser.add_argument("--pdbutil", help="llvm-pdbutil executable (default: search PATH)")
    parser.add_argument("--pdb", action="append", default=[], metavar="PDB", help="matching PDB for a PE image (required)")
    parser.add_argument(
        "--allow-symbol",
        action="append",
        default=[],
        metavar="REGEX",
        help="exempt ELF functions matching REGEX (CPUID-dispatched code only; never applies to PE)",
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
            with open(path, "rb") as header_probe:
                is_pe = header_probe.read(2) == b"MZ"
            pdb = None
            if is_pe:
                if len(args.binaries) == 1 and len(args.pdb) == 1 and "=" not in args.pdb[0]:
                    pdb = args.pdb[0]
                else:
                    for pair in args.pdb:
                        image_name, separator, pdb_name = pair.partition("=")
                        if separator and os.path.abspath(image_name) == os.path.abspath(path):
                            pdb = pdb_name
                if not pdb:
                    raise RuntimeError(f"PE image {path} requires --pdb <matching PDB>")
            result = scan(tool, path, allow, _pdb_ranges(pdb, path, args.pdbutil) if pdb else None)
        except (RuntimeError, OSError, ValueError, struct.error) as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
        report(result, args.max_report)
        failed = failed or bool(result.violations)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
