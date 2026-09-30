#!/usr/bin/env python3
"""Fail when an x86-64 binary executes instructions above the stable-v1 CPU floor.

BLD-100 / owner decision OD-04: the stable-v1 CPU floor is x86-64 with SSE4.2
and POPCNT (x86-64-v2). cmake/SparkCpuFloor.cmake rejects above-floor compiler
flags at configure time; this tool checks the linked result, so an above-floor
instruction from one of the families below that reaches a shipped image
through any route -- a per-source COMPILE_OPTIONS entry, an inline-asm block, a
prebuilt static library, a target("avx2") attribute -- is still caught.

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
  XSAVE     xsave*, xrstor*, xgetbv, xsetbv
  TSX       xbegin, xend, xabort, xtest (RTM)
  FSGSBASE  rdfsbase, rdgsbase, wrfsbase, wrgsbase
  SSE4a     extrq, insertq, movntsd, movntss (AMD only)
  3DNow!    femms, pf*, pi2f*
  others    clflushopt, clwb, rdpid, umonitor/umwait/tpause (WAITPKG),
            movdiri, movdir64b, serialize, rdpkru/wrpkru (PKU) and AMX tile
            instructions

The HLE prefixes xacquire/xrelease are stripped, not classified: CPUs without
TSX ignore them architecturally, so they never require the feature.

TZCNT is reported but is not a violation: it shares its encoding (F3 0F BC)
with REP BSF, which GCC and Clang emit at the SSE4.2 floor for ctz on inputs
known to be non-zero, and a pre-BMI CPU executes it as BSF with the same
result for those inputs. A build that really enables BMI also emits the BMI1
instructions above, and SparkCpuFloor.cmake rejects -mbmi at configure time.

ELF functions selected only after a CPUID check may be exempted with
--allow-symbol REGEX. The one reviewed ELF exemption built in is XGETBV (only)
inside Spark::Detail::ReadXcr0(), which Utils/MultiISA.h runs after its
CPUID.1:ECX.OSXSAVE check and keeps out of line.

PE images require --pdb PDB (or repeated --pdb IMAGE=PDB for multiple images)
and llvm-pdbutil; their RSDS GUID/age must match. Regex exemptions never apply
to PE images. A PE instruction is exempt only through one of these reviewed,
feature-scoped mechanisms; anything else fails closed until reviewed:

* Section contributions (DBI stream) of the MSVC STL's vector_algorithms.obj,
  identified by its exact Microsoft build path and a library from the reviewed
  MSVC 14.44.35207 toolset: AVX, AVX2 and LZCNT only (review below).
* The exact PDB procedure/module pairs memcpy/memset (AVX/AVX2), and XGETBV in
  vcruntime's __isa_available_init and in Spark::Detail::ReadXcr0.
* Guard dominance for code the MSVC headers inline into arbitrary procedures:
  the instruction is reachable from its procedure's entry, and unreachable once
  the edges implied by a reviewed guard compare are removed from that
  procedure's control-flow graph (see REVIEWED_MSVC_GUARDS).

Exit status: 0 when no violation remains, 1 when violations remain, 2 on a
usage or tool error (missing file, no disassembler, not an x86-64 image).
"""

from __future__ import annotations

import argparse
import bisect
import importlib.util
import os
import re
import shutil
import struct
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

XSAVE = "XSAVE"
_XSAVE_MNEMONICS = ("xsave", "xsaveopt", "xsavec", "xsaves", "xrstor", "xrstors")

# Legacy-encoded (non-VEX) extensions that are not part of x86-64-v2, matched as
# whole mnemonics. VEX VAES/VPCLMULQDQ/GFNI have independent feature classes
# so they cannot inherit AVX/AVX2 exemptions.
LEGACY_EXTENSION_MNEMONICS = {
    "adcx": "ADX",
    "adox": "ADX",
    "rdrand": "RDRAND",
    "rdseed": "RDSEED",
    **{name: XSAVE for base in _XSAVE_MNEMONICS for name in (base, base + "64")},
    "xgetbv": XSAVE,
    "xsetbv": XSAVE,
    "xbegin": "TSX",
    "xend": "TSX",
    "xabort": "TSX",
    "xtest": "TSX",
    "rdfsbase": "FSGSBASE",
    "rdgsbase": "FSGSBASE",
    "wrfsbase": "FSGSBASE",
    "wrgsbase": "FSGSBASE",
    "extrq": "SSE4a",
    "insertq": "SSE4a",
    "movntsd": "SSE4a",
    "movntss": "SSE4a",
    "femms": "3DNow!",
    "clflushopt": "CLFLUSHOPT",
    "clwb": "CLWB",
    "rdpid": "RDPID",
    "umonitor": "WAITPKG",
    "umwait": "WAITPKG",
    "tpause": "WAITPKG",
    "movdiri": "MOVDIRI",
    "movdir64b": "MOVDIR64B",
    "serialize": "SERIALIZE",
    "rdpkru": "PKU",
    "wrpkru": "PKU",
    "ldtilecfg": "AMX",
    "sttilecfg": "AMX",
}

# Mnemonic prefixes of legacy-encoded extension families. GNU objdump prints
# PCLMULQDQ as pclmullqlqdq/pclmulhqhqdq/... depending on its immediate.
LEGACY_EXTENSION_PREFIXES = (
    ("aes", "AES-NI"),
    ("pclmul", "PCLMULQDQ"),
    ("sha1", "SHA"),
    ("sha256", "SHA"),
    ("gf2p8", "GFNI"),
    ("pf", "3DNow!"),
    ("pi2f", "3DNow!"),
    ("tile", "AMX"),
    ("tdp", "AMX"),
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
# llvm-objdump continues a long annotation on its own line ("        # 0x1400...").
CONTINUED_ANNOTATION_RE = re.compile(r"^\s+#\s*(?:0x)?([0-9a-fA-F]+)(?:\s*<[^>]*>)?\s*$")
ANNOTATION_ADDRESS_RE = re.compile(r"#\s*(?:0x)?([0-9a-fA-F]+)(?:\s*<[^>]*>)?\s*$")
BRANCH_TARGET_RE = re.compile(r"^(?:0x)?([0-9a-fA-F]+)\b")
# Raw instruction bytes, then a tab, then the instruction (both disassemblers).
RAW_BYTES_RE = re.compile(r"^((?:[0-9a-fA-F]{2} )+)\s*\t(.*)$")
VECTOR_REG_RE = re.compile(r"%?\b([xyz])mm\d+\b")
WIDE_REG_RE = re.compile(r"%?\b([yz])mm\d+\b")
OPMASK_RE = re.compile(r"%k[0-7]\b|\{%?k[0-7]\}")
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


AVX_FEATURES = frozenset({"AVX (VEX)", "AVX/AVX2 (ymm)"})
# MSVC 14.44.35207 crt/src/{x64/memcpy,x64/memset}.asm compare __isa_available
# with __ISA_AVAILABLE_AVX and branch to NoAVX (memcpy:266-267, memset:204-205).
REVIEWED_MSVC_FEATURES = AVX_FEATURES


@dataclass(frozen=True)
class PdbRange:
    start: int
    end: int
    symbol: str
    module: str
    features: frozenset = REVIEWED_MSVC_FEATURES


# The only toolset whose runtime sources were reviewed. Contribution ranges and
# the cpu_disp pair apply only to objects the PDB says came from its libraries.
REVIEWED_MSVC_TOOLSET_LIB = re.compile(
    r"\\msvc\\14\.44\.35207\\lib\\x64\\(msvcprt|msvcprtd|libcpmt|libcpmtd|msvcrt|msvcrtd)\.lib$"
)

# Every code contribution of MSVC 14.44.35207 crt/src/stl/vector_algorithms.cpp
# (5382 lines) was reviewed. Its only above-floor intrinsics are 372 _mm256_*
# uses (AVX/AVX2 on ymm, VEX-encoding the surrounding SSE code), _mm256_zeroupper
# and four _lzcnt_u32 (2653, 2667, 3261, 3272); it uses no FMA, F16C, BMI or
# AVX-512 intrinsic. Every one is dominated by _Use_avx2() (line 26: __isa_enabled
# & (1 << __ISA_AVAILABLE_AVX2)): the swap/reverse entries guard directly
# (99-460); the _Minmax_traits_*_avx traits (575-1735) are instantiated only at
# 2055 and 2238; find/find_last/count guard at 2568/2640/2840; the bitmap and
# shuffle helpers (2933-3700) run from _Impl_first_avx/_Impl_last_avx, which run
# only for _Strategy::_Vector_bitmap, which _Pick_strategy (3157) returns only
# when passed _Use_avx2() (3747, 3984) or from _Dispatch_pos_avx_4_8 (called at
# 3822 under _Use_avx2()), and _Impl_4_8 runs at 3722/3792 under _Use_avx2();
# mismatch guards at 4020, replace at 4665/4703, remove at 4873/4915 and the
# bitset conversions at 5092/5348. _lzcnt_u32 2653/2667 sit in the 2640 branch,
# 3261/3272 in _Impl_last_avx. LZCNT is allowed because every AVX2 CPU also
# implements LZCNT (the STL's own countl_zero treats the AVX2 level as
# "_Definitely_have_lzcnt", __msvc_bit_utils.hpp:119). The _mm_* intrinsics are
# SSE2-SSE4.2, within the floor. The source review cannot see code the compiler
# adds: the shipped object also holds auto-vectorized EVEX loops (vpminuq xmm)
# behind "cmpl $0x6, __isa_available", which classify as AVX-512 and stay
# violations until that dispatch is reviewed separately.
REVIEWED_MSVC_VECTOR_FEATURES = AVX_FEATURES | {"LZCNT"}
# Microsoft build paths observed in locally linked /MT, /MD and /MDd images.
REVIEWED_MSVC_MODULE_SUFFIXES = (
    "\\crt\\github\\stl\\msbuild\\stl_base\\mt\\libcpmt_mt_kernel32.vcxproj\\objr\\amd64\\vector_algorithms.obj",
    "\\crt\\github\\stl\\msbuild\\stl_base\\md\\msvcp_base_md_kernel32.vcxproj\\objr\\amd64\\vector_algorithms.obj",
    "\\crt\\github\\stl\\msbuild\\stl_base\\xmd\\msvcp_base_xmd_kernel32.vcxproj\\objd\\amd64\\vector_algorithms.obj",
)
# Exact procedure/module pairs, observed in the /MT fixture PDB.
REVIEWED_MSVC_MEMORY_MODULES = {
    name: f"\\crt\\vcruntime\\build\\base\\mt\\libvcruntime_mt_kernel32.vcxproj\\objr\\amd64\\{name}.obj"
    for name in ("memcpy", "memset")
}
# vcruntime's cpu_disp source is not shipped; its disassembly in 14.44.35207
# msvcrt.lib/msvcrtd.lib was reviewed instead: the only XGETBV runs after
# "bt $0x1b" on the CPUID.1 ECX value (OSXSAVE) falls through, as Intel requires.
REVIEWED_MSVC_XSAVE_MODULES = (
    "\\crt\\vcstartup\\build\\md\\msvcrt_md_kernel32.vcxproj\\objr\\amd64\\cpu_disp.obj",
    "\\crt\\vcstartup\\build\\xmd\\msvcrt_xmd_kernel32.vcxproj\\objd\\amd64\\cpu_disp.obj",
)
# Utils/MultiISA.h: DetectCpuFeatures() calls ReadXcr0() only when CPUID.1:ECX
# reports OSXSAVE; ReadXcr0 is noinline so XGETBV stays inside it.
SPARK_XSAVE_PROCEDURE = "Spark::Detail::ReadXcr0"
ELF_REVIEWED_PROCEDURES = {SPARK_XSAVE_PROCEDURE + "()": frozenset({XSAVE})}

INT32_MIN = -(1 << 31)
INT32_MAX = (1 << 31) - 1


@dataclass(frozen=True)
class GuardSpec:
    """A reviewed global int whose value decides whether inline code may run."""

    predicate: tuple[tuple[int, int], ...]  # disjoint signed 32-bit intervals
    features: frozenset


# Windows SDK 10.0.26100.0 ucrt/wchar.h:207-214 declares _Avx2WmemEnabled with
# /alternatename to a selectany _Avx2WmemEnabledWeakValue = 0; wmemchr (:274) and
# wmemcmp (:401) take their AVX2 paths only when it is non-zero. With the /MD CRT
# only libucrt(d).lib defines the strong symbol, so these images resolve both
# names to the never-written zero. AVX/AVX2 only.
_WMEM_GUARD = GuardSpec(((INT32_MIN, -1), (1, INT32_MAX)), AVX_FEATURES)
# Guards the MSVC headers read inline. An edge is removed only when every value
# that takes it satisfies the predicate, and only for the features listed.
REVIEWED_MSVC_GUARDS = {
    "_Avx2WmemEnabled": _WMEM_GUARD,
    "_Avx2WmemEnabledWeakValue": _WMEM_GUARD,
    # MSVC 14.44.35207 include/__msvc_bit_utils.hpp:115-127
    # (_Checked_x86_x64_countl_zero) uses lzcnt only when __isa_available >=
    # _Stl_isa_available_avx2 (5, __ISA_AVAILABLE_AVX2). LZCNT only.
    "__isa_available": GuardSpec(((5, INT32_MAX),), frozenset({"LZCNT"})),
}


@dataclass(frozen=True)
class Procedure:
    start: int
    end: int
    name: str
    ambiguous: bool  # overlaps another procedure's extent without matching it


@dataclass
class PdbInfo:
    ranges: list[PdbRange]
    procedures: list[Procedure]
    guards: dict[int, GuardSpec]

    def __post_init__(self) -> None:
        self.procedures.sort(key=lambda proc: (proc.start, proc.end))
        self._starts = [proc.start for proc in self.procedures]
        # A gap must not search every earlier procedure for every instruction.
        # Prefix maxima preserve overlapping extents while bounding that search.
        self._max_ends = []
        maximum = 0
        for proc in self.procedures:
            maximum = max(maximum, proc.end)
            self._max_ends.append(maximum)

    def procedure_at(self, address: int) -> Procedure | None:
        index = bisect.bisect_right(self._starts, address) - 1
        while index >= 0 and self._max_ends[index] > address:
            proc = self.procedures[index]
            if address < proc.end:
                return proc
            index -= 1
        return None


def _normalized(path: str) -> str:
    return path.replace("/", "\\").lower()


def _reviewed_runtime_symbol(module: str, symbol: str | None) -> bool:
    suffix = REVIEWED_MSVC_MEMORY_MODULES.get(symbol)
    return suffix is not None and _normalized(module).endswith(suffix)


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
    coff = 4
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
        executable = bool(characteristics & 0x20000000)
        # Instructions must have file bytes; data guards can live in the
        # loader's zero-filled tail (VirtualSize may exceed SizeOfRawData).
        size = min(virtual_size, raw_size) if executable else virtual_size
        sections.append((rva, size, executable))
    occupied = sorted((rva, rva + size) for rva, size, executable in sections if executable and size)
    if any(start < previous_end for (start, _), (_, previous_end) in zip(occupied[1:], occupied)):
        raise RuntimeError(f"{path} has overlapping executable PE sections")
    return image_base, sections


def _code_address(section: int, offset: int, size: int, image_base: int,
                  sections: list[tuple[int, int, bool]], what: str) -> int:
    """Validate a section:offset/size extent against executable PE section bounds."""
    if section < 1 or section > len(sections) or size <= 0:
        raise RuntimeError(f"PDB contains an invalid range for {what}")
    section_rva, section_size, executable = sections[section - 1]
    if not executable or offset + size > section_size:
        raise RuntimeError(f"PDB range for {what} exceeds executable PE section bounds")
    return image_base + section_rva + offset


def _run_pdbutil(pdbutil: str, arguments: list[str], pdb: str, parser):
    # PDB dumps can be much larger than the image. Parse incrementally.
    with subprocess.Popen([pdbutil, "dump", *arguments, pdb], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True, errors="replace") as proc:
        try:
            parsed = parser(proc.stdout)
        except Exception:
            proc.kill()
            proc.wait()
            raise
    if proc.returncode != 0:
        raise RuntimeError(f"llvm-pdbutil cannot read {pdb} (exit {proc.returncode})")
    return parsed


def _pdb_info(pdb: str, image: str, pdbutil_path: str | None = None) -> PdbInfo:
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
    contributions, libraries = _run_pdbutil(
        pdbutil, ["--modules", "--section-contribs"], pdb,
        lambda lines: _parse_section_contributions(lines, image_base, sections))
    ranges, procedures = _run_pdbutil(
        pdbutil, ["--symbols"], pdb, lambda lines: _parse_pdb_symbols(lines, image_base, sections, libraries))
    guards = _run_pdbutil(pdbutil, ["--publics"], pdb, lambda lines: _parse_pdb_guards(lines, image_base, sections))
    return PdbInfo(contributions + ranges, procedures, guards)


MODULE_RE = re.compile(r"^\s*Mod\s+([0-9]+)\s+\|\s+`([^`]*)`\s*:")
OBJ_RE = re.compile(r"^\s*Obj:\s+`([^`]*)`")
CONTRIBUTION_RE = re.compile(r"^\s*SC\[[^\]]*\]\s*\|\s*mod = ([0-9]+), ([0-9]+):([0-9]+), size = (-?[0-9]+),")


def _reviewed_library(libraries: dict[int, tuple[str, str]], index: int, suffixes: tuple[str, ...]) -> bool:
    module, library = libraries.get(index, ("", ""))
    return _normalized(module).endswith(suffixes) and REVIEWED_MSVC_TOOLSET_LIB.search(_normalized(library)) is not None


def _parse_section_contributions(lines, image_base: int, sections: list[tuple[int, int, bool]]):
    """Read `dump --modules --section-contribs`: module paths, then DBI contributions.

    Returns the reviewed vector_algorithms code ranges and {module index: (module
    path, library path)}. Section:offset values are decimal, as in --symbols.
    """
    libraries: dict[int, list[str]] = {}
    current = None
    in_contributions = False
    pending = None
    ranges = []

    def finish(item) -> None:
        index, section, offset, size, flags = item
        if index not in libraries:
            raise RuntimeError(f"PDB section contribution names unknown module {index}")
        is_code = bool(flags & {"IMAGE_SCN_CNT_CODE", "IMAGE_SCN_MEM_EXECUTE"})
        if is_code and _reviewed_library(libraries, index, REVIEWED_MSVC_MODULE_SUFFIXES):
            module = libraries[index][0]
            start = _code_address(section, offset, size, image_base, sections, module)
            ranges.append(
                PdbRange(start, start + size, "<section contribution>", module, REVIEWED_MSVC_VECTOR_FEATURES))

    for line in lines:
        if not in_contributions:
            module_match = MODULE_RE.match(line)
            if module_match:
                current = int(module_match.group(1), 10)
                libraries[current] = [module_match.group(2), ""]
                continue
            obj_match = OBJ_RE.match(line)
            if obj_match and current is not None and not libraries[current][1]:
                libraries[current][1] = obj_match.group(1)
                continue
            in_contributions = "Section Contributions" in line
            continue
        contribution = CONTRIBUTION_RE.match(line)
        if contribution:
            if pending:
                finish(pending)
            pending = (*(int(value, 10) for value in contribution.groups()), set())
            continue
        if pending and line.strip().startswith("IMAGE_SCN_"):
            pending[4].update(token.strip() for token in line.split("|") if token.strip())
    if pending:
        finish(pending)
    if not in_contributions:
        raise RuntimeError("llvm-pdbutil printed no section contributions")
    return ranges, {index: (module, library) for index, (module, library) in libraries.items()}


def _parse_pdb_symbols(lines, image_base: int, sections: list[tuple[int, int, bool]],
                       libraries: dict[int, tuple[str, str]] | None = None):
    """Read decimal section:offset procedure records from `dump --symbols`.

    Returns the reviewed procedure ranges and every procedure extent (for the
    guard-dominance analysis).
    """
    libraries = libraries or {}
    module = None
    module_index = None
    pending = None
    ranges = []
    extents: dict[tuple[int, int], set[str]] = {}
    # Names may contain backticks ("`anonymous namespace'::..."): take everything
    # between the first backtick after the record size and the last one.
    proc_re = re.compile(r"S_(?:G|L)PROC32(?:_ID)? \[[^\]]*\] `(.*)`\s*$")
    addr_re = re.compile(r"addr = ([0-9]+):([0-9]+), code size = ([0-9]+)")

    def reviewed(name: str) -> frozenset | None:
        if module is None:
            return None
        if _reviewed_runtime_symbol(module, name):
            return REVIEWED_MSVC_FEATURES
        if name == "__isa_available_init" and _reviewed_library(libraries, module_index, REVIEWED_MSVC_XSAVE_MODULES):
            return frozenset({XSAVE})
        if name == SPARK_XSAVE_PROCEDURE:
            return frozenset({XSAVE})
        return None

    for line in lines:
        module_match = MODULE_RE.match(line)
        if module_match:
            module_index = int(module_match.group(1), 10)
            module = module_match.group(2)
            pending = None
            continue
        proc_match = proc_re.search(line)
        if proc_match:
            pending = proc_match.group(1)
            continue
        if re.match(r"^\s*[0-9]+\s+\|\s+S_", line):
            if pending and reviewed(pending):
                raise RuntimeError("PDB procedure has no address/length record")
            pending = None
        addr_match = addr_re.search(line)
        if not addr_match or pending is None or module is None:
            continue
        section = int(addr_match.group(1), 10)
        offset = int(addr_match.group(2), 10)
        size = int(addr_match.group(3), 10)
        features = reviewed(pending)
        if features:
            start = _code_address(section, offset, size, image_base, sections, pending)
            ranges.append(PdbRange(start, start + size, pending, module, features))
        elif 1 <= section <= len(sections) and size > 0:
            section_rva, section_size, executable = sections[section - 1]
            if executable and offset + size <= section_size:
                start = image_base + section_rva + offset
                extents.setdefault((start, start + size), set()).add(pending)
        pending = None
    if pending and reviewed(pending):
        raise RuntimeError("PDB procedure has no address/length record")
    for item in ranges:
        extents.setdefault((item.start, item.end), set()).add(item.symbol)
    # The Spark exemption is granted by name alone, so it holds only when no other
    # procedure (for example one folded onto the same bytes by /OPT:ICF) shares them.
    ranges = [item for item in ranges
              if item.symbol != SPARK_XSAVE_PROCEDURE or extents[(item.start, item.end)] == {item.symbol}]
    ordered = sorted(extents)
    procedures = []
    previous_end = 0
    for index, (start, end) in enumerate(ordered):
        # Folded procedures share identical bytes, which is harmless for the CFG;
        # an overlap between different extents leaves the entry and extent in doubt.
        overlaps = previous_end > start or (index + 1 < len(ordered) and ordered[index + 1][0] < end)
        previous_end = max(previous_end, end)
        procedures.append(Procedure(start, end, min(extents[(start, end)]), overlaps))
    return ranges, procedures


def _parse_pdb_guards(lines, image_base: int, sections: list[tuple[int, int, bool]]) -> dict[int, GuardSpec]:
    """Resolve REVIEWED_MSVC_GUARDS addresses from `dump --publics` (S_PUB32)."""
    pub_re = re.compile(r"S_PUB32 \[[^\]]*\] `(.*)`\s*$")
    addr_re = re.compile(r"addr = ([0-9]+):([0-9]+)")
    found: dict[str, set[int]] = {}
    pending = None
    for line in lines:
        pub_match = pub_re.search(line)
        if pub_match:
            pending = pub_match.group(1) if pub_match.group(1) in REVIEWED_MSVC_GUARDS else None
            continue
        addr_match = addr_re.search(line)
        if pending and addr_match:
            section = int(addr_match.group(1), 10)
            offset = int(addr_match.group(2), 10)
            if not 1 <= section <= len(sections) or offset + 4 > sections[section - 1][1]:
                raise RuntimeError(f"PDB guard {pending} is outside its section")
            address = image_base + sections[section - 1][0] + offset
            found.setdefault(pending, set()).add(address)
            pending = None
    guards: dict[int, GuardSpec] = {}
    conflicts = set()
    for name, addresses in found.items():
        if len(addresses) != 1:
            continue  # an ambiguous guard symbol grants nothing
        address = next(iter(addresses))
        spec = REVIEWED_MSVC_GUARDS[name]
        if address in guards and guards[address] != spec:
            conflicts.add(address)
        guards[address] = spec
    return {address: spec for address, spec in guards.items() if address not in conflicts}


def _pdb_allows(feature: str, address: int, ranges: list[PdbRange]) -> bool:
    return any(item.start <= address < item.end and feature in item.features for item in ranges)


# ---------------------------------------------------------------------------
# Guard dominance: inline CRT/STL code guarded by a reviewed global.

_CONDITIONS = {
    "je": ("eq", False), "jz": ("eq", False), "jne": ("eq", True), "jnz": ("eq", True),
    "jl": ("lt", False), "jnge": ("lt", False), "jge": ("lt", True), "jnl": ("lt", True),
    "jle": ("le", False), "jng": ("le", False), "jg": ("le", True), "jnle": ("le", True),
    "jb": ("ult", False), "jnae": ("ult", False), "jc": ("ult", False),
    "jae": ("ult", True), "jnb": ("ult", True), "jnc": ("ult", True),
    "jbe": ("ule", False), "jna": ("ule", False), "ja": ("ule", True), "jnbe": ("ule", True),
}
_OTHER_CONDITIONAL = {"js", "jns", "jo", "jno", "jp", "jnp", "jpe", "jpo", "jrcxz", "jecxz",
                      "loop", "loope", "loopne", "loopz", "loopnz"}
_TERMINATORS = {"ret", "retq", "retl", "ud2", "int3", "hlt"}
_REG32 = {"eax": "a", "ebx": "b", "ecx": "c", "edx": "d", "esi": "si", "edi": "di", "ebp": "bp", "esp": "sp",
          **{f"r{n}d": f"r{n}" for n in range(8, 16)}}
_REG_FAMILY = {
    **{name: family for name, family in _REG32.items()},
    **{"r" + name[1:]: family for name, family in _REG32.items() if name.startswith("e")},
    **{f"r{n}": f"r{n}" for n in range(8, 16)}, **{f"r{n}w": f"r{n}" for n in range(8, 16)},
    **{f"r{n}b": f"r{n}" for n in range(8, 16)},
    "ax": "a", "bx": "b", "cx": "c", "dx": "d", "si": "si", "di": "di", "bp": "bp", "sp": "sp",
    "al": "a", "ah": "a", "bl": "b", "bh": "b", "cl": "c", "ch": "c", "dl": "d", "dh": "d",
    "sil": "si", "dil": "di", "bpl": "bp", "spl": "sp",
}


@dataclass(frozen=True)
class Instruction:
    address: int
    mnemonic: str
    operands: str
    annotation: int | None  # absolute address from a "# 0x..." RIP-relative annotation


def _intervals(kind: str, constant: int, negate: bool) -> list[tuple[int, int]]:
    """Signed 32-bit values v for which "cmp $constant, v" takes (or, negated, skips) the branch."""
    if kind in ("ult", "ule"):
        unsigned = constant & 0xFFFFFFFF
        high = unsigned - 1 if kind == "ult" else unsigned
        taken_unsigned = [(0, high)] if high >= 0 else []
        taken = []
        for low, top in taken_unsigned:
            if low <= INT32_MAX:
                taken.append((low, min(top, INT32_MAX)))
            if top > INT32_MAX:
                taken.append((max(low, INT32_MAX + 1) - (1 << 32), top - (1 << 32)))
    else:
        low, high = {"eq": (constant, constant), "lt": (INT32_MIN, constant - 1), "le": (INT32_MIN, constant)}[kind]
        taken = [(low, high)] if low <= high else []
    if not negate:
        return taken
    result, cursor = [], INT32_MIN
    for low, high in sorted(taken):
        if low > cursor:
            result.append((cursor, low - 1))
        cursor = max(cursor, high + 1)
    if cursor <= INT32_MAX:
        result.append((cursor, INT32_MAX))
    return result


def _split_operands(operands: str) -> list[str]:
    """Split AT&T operands on the commas outside a memory operand's parentheses."""
    parts, depth, current = [], 0, ""
    for char in operands:
        depth += {"(": 1, ")": -1}.get(char, 0)
        if char == "," and depth == 0:
            parts.append(current.strip())
            current = ""
        else:
            current += char
    return parts + [current.strip()] if current.strip() else parts


def _is_flag_neutral_move(insn: Instruction) -> bool:
    # mov*/lea* never write flags; string moves (movsb...) have implicit operands.
    return (insn.mnemonic.startswith(("mov", "lea")) and not re.fullmatch(r"movs[bwlq]?", insn.mnemonic)
            and len(_split_operands(insn.operands)) == 2)


def _guard_compare(insns: list[Instruction], jcc: int, leaders: set[int],
                   guards: dict[int, GuardSpec]) -> tuple[GuardSpec, int] | None:
    """Find "cmpl SRC, guard(%rip)" setting the flags jcc reads, with no other way in."""
    index = jcc - 1
    while index >= 0 and _is_flag_neutral_move(insns[index]):
        index -= 1
    # No branch may enter between the compare and the jcc (the compare itself may be a target).
    if index < 0 or any(i in leaders for i in range(index + 1, jcc + 1)):
        return None
    compare = insns[index]
    operands = _split_operands(compare.operands)
    if compare.mnemonic not in ("cmpl", "cmp") or len(operands) != 2 or not operands[1].endswith("(%rip)"):
        return None
    spec = guards.get(compare.annotation) if compare.annotation is not None else None
    if spec is None:
        return None
    source = operands[0]
    if source.startswith("$"):
        if compare.mnemonic != "cmpl":
            return None  # the operand size is not stated; the guards are 32-bit ints
        constant = int(source[1:], 0)
        if constant > INT32_MAX:
            constant -= 1 << 32
        return spec, constant
    register = source.lstrip("%").lower()
    if register not in _REG32:
        return None
    # MSVC also compares against a register it zeroed with "xor %r32, %r32" earlier in
    # the same block; only mov/lea instructions that leave that register alone may sit
    # between, and no branch may enter after the xor.
    family = _REG32[register]
    while index > 0 and index not in leaders:
        index -= 1
        insn = insns[index]
        parts = _split_operands(insn.operands)
        if insn.mnemonic in ("xorl", "xor") and parts == [source, source]:
            return spec, 0
        destination = parts[-1].lstrip("%").lower() if parts and parts[-1].startswith("%") else None
        if not _is_flag_neutral_move(insn) or _REG_FAMILY.get(destination) == family:
            return None
    return None


def _guard_exempt(insns: list[Instruction], candidates: list[tuple[int, str]],
                  guards: dict[int, GuardSpec], extent: tuple[int, int]) -> set[int]:
    """Return indices of candidate instructions that only reviewed guard edges reach.

    The CFG is instruction-level over one procedure: jcc -> target and
    fallthrough; jmp -> target (or leaves the procedure); ret/ud2/int3/hlt end a
    path; everything else falls through. An indirect jump or a branch into the
    middle of an instruction makes the whole procedure fail closed. Code reached
    only through exception handlers or from another procedure is unreachable from
    the entry here, so it is never exempt.
    """
    index_of = {insn.address: i for i, insn in enumerate(insns)}
    successors: list[list[tuple[int, str]]] = []
    targets: set[int] = set()
    start, end = extent
    for i, insn in enumerate(insns):
        mnemonic = insn.mnemonic
        if mnemonic == "<undecodable>":
            return set()  # unknown bytes may change flags or control flow
        edges: list[tuple[int, str]] = []
        is_jump = mnemonic in ("jmp", "jmpq") or mnemonic in _CONDITIONS or mnemonic in _OTHER_CONDITIONAL
        if is_jump or mnemonic in ("call", "callq"):
            if insn.operands.startswith("*"):
                if is_jump:
                    return set()
            else:
                target_match = BRANCH_TARGET_RE.match(insn.operands)
                if target_match is None:
                    return set()
                target = int(target_match.group(1), 16)
                if start <= target < end:
                    if target not in index_of:
                        return set()
                    edges.append((index_of[target], "taken"))
                    targets.add(index_of[target])
        if mnemonic not in _TERMINATORS and mnemonic not in ("jmp", "jmpq") and i + 1 < len(insns):
            edges.append((i + 1, "fall"))
        successors.append(edges)

    removed_for: dict[str, set[tuple[int, str]]] = {}
    for i, insn in enumerate(insns):
        if insn.mnemonic not in _CONDITIONS:
            continue
        found = _guard_compare(insns, i, targets, guards)
        if found is None:
            continue
        spec, constant = found
        kind, negate = _CONDITIONS[insn.mnemonic]
        for edge_kind, edge_negate in (("taken", negate), ("fall", not negate)):
            implied = _intervals(kind, constant, edge_negate)
            if implied and all(any(low >= p_low and high <= p_high for p_low, p_high in spec.predicate)
                               for low, high in implied):
                for feature in spec.features:
                    removed_for.setdefault(feature, set()).add((i, edge_kind))

    def reachable(removed: set[tuple[int, str]]) -> set[int]:
        seen, stack = {0}, [0]
        while stack:
            node = stack.pop()
            for successor, kind in successors[node]:
                if (node, kind) not in removed and successor not in seen:
                    seen.add(successor)
                    stack.append(successor)
        return seen

    full = reachable(set())
    exempt = set()
    for feature in {feature for _, feature in candidates}:
        if feature not in removed_for:
            continue
        cut = reachable(removed_for[feature])
        exempt.update(i for i, candidate_feature in candidates
                      if candidate_feature == feature and i in full and i not in cut)
    return exempt


# ---------------------------------------------------------------------------


def _base_mnemonic(mnemonic: str) -> str:
    """Strip an AT&T operand-size suffix (shlxq -> shlx) for the scalar sets."""
    match = SIZE_SUFFIX_RE.match(mnemonic)
    if match and match.group(1) in SCALAR_MNEMONICS:
        return match.group(1)
    return mnemonic


def classify(mnemonic: str, operands: str, evex: bool = False) -> str | None:
    """Return the above-floor feature an instruction needs, "TZCNT", or None.

    evex marks an instruction whose encoding starts with the EVEX 0x62 byte: an
    xmm-only EVEX instruction (vpmaxuq, vpternlogd, ...) needs AVX-512, not AVX.
    """
    mnemonic = mnemonic.lower()
    if evex:
        return "AVX-512" if mnemonic.startswith(("v", "k")) else "APX (EVEX)"
    if mnemonic.startswith("v"):
        if mnemonic in VEX_WITHOUT_VECTOR_OPERAND or VECTOR_REG_RE.search(operands):
            if OPMASK_RE.search(operands) or re.search(r"%?\bzmm\d+", operands):
                return "AVX-512"
            # These extensions have independent CPUID bits. Neither an AVX2
            # guard nor a reviewed AVX runtime range authorizes them.
            for prefix, feature in (("vaes", "VAES"), ("vpclmul", "VPCLMULQDQ"), ("vgf2p8", "GFNI"),
                                    ("vprot", "XOP")):
                if mnemonic.startswith(prefix):
                    return feature
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


_LEGACY_PREFIX_BYTES = {0x66, 0x67, 0xF0, 0xF2, 0xF3, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65}


def split_raw_bytes(text: str) -> tuple[bool, str]:
    """Split "62 f2 ed 08 3f d1 <tab>vpmaxuq ..." into (is EVEX, instruction text).

    In 64-bit mode a 0x62 byte after the legacy prefixes always starts an EVEX
    prefix. Text without leading raw bytes is returned unchanged.
    """
    match = RAW_BYTES_RE.match(text)
    if not match:
        return False, text
    encoding = [int(byte, 16) for byte in match.group(1).split()]
    while encoding and encoding[0] in _LEGACY_PREFIX_BYTES:
        encoding.pop(0)
    return bool(encoding) and encoding[0] == 0x62, match.group(2)


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


def _join_split_prefixes(lines):
    """LLVM prints some prefixes (notably LOCK) as separate address records.

    Join only contiguous raw bytes, retaining the prefix's entry address for
    branch analysis. An orphan prefix still reaches the undecodable check.
    """
    pending = None
    for line in lines:
        row = INSN_RE.match(line)
        raw = RAW_BYTES_RE.match(row.group(2)) if row else None
        if pending is not None:
            address, encoding, prefix, original = pending
            pending = None
            if (row and raw and int(row.group(1), 16) == address + len(encoding.split())
                    and len(encoding.split()) + len(raw.group(1).split()) <= 15):
                line = f"{address:x}: {encoding}{raw.group(1)}\t{prefix} {raw.group(2)}"
                row = INSN_RE.match(line)
                raw = RAW_BYTES_RE.match(row.group(2))
            else:
                yield original
        if raw:
            tokens = raw.group(2).lower().split()
            if tokens and all(token in PREFIXES for token in tokens):
                pending = (int(row.group(1), 16), raw.group(1), raw.group(2), line)
                continue
        yield line
    if pending is not None:
        yield pending[3]


def scan_lines(path: str, lines, allow: list[re.Pattern[str]], pdb: PdbInfo | None = None) -> ScanResult:
    """Classify disassembly lines; PE findings go through the reviewed PDB mechanisms."""
    result = ScanResult(path=path)
    symbol = "<unknown>"
    symbol_allowed = False
    procedure: Procedure | None = None
    buffered: list[Instruction] = []
    pending: list[tuple[int, str, Finding]] = []

    def flush() -> None:
        exempt: set[int] = set()
        if pending and procedure is not None and not procedure.ambiguous and buffered[0].address == procedure.start:
            candidates = [(index, feature) for index, feature, _ in pending]
            exempt = _guard_exempt(buffered, candidates, pdb.guards, (procedure.start, procedure.end))
        for index, feature, finding in pending:
            if index in exempt:
                result.allowed[feature] += 1
            else:
                result.violations.append(finding)
        buffered.clear()
        pending.clear()

    for line in _join_split_prefixes(lines):
        header = SYMBOL_RE.match(line)
        if header:
            symbol = header.group(2)
            symbol_allowed = any(pattern.search(symbol) for pattern in allow)
            continue
        insn = INSN_RE.match(line)
        if not insn:
            continued = CONTINUED_ANNOTATION_RE.match(line)
            if continued and buffered and buffered[-1].annotation is None:
                buffered[-1] = Instruction(buffered[-1].address, buffered[-1].mnemonic, buffered[-1].operands,
                                           int(continued.group(1), 16))
            continue
        evex, text = split_raw_bytes(insn.group(2))
        mnemonic, operands = split_instruction(text)
        undecodable = not mnemonic or mnemonic.startswith(("(", "<", "."))
        address = int(insn.group(1), 16)
        if pdb is not None:
            current = pdb.procedure_at(address)
            if current != procedure:
                flush()
                procedure = current
            if procedure is not None:
                annotation = ANNOTATION_ADDRESS_RE.search(text)
                buffered.append(Instruction(address, "<undecodable>" if undecodable else mnemonic.lower(), operands,
                                            int(annotation.group(1), 16) if annotation else None))
        if undecodable:
            # This can be data in .text or an instruction unknown to the tool.
            # Neither is a proof of floor safety; never silently discard it.
            result.violations.append(Finding("undecodable", insn.group(1),
                                             procedure.name if procedure else symbol, text.strip()))
            continue
        result.instructions += 1
        feature = classify(mnemonic, operands, evex)
        if feature is None:
            continue
        if feature == "TZCNT":
            result.informational[feature] += 1
            continue
        where = procedure.name if procedure is not None else symbol
        finding = Finding(feature, insn.group(1), where, text.strip())
        # The OSXSAVE review covers XGETBV(0), not every member of XSAVE.
        reviewed_instruction = feature != XSAVE or mnemonic.lower() == "xgetbv"
        if pdb is None:
            if symbol_allowed or (reviewed_instruction and feature in ELF_REVIEWED_PROCEDURES.get(symbol, ())):
                result.allowed[feature] += 1
            else:
                result.violations.append(finding)
        elif reviewed_instruction and _pdb_allows(feature, address, pdb.ranges):
            result.allowed[feature] += 1
        elif procedure is not None and any(feature in spec.features for spec in pdb.guards.values()):
            pending.append((len(buffered) - 1, feature, finding))
        else:
            result.violations.append(finding)
    if pdb is not None:
        flush()
    return result


def scan(tool: list[str], path: str, allow: list[re.Pattern[str]], pdb: PdbInfo | None = None) -> ScanResult:
    # Raw bytes are needed to tell EVEX from VEX. GNU objdump wraps them after 7
    # bytes unless told otherwise; llvm-objdump prints them on one line.
    command = tool + ["-d", "-C", path]
    if not os.path.basename(tool[0]).lower().startswith("llvm-objdump"):
        command.insert(-1, "--insn-width=15")
    with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors="replace") as proc:
        assert proc.stdout is not None
        result = scan_lines(path, proc.stdout, allow, pdb)
        stderr = proc.stderr.read() if proc.stderr else ""
    if proc.returncode != 0:
        raise RuntimeError(f"disassembly of {path} failed: {stderr.strip()}")
    if result.instructions == 0:
        raise RuntimeError(f"no instructions disassembled from {path}")
    return result


def report(result: ScanResult, max_report: int) -> None:
    counts = Counter(finding.feature for finding in result.violations)
    status = "FAIL" if result.violations else "OK"
    unknown = counts.get("undecodable", 0)
    print(f"{status} {result.path}: {result.instructions} instructions, "
          f"{len(result.violations) - unknown} above-floor, {unknown} undecodable")
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
            result = scan(tool, path, allow, _pdb_info(pdb, path, args.pdbutil) if pdb else None)
        except (RuntimeError, OSError, ValueError, struct.error) as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
        report(result, args.max_report)
        failed = failed or bool(result.violations)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
