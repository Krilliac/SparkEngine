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
* Guard dominance for code the MSVC headers or auto-vectorizer place inline in
  arbitrary procedures: the instruction is reachable from its procedure's
  entry, and unreachable once the edges implied by a reviewed guard compare are
  removed from that procedure's control-flow graph (see REVIEWED_MSVC_GUARDS).
* Out-of-line procedures named in REVIEWED_CALLER_GUARDED_PROCEDURES, when every
  code reference to them is such a guarded branch or call and no data section
  holds their address (re-proven on every scan).

Before classifying a PE procedure, tools/isa_code_map.py rebuilds its
instruction stream around the MSVC switch tables it can prove, so table bytes
are data rather than undecodable or bogus instructions. Bytes it cannot
explain stay in the stream and fail the scan. A reviewed CRT/STL exemption
(the vector_algorithms contributions, memcpy/memset, __isa_available_init
XGETBV, the inline _Avx2Wmem/__isa_available guards and the caller-guarded
LZCNT helper) is granted only when the PDB's sole observed MSVC toolset is the
reviewed 14.44.35207; otherwise the instruction is a violation and report()
emits a "re-review for this toolset" note. The engine's own reviewed procedure
(Spark::Detail::ReadXcr0) does not depend on the toolset.

Threat model and residual limit. The scans target compiler-generated MSVC code
built from this repository's own sources, not adversarial or hand-written
binaries. The scanner resolves direct branches, structural code pointers
(.pdata exception handlers, exports, base-relocation pointees, the guard-CF
table) and basic-block-local computed targets -- an immediate image address
materialized into a register (movabs/mov/lea, with constant add/sub/inc/dec and
the "mov RVA; add image base" idiom) that reaches a `jmp *reg` / `call *reg`, or
is stored while the procedure has an indirect branch. Interprocedural or
memory-carried computed targets in a fixed-base image (a code pointer passed in
through a register or loaded from memory, built across basic blocks) are not
resolved; within the threat model MSVC does not generate such control flow into
the middle of a switch table or past an ISA guard.

Coverage. The Windows Shipping scan covers every configured first-party image
target, each paired with its own build PDB. The Microsoft runtime DLLs CMake
copies into the package's redist/ directory have no build PDB and are not
scanned; they are Microsoft's dispatch-guarded runtime, outside this
repository's sources.

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
import tempfile
from collections import Counter
from dataclasses import dataclass, field, replace


def _load_sibling(name: str):
    """Load a module that lives next to this script (it also runs as a plain file)."""
    module_name = "spark_" + name
    if module_name in sys.modules:
        return sys.modules[module_name]
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), name + ".py")
    spec = importlib.util.spec_from_file_location(module_name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


code_map = _load_sibling("isa_code_map")
Instruction = code_map.Record
_split_operands = code_map.split_operands
_REG32 = code_map.REG32
_REG_FAMILY = code_map.REG_FAMILY

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
    switch_tables: int = 0
    table_bytes: int = 0
    padding_bytes: int = 0
    # Caller-guarded procedures: findings held back, and the references seen.
    deferred: dict = field(default_factory=dict)
    unguarded_references: Counter = field(default_factory=Counter)
    guarded_references: Counter = field(default_factory=Counter)
    covered: list = field(default_factory=list)  # (start, end) byte ranges classified
    toolsets: frozenset = frozenset()  # MSVC toolset versions seen in the PDB


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
    # True for a reviewed MSVC CRT/STL range (valid only for toolset 14.44.35207);
    # False for an engine-owned reviewed procedure (e.g. Spark::Detail::ReadXcr0),
    # whose review does not depend on the MSVC toolset.
    toolset_sensitive: bool = True


# The only toolset whose runtime sources were reviewed. Contribution ranges and
# the cpu_disp pair apply only to objects the PDB says came from its libraries.
REVIEWED_TOOLSET = "14.44.35207"
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
# behind "cmpl $0x6, __isa_available", which classify as AVX-512; the
# contribution does not cover them, the __isa_available >= 6 guard rule does.
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
class GuardRule:
    """Values of a reviewed guard that prove a set of features present."""

    predicate: tuple[tuple[int, int], ...]  # disjoint signed 32-bit intervals
    features: frozenset
    # When set, only these mnemonics are covered (for families such as AVX-512
    # whose subsets have their own CPUID bits).
    mnemonics: frozenset | None = None

    def covers(self, feature: str, mnemonic: str) -> bool:
        return feature in self.features and (self.mnemonics is None or mnemonic in self.mnemonics)


@dataclass(frozen=True)
class GuardSpec:
    """A reviewed global int whose value decides whether inline code may run."""

    rules: tuple[GuardRule, ...]

    @property
    def features(self) -> frozenset:
        return frozenset().union(*(rule.features for rule in self.rules))


# Windows SDK 10.0.26100.0 ucrt/wchar.h:207-214 declares _Avx2WmemEnabled with
# /alternatename to a selectany _Avx2WmemEnabledWeakValue = 0; wmemchr (:274) and
# wmemcmp (:401) take their AVX2 paths only when it is non-zero. With the /MD CRT
# only libucrt(d).lib defines the strong symbol, so these images resolve both
# names to the never-written zero. AVX/AVX2 only.
_WMEM_GUARD = GuardSpec((GuardRule(((INT32_MIN, -1), (1, INT32_MAX)), AVX_FEATURES),))
# Guards the MSVC headers read inline. An edge is removed only when every value
# that takes it satisfies the predicate, and only for the features listed.
REVIEWED_MSVC_GUARDS = {
    "_Avx2WmemEnabled": _WMEM_GUARD,
    "_Avx2WmemEnabledWeakValue": _WMEM_GUARD,
    "__isa_available": GuardSpec((
        # MSVC 14.44.35207 include/__msvc_bit_utils.hpp:115-127
        # (_Checked_x86_x64_countl_zero) uses lzcnt only when __isa_available >=
        # _Stl_isa_available_avx2 (5, __ISA_AVAILABLE_AVX2). The auto-vectorizer
        # guards its AVX2 loops the same way ("cmpl $0x5, __isa_available; jl
        # scalar", e.g. Sha256State::Finalize). vcruntime's __isa_available_init
        # (14.44.35207 msvcrt.lib cpu_disp.obj, disassembly reviewed) stores 5
        # only after CPUID.1:ECX OSXSAVE (bit 27) and AVX (bit 28), XGETBV(0) &
        # 6 == 6 (XMM+YMM state), and CPUID.7.0:EBX AVX2 (bit 5). Every AVX2 CPU
        # implements LZCNT (the STL's _Definitely_have_lzcnt). AVX/AVX2/LZCNT only.
        GuardRule(((5, INT32_MAX),), AVX_FEATURES | {"LZCNT"}),
        # The auto-vectorizer emits EVEX loops behind "cmpl $0x6, __isa_available;
        # jl scalar" (cgltf_calc_index_bound, the STL's __std_minmax_*). The same
        # procedure stores 6 only after the AVX2 conditions above, CPUID.7.0:EBX &
        # 0xD0030000 == 0xD0030000 (AVX512F bit 16, DQ 17, CD 28, BW 30, VL 31)
        # and XGETBV(0) & 0xE0 == 0xE0 (opmask, ZMM_Hi256, Hi16_ZMM state). Only
        # the observed instructions, each in AVX512F/VL, are covered; another
        # EVEX instruction, possibly from a subset with its own CPUID bit
        # (VBMI, VNNI, IFMA, ...), needs its own review.
        GuardRule(((6, INT32_MAX),), frozenset({"AVX-512"}), frozenset({"vpmaxuq", "vpminuq"})),
    )),
}


@dataclass(frozen=True)
class CallerGuarded:
    """A reviewed out-of-line procedure that runs only behind a guard in its callers."""

    features: frozenset
    mnemonics: frozenset  # the only above-floor instructions it may contain
    justification: str


# Each entry is re-proven on every scan, not trusted: the procedure must be one
# unambiguous PDB extent of exactly this name and hold no other above-floor
# instruction; every code reference to it must be a direct branch or call that
# a reviewed guard rule (REVIEWED_MSVC_GUARDS) dominates for these features;
# and its address must not appear in any non-executable section (no function
# pointer, vtable or /guard:cf entry). Otherwise its findings stay violations.
REVIEWED_CALLER_GUARDED_PROCEDURES = {
    "std::_Countl_zero_lzcnt<unsigned __int64>": CallerGuarded(
        frozenset({"LZCNT"}), frozenset({"lzcntq"}),
        "MSVC 14.44.35207 include/__msvc_bit_utils.hpp: _Checked_x86_x64_countl_zero tail-jumps here only "
        "when __isa_available >= 5 (cmpl $0x5, __isa_available; jge _Countl_zero_lzcnt), and every AVX2 "
        "CPU implements LZCNT; otherwise it takes _Countl_zero_bsr."),
}


@dataclass(frozen=True)
class Procedure:
    start: int
    end: int
    name: str
    ambiguous: bool  # overlaps another procedure's extent without matching it
    noreturn: bool = False  # every procedure record for these bytes carries the PDB noreturn flag


@dataclass
class PdbInfo:
    ranges: list[PdbRange]
    procedures: list[Procedure]
    guards: dict[int, GuardSpec]
    toolsets: frozenset = frozenset()

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
        self.noreturn = frozenset(proc.start for proc in self.procedures if proc.noreturn)
        self.caller_guarded = {proc.start: REVIEWED_CALLER_GUARDED_PROCEDURES[proc.name]
                               for proc in self.procedures
                               if proc.name in REVIEWED_CALLER_GUARDED_PROCEDURES and not proc.ambiguous}
        # Every reviewed MSVC CRT/STL exemption (vector_algorithms contributions,
        # memcpy/memset, __isa_available_init XGETBV, the inline _Avx2Wmem/
        # __isa_available guards and the caller-guarded LZCNT helper) was
        # validated against toolset 14.44.35207. Grant them only when that is the
        # PDB's sole observed toolset; otherwise the instruction is a violation
        # and report()'s "re-review for toolset X" note explains the red.
        self.reviewed_toolset = self.toolsets == frozenset({REVIEWED_TOOLSET})

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


def _pe_section_table(path: str) -> tuple[int, list[tuple[int, int, int, int, int]]]:
    """(image base, [(rva, virtual size, raw size, raw pointer, characteristics)])."""
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
    table = []
    section_table = optional + optional_size
    for index in range(section_count):
        offset = section_table + index * 40
        if offset + 40 > len(data):
            raise RuntimeError(f"{path} has a truncated PE section table")
        virtual_size, rva, raw_size, raw_pointer = struct.unpack_from("<IIII", data, offset + 8)
        table.append((rva, virtual_size, raw_size, raw_pointer, struct.unpack_from("<I", data, offset + 36)[0]))
    return image_base, table


def _pe_sections(path: str) -> tuple[int, list[tuple[int, int, bool]]]:
    image_base, table = _pe_section_table(path)
    sections = []
    for rva, virtual_size, raw_size, _, characteristics in table:
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
    toolsets = set()
    for module, library in libraries.values():
        for text in (module, library):
            toolsets.update(re.findall(r"\\msvc\\([0-9.]+)\\", _normalized(text)))
    return PdbInfo(contributions + ranges, procedures, guards, frozenset(toolsets))


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
    # Folded (/OPT:ICF) procedures share an extent; it is noreturn only when
    # every procedure record for it carries the flag.
    records = Counter()
    noreturn_records = Counter()
    awaiting_flags = None
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
            awaiting_flags = None
        if awaiting_flags is not None and "flags = " in line:
            if "noreturn" in line.split("flags = ", 1)[1].split(" | "):
                noreturn_records[awaiting_flags] += 1
            awaiting_flags = None
        addr_match = addr_re.search(line)
        if not addr_match or pending is None or module is None:
            continue
        section = int(addr_match.group(1), 10)
        offset = int(addr_match.group(2), 10)
        size = int(addr_match.group(3), 10)
        features = reviewed(pending)
        if features:
            start = _code_address(section, offset, size, image_base, sections, pending)
            ranges.append(PdbRange(start, start + size, pending, module, features,
                                   toolset_sensitive=(pending != SPARK_XSAVE_PROCEDURE)))
            awaiting_flags = (start, start + size)
            records[awaiting_flags] += 1
        elif 1 <= section <= len(sections) and size > 0:
            section_rva, section_size, executable = sections[section - 1]
            if executable and offset + size <= section_size:
                start = image_base + section_rva + offset
                extents.setdefault((start, start + size), set()).add(pending)
                awaiting_flags = (start, start + size)
                records[awaiting_flags] += 1
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
        key = (start, end)
        procedures.append(Procedure(start, end, min(extents[key]), overlaps,
                                    records[key] > 0 and noreturn_records[key] == records[key]))
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


def _pdb_allows(feature: str, address: int, ranges: list[PdbRange], reviewed_toolset: bool = True) -> bool:
    for item in ranges:
        if item.start <= address < item.end and feature in item.features:
            if item.toolset_sensitive and not reviewed_toolset:
                continue  # a CRT/STL range needs toolset 14.44.35207 provenance
            return True
    return False


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
                  guards: dict[int, GuardSpec], extent: tuple[int, int],
                  dispatch: dict[int, tuple[int, ...]] | None = None,
                  references: list[tuple[int, str, frozenset]] | None = None,
                  guarded_references: set[int] | None = None,
                  roots: set[int] | None = None) -> set[int]:
    """Return indices of candidate instructions that only reviewed guard edges reach.

    references are (index, feature, mnemonics) of branches and calls into a
    caller-guarded procedure; the index of each one that is guarded the same way
    (a jcc whose taken edge the guard removes, or a call/jmp only guard edges
    reach) is added to guarded_references.

    The CFG is instruction-level over one procedure: jcc -> target and
    fallthrough; jmp -> target (or leaves the procedure); ret/ud2/int3/hlt end a
    path; everything else falls through. A switch dispatch whose tables
    isa_code_map proved has an edge to each case. Any other indirect jump, or a
    branch into the middle of an instruction, makes the whole procedure fail
    closed. Code reached
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
                if is_jump and insn.address not in (dispatch or {}):
                    return set()
                for case in (dispatch or {}).get(insn.address, ()):
                    if case not in index_of:
                        return set()
                    edges.append((index_of[case], "taken"))
                    targets.add(index_of[case])
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

    removed_for: dict[GuardRule, set[tuple[int, str]]] = {}
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
            for rule in spec.rules:
                if implied and all(any(low >= p_low and high <= p_high for p_low, p_high in rule.predicate)
                                   for low, high in implied):
                    removed_for.setdefault(rule, set()).add((i, edge_kind))

    entry_roots = {0} if roots is None else set(roots) | {0}

    def reachable(removed: set[tuple[int, str]]) -> set[int]:
        seen, stack = set(entry_roots), list(entry_roots)
        while stack:
            node = stack.pop()
            for successor, kind in successors[node]:
                if (node, kind) not in removed and successor not in seen:
                    seen.add(successor)
                    stack.append(successor)
        return seen

    full = reachable(set())
    exempt = set()
    for rule, removed in removed_for.items():
        covered = [i for i, feature in candidates if rule.covers(feature, insns[i].mnemonic)]
        refs = [i for i, feature, mnemonics in references or ()
                if mnemonics and all(rule.covers(feature, mnemonic) for mnemonic in mnemonics)]
        if not covered and not refs:
            continue
        cut = reachable(removed)
        exempt.update(i for i in covered if i in full and i not in cut)
        for i in refs:
            if i in full and (i not in cut or (insns[i].mnemonic in _CONDITIONS and (i, "taken") in removed)):
                guarded_references.add(i)
    return exempt


# ---------------------------------------------------------------------------


def _base_mnemonic(mnemonic: str) -> str:
    """Strip an AT&T operand-size suffix (shlxq -> shlx) for the scalar sets."""
    match = SIZE_SUFFIX_RE.match(mnemonic)
    if match and match.group(1) in SCALAR_MNEMONICS:
        return match.group(1)
    return mnemonic


# VEX-encoded instructions whose CPUID feature is NOT AVX/AVX2 but a separate
# bit, so an AVX2 guard or AVX runtime range must never cover them. Matched by
# mnemonic prefix, ahead of the AVX/AVX2 allow-list.
_SEPARATE_VEX_FEATURES = (
    ("vaes", "VAES"),
    ("vpclmul", "VPCLMULQDQ"),
    ("vgf2p8", "GFNI"),
    ("vprot", "XOP"), ("vpcom", "XOP"), ("vpmacs", "XOP"), ("vpmadcs", "XOP"), ("vpperm", "XOP"),
    ("vpdp", "AVX-VNNI"),            # vpdpbusd/vpdpwssd and the VNNI-INT8/16 forms
    ("vpmadd52", "AVX-IFMA"),        # vpmadd52luq/huq
    ("vcvtne", "AVX-NE-CONVERT"),    # vcvtneps2bf16, vcvtne{e,o}{bf16,ph}2ps
    ("vbcstne", "AVX-NE-CONVERT"),   # vbcstnebf162ps/vbcstnesh2ps
    ("vsha", "SHA"),
)


def _vex_avx_allow_list():
    """Every VEX mnemonic llvm-objdump prints that AVX or AVX2 (not a separate
    CPUID bit) establishes. Anything VEX outside this set fails closed."""
    names = set()
    for base in ("add", "sub", "mul", "div", "min", "max", "sqrt"):
        names |= {"v" + base + s for s in ("ps", "pd", "ss", "sd")}
    for base in ("rcp", "rsqrt"):
        names |= {"v" + base + s for s in ("ps", "ss")}
    for base in ("and", "andn", "or", "xor"):
        names |= {"v" + base + s for s in ("ps", "pd")}
    for base in ("hadd", "hsub", "addsub"):
        names |= {"v" + base + s for s in ("ps", "pd")}
    names |= {"vcomis" + s for s in ("s", "d")} | {"vucomis" + s for s in ("s", "d")}
    names |= {"vmovmsk" + s for s in ("ps", "pd")}
    names |= {"vmov" + m for m in ("aps", "apd", "ups", "upd", "ss", "sd", "hps", "lps", "hpd", "lpd",
                                   "hlps", "lhps", "ddup", "shdup", "sldup", "ntps", "ntpd", "ntdq",
                                   "ntdqa", "d", "q", "dqa", "dqu", "mskb")}
    names |= {"vlddqu", "vmaskmovdqu", "vmovntdqa"}
    names |= {"vshuf" + s for s in ("ps", "pd")}
    names |= {"vunpck" + h + s for h in ("l", "h") for s in ("ps", "pd")}
    names |= {"vblend" + s for s in ("ps", "pd", "vps", "vpd", "w", "d")}
    names |= {"vperm2f128", "vperm2i128", "vpermilps", "vpermilpd", "vpermd", "vpermq", "vpermps", "vpermpd"}
    names |= {"vinsertf128", "vextractf128", "vinserti128", "vextracti128", "vinsertps", "vextractps"}
    names |= {"vbroadcast" + s for s in ("ss", "sd", "f128", "i128")}
    names |= {"vpbroadcast" + s for s in ("b", "w", "d", "q")}
    names |= {"vmaskmov" + s for s in ("ps", "pd")} | {"vpmaskmov" + s for s in ("d", "q")}
    names |= {"vround" + s for s in ("ps", "pd", "ss", "sd")}
    names |= {"vdp" + s for s in ("ps", "pd")} | {"vmpsadbw"}
    names |= {"vtest" + s for s in ("ps", "pd")} | {"vptest"}
    names |= {"vzeroupper", "vzeroall", "vldmxcsr", "vstmxcsr"}
    names |= {"vcvt" + c for c in ("dq2ps", "ps2dq", "tps2dq", "dq2pd", "pd2dq", "tpd2dq", "ps2pd", "pd2ps",
                                   "sd2ss", "ss2sd", "sd2si", "ss2si", "tsd2si", "tss2si", "si2sd", "si2ss")}
    for base in ("padd", "psub"):
        names |= {"vp" + base[1:] + s for s in ("b", "w", "d", "q")}
        names |= {"vp" + base[1:] + "s" + s for s in ("b", "w")} | {"vp" + base[1:] + "us" + s for s in ("b", "w")}
    names |= {"vpmullw", "vpmulld", "vpmulhw", "vpmulhuw", "vpmulhrsw", "vpmuldq", "vpmuludq",
              "vpmaddwd", "vpmaddubsw"}
    names |= {"vpavg" + s for s in ("b", "w")}
    names |= {"vpmin" + s + w for s in ("s", "u") for w in ("b", "w", "d")}
    names |= {"vpmax" + s + w for s in ("s", "u") for w in ("b", "w", "d")}
    names |= {"vpand", "vpandn", "vpor", "vpxor"}
    names |= {"vpcmpeq" + s for s in ("b", "w", "d", "q")} | {"vpcmpgt" + s for s in ("b", "w", "d", "q")}
    names |= {"vpsll" + s for s in ("w", "d", "q")} | {"vpsrl" + s for s in ("w", "d", "q")}
    names |= {"vpsra" + s for s in ("w", "d")} | {"vpsllv" + s for s in ("d", "q")}
    names |= {"vpsrlv" + s for s in ("d", "q")} | {"vpsravd", "vpslldq", "vpsrldq"}
    names |= {"vpsign" + s for s in ("b", "w", "d")}
    names |= {"vphadd" + s for s in ("w", "d", "sw")} | {"vphsub" + s for s in ("w", "d", "sw")}
    names |= {"vpabs" + s for s in ("b", "w", "d")} | {"vpsadbw", "vphminposuw"}
    names |= {"vpmovzx" + s for s in ("bw", "bd", "bq", "wd", "wq", "dq")}
    names |= {"vpmovsx" + s for s in ("bw", "bd", "bq", "wd", "wq", "dq")}
    names |= {"vpackss" + s for s in ("wb", "dw")} | {"vpackus" + s for s in ("wb", "dw")}
    names |= {"vpunpck" + h + s for h in ("l", "h") for s in ("bw", "wd", "dq", "qdq")}
    names |= {"vpshufb", "vpshufd", "vpshufhw", "vpshuflw"}
    names |= {"vpblendvb"}
    names |= {"vpinsr" + s for s in ("b", "w", "d", "q")} | {"vpextr" + s for s in ("b", "w", "d", "q")}
    names |= {"vpalignr", "vpmovmskb"}
    names |= {"vpgather" + s for s in ("dd", "qd", "dq", "qq")}
    names |= {"vgather" + s for s in ("dps", "qps", "dpd", "qpd")}
    names |= {"vpcmpestr" + s for s in ("i", "m")} | {"vpcmpistr" + s for s in ("i", "m")}
    return frozenset(names)


VEX_AVX_ALLOW_LIST = _vex_avx_allow_list()
# vcmp<cc>ps / vcmp<cc>pd / vcmp<cc>ss / vcmp<cc>sd carry a named condition
# (vcmpeqps, vcmpgt_oqpd, ...); their feature is AVX regardless of the condition.
_VEX_CMP_RE = re.compile(r"^vcmp[a-z_0-9]*(ps|pd|ss|sd)$")


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
            for prefix, feature in _SEPARATE_VEX_FEATURES:
                if mnemonic.startswith(prefix):
                    return feature
            if re.match(r"^vf(n)?m(add|sub)", mnemonic):
                return "FMA"
            if mnemonic in ("vcvtph2ps", "vcvtps2ph"):
                return "F16C"
            # Only VEX mnemonics AVX/AVX2 actually establishes may be excused by
            # an AVX2 guard. Anything else (a newer VEX extension with its own
            # CPUID bit, e.g. AVX-VNNI vpdpbusd) fails closed as its own feature.
            if mnemonic not in VEX_AVX_ALLOW_LIST and not _VEX_CMP_RE.match(mnemonic):
                return "AVX (unrecognized VEX)"
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


def _records(lines):
    """Yield (objdump symbol header, Record) for every disassembled instruction line."""
    symbol = "<unknown>"
    previous = None
    for line in _join_split_prefixes(lines):
        header = SYMBOL_RE.match(line)
        if header:
            symbol = header.group(2)
            continue
        insn = INSN_RE.match(line)
        if not insn:
            continued = CONTINUED_ANNOTATION_RE.match(line)
            if continued and previous and previous[1].annotation is None:
                previous = (previous[0], replace(previous[1], annotation=int(continued.group(1), 16)))
            continue
        if previous:
            yield previous
        raw = RAW_BYTES_RE.match(insn.group(2))
        evex, text = split_raw_bytes(insn.group(2))
        mnemonic, operands = split_instruction(text)
        undecodable = not mnemonic or mnemonic.startswith(("(", "<", "."))
        annotation = ANNOTATION_ADDRESS_RE.search(text)
        previous = (symbol, Instruction(int(insn.group(1), 16), len(raw.group(1).split()) if raw else 0,
                                        "<undecodable>" if undecodable else mnemonic.lower(), operands,
                                        int(annotation.group(1), 16) if annotation else None, evex, text.strip()))
    if previous:
        yield previous


@dataclass(frozen=True)
class Gap:
    """Executable bytes outside every PDB procedure: padding, or code with no S_GPROC32."""

    start: int
    end: int


def _region(pdb: PdbInfo, image, address: int):
    procedure = pdb.procedure_at(address)
    if procedure is not None:
        return procedure
    index = bisect.bisect_right(pdb._starts, address)
    start = pdb._max_ends[index - 1] if index else 0
    end = pdb._starts[index] if index < len(pdb._starts) else 1 << 64
    bounds = image.section_bounds(address) if image is not None else None
    if bounds is not None:
        start, end = max(start, bounds[0]), min(end, bounds[1])
    return Gap(start, end)


def scan_lines(path: str, lines, allow: list[re.Pattern[str]], pdb: PdbInfo | None = None,
               image=None, entries=None, collector=None) -> ScanResult:
    """Classify disassembly lines; PE findings go through the reviewed PDB mechanisms.

    With the image bytes (PE scans of real images), each procedure's instruction
    stream is first rebuilt around its proven switch tables (isa_code_map).
    """
    result = ScanResult(path=path, toolsets=pdb.toolsets if pdb is not None else frozenset())
    if pdb is None:
        for symbol, insn in _records(lines):
            _classify(result, insn, symbol, allow, None, None, [])
        return result
    region = None
    buffered: list[tuple[str, Instruction]] = []
    for symbol, insn in _records(lines):
        key = _region(pdb, image, insn.address)
        if key != region:
            _finish_region(result, pdb, image, region, buffered, entries, collector)
            region, buffered = key, []
        buffered.append((symbol, insn))
    _finish_region(result, pdb, image, region, buffered, entries, collector)
    _settle_caller_guarded(result, pdb, image, entries)
    return result


def _settle_caller_guarded(result: ScanResult, pdb: PdbInfo, image, entries=None) -> None:
    for start, findings in result.deferred.items():
        entry = pdb.caller_guarded[start]
        reasons = []
        if entries is not None and entries.non_branch_reference(start):
            reasons.append("its address is taken or reached indirectly")
        if result.unguarded_references[start]:
            reasons.append(f"{result.unguarded_references[start]} unguarded reference(s)")
        if image is None:
            reasons.append("no image bytes to rule out data references")
        elif (image.data_contains(struct.pack("<Q", start))
              or image.data_contains(struct.pack("<I", start - image.image_base))):
            reasons.append("its address appears in a data section")
        if any(finding.text.split()[0].lower() not in entry.mnemonics for finding in findings):
            reasons.append("an unreviewed instruction")
        for finding in findings:
            if reasons:
                note = f"{finding.symbol} (caller guard not proven: {', '.join(reasons)})"
                result.violations.append(replace(finding, symbol=note))
            else:
                result.allowed[finding.feature] += 1


def _finish_region(result: ScanResult, pdb: PdbInfo, image, region, buffered, entries=None,
                   collector=None) -> None:
    if not buffered:
        return
    insns = [insn for _, insn in buffered]
    symbols = {insn.address: symbol for symbol, insn in buffered}
    procedure = region if isinstance(region, Procedure) else None
    dispatch: dict[int, tuple[int, ...]] = {}
    if image is not None and all(insn.size > 0 for insn in insns):
        if procedure is not None and not procedure.ambiguous:
            mapped = code_map.map_procedure(insns, procedure.start, procedure.end, image, pdb.noreturn)
            insns, dispatch = mapped.records, mapped.dispatch_targets
            result.switch_tables += sum(table.kind == "jump" for table in mapped.tables)
            result.table_bytes += sum(table.end - table.start for table in mapped.tables)
            result.padding_bytes += mapped.padding
            result.covered.append((procedure.start, procedure.end))
        elif isinstance(region, Gap):
            insns = code_map.map_gap(insns, region.start, region.end, image)
            result.covered.append((region.start, region.end))
        else:  # an ambiguous (folded) procedure: classified as disassembled
            result.covered.extend((insn.address, insn.address + insn.size) for insn in insns)
    elif image is not None:
        # A record without raw bytes (size 0) cannot be placed; classify as-is
        # and let _verify_coverage fail on the gap it leaves.
        result.covered.extend((insn.address, insn.address + insn.size) for insn in insns)
    if collector is not None:
        collector.record(insns)
    pending: list[tuple[int, str, Finding]] = []
    for index, insn in enumerate(insns):
        _classify(result, insn, symbols.get(insn.address, "<unknown>"), [], pdb, procedure,
                  pending, index)
    references = []
    for index, insn in enumerate(insns):
        target = code_map.direct_target(insn.mnemonic, insn.operands)
        if target in pdb.caller_guarded:
            entry = pdb.caller_guarded[target]
            references += [(index, feature, entry.mnemonics) for feature in entry.features]
        if insn.annotation in pdb.caller_guarded:
            result.unguarded_references[insn.annotation] += 1  # the address is taken
    exempt: set[int] = set()
    guarded: set[int] = set()
    analysable = procedure is not None and not procedure.ambiguous and insns[0].address == procedure.start
    if (pending or references) and analysable:
        candidates = [(index, feature) for index, feature, _ in pending]
        address_of = {insn.address: i for i, insn in enumerate(insns)}
        roots = {0}
        if entries is not None:
            roots |= {address_of[a] for a in entries.alternate_roots(pdb, procedure.start, procedure.end)
                      if a in address_of}
        exempt = _guard_exempt(insns, candidates, pdb.guards, (procedure.start, procedure.end), dispatch,
                               references, guarded, roots)
    for index, _, _ in references:
        target = code_map.direct_target(insns[index].mnemonic, insns[index].operands)
        if index in guarded:
            result.guarded_references[target] += 1
        else:
            result.unguarded_references[target] += 1
    held = (procedure.start if procedure is not None and pdb.reviewed_toolset
            and procedure.start in pdb.caller_guarded else None)
    for index, feature, finding in pending:
        if index in exempt:
            result.allowed[feature] += 1
        elif held is not None and feature in pdb.caller_guarded[held].features:
            result.deferred.setdefault(held, []).append(finding)
        else:
            result.violations.append(finding)


def _classify(result: ScanResult, insn: Instruction, symbol: str, allow: list[re.Pattern[str]],
              pdb: PdbInfo | None, procedure: Procedure | None, pending: list, index: int = 0) -> None:
    where = procedure.name if procedure is not None else symbol
    address = f"{insn.address:x}"
    if insn.mnemonic == "<undecodable>":
        # This can be data in .text or an instruction unknown to the tool.
        # Neither is a proof of floor safety; never silently discard it.
        result.violations.append(Finding("undecodable", address, where, insn.text))
        return
    result.instructions += 1
    feature = classify(insn.mnemonic, insn.operands, insn.evex)
    if feature is None:
        return
    if feature == "TZCNT":
        result.informational[feature] += 1
        return
    finding = Finding(feature, address, where, insn.text)
    # The OSXSAVE review covers XGETBV(0), not every member of XSAVE.
    reviewed_instruction = feature != XSAVE or insn.mnemonic == "xgetbv"
    if pdb is None:
        symbol_allowed = any(pattern.search(symbol) for pattern in allow)
        if symbol_allowed or (reviewed_instruction and feature in ELF_REVIEWED_PROCEDURES.get(symbol, ())):
            result.allowed[feature] += 1
        else:
            result.violations.append(finding)
    elif reviewed_instruction and _pdb_allows(feature, insn.address, pdb.ranges, pdb.reviewed_toolset):
        result.allowed[feature] += 1
    elif (procedure is not None and pdb.reviewed_toolset
          and any(feature in spec.features for spec in pdb.guards.values())):
        pending.append((index, feature, finding))
    else:
        result.violations.append(finding)


def _disassemble_command(tool: list[str], path: str, *extra: str) -> list[str]:
    # Raw bytes are needed to tell EVEX from VEX and to size each instruction.
    # GNU objdump wraps them after 7 bytes unless told otherwise; llvm-objdump
    # prints them on one line.
    command = tool + ["-d", "-C", *extra, path]
    if not os.path.basename(tool[0]).lower().startswith("llvm-objdump"):
        command.insert(-1, "--insn-width=15")
    return command


def _image_bytes(tool: list[str], path: str):
    image_base, table = _pe_section_table(path)

    def redecode(start: int, stop: int) -> list[Instruction]:
        proc = subprocess.run(_disassemble_command(tool, path, f"--start-address={start:#x}",
                                                   f"--stop-address={stop:#x}"),
                              capture_output=True, text=True, errors="replace", check=False)
        if proc.returncode != 0:
            raise RuntimeError(f"re-disassembly of {path} failed: {proc.stderr.strip()}")
        return [insn for _, insn in _records(proc.stdout.splitlines())]

    def ranges(executable: bool) -> list[tuple[int, int, int]]:
        return [(image_base + rva, min(virtual_size, raw_size), raw_pointer)
                for rva, virtual_size, raw_size, raw_pointer, characteristics in table
                if bool(characteristics & 0x20000000) == executable and min(virtual_size, raw_size)]

    return code_map.ImageBytes(path, image_base, ranges(True), redecode, ranges(False))


_IMMEDIATE_RE = re.compile(r"\$(?:0x)?([0-9a-fA-F]+)\b")


def _pe_structural_entries(path: str) -> set[int]:
    """Reliable code entry VAs taken from the PE structures themselves.

    Exported function RVAs, guard-CF valid indirect-call targets, exception-
    handler RVAs from .pdata unwind info, and absolute code pointers named by
    base relocations. A .pdata BeginAddress is deliberately NOT included: MSVC
    gives a compiler-placed jump table its own RUNTIME_FUNCTION, so a begin can
    coincide with table bytes and must not reject a proven table. A malformed
    standard directory fails the scan.
    """
    with open(path, "rb") as handle:
        data = handle.read()

    def u16(off: int) -> int:
        return struct.unpack_from("<H", data, off)[0]

    def u32(off: int) -> int:
        return struct.unpack_from("<I", data, off)[0]

    def u64(off: int) -> int:
        return struct.unpack_from("<Q", data, off)[0]

    pe = u32(0x3C)
    optional = pe + 24
    if u16(optional) != 0x20B:
        raise RuntimeError(f"{path} is not a PE32+ image")
    image_base = u64(optional + 24)
    rva_count = u32(optional + 108)
    directory = optional + 112
    section_count = u16(pe + 6)
    optional_size = u16(pe + 20)
    section_table = pe + 24 + optional_size
    sections = []  # (rva, virtual size, raw size, raw pointer, executable)
    for i in range(section_count):
        o = section_table + i * 40
        vs, rva, rs, rp = struct.unpack_from("<IIII", data, o + 8)
        ch = u32(o + 36)
        sections.append((rva, vs, rs, rp, bool(ch & 0x20000000)))

    def directory_entry(index: int) -> tuple[int, int]:
        if index >= rva_count:
            return 0, 0
        return u32(directory + index * 8), u32(directory + index * 8 + 4)

    def offset_of(rva: int, length: int) -> int | None:
        for srva, vs, rs, rp, _ in sections:
            if srva <= rva and rva + length <= srva + rs:
                return rp + (rva - srva)
        return None

    def in_exec(va: int) -> bool:
        rva = va - image_base
        return any(executable and srva <= rva < srva + min(vs, rs)
                   for srva, vs, rs, rp, executable in sections)

    other: set[int] = set()

    # .pdata: RUNTIME_FUNCTION[] {BeginRVA, EndRVA, UnwindRVA}; take the handlers.
    pdata_rva, pdata_size = directory_entry(3)
    if pdata_rva:
        base = offset_of(pdata_rva, pdata_size)
        if base is None:
            raise RuntimeError(f"{path} .pdata is outside the file image")
        for o in range(base, base + (pdata_size // 12) * 12, 12):
            _begin, _end, unwind = struct.unpack_from("<III", data, o)
            handler = _unwind_handler(data, offset_of, unwind)
            if handler is not None and in_exec(image_base + handler):
                other.add(image_base + handler)

    # Export address table: each slot is a function RVA (skip forwarders).
    export_rva, export_size = directory_entry(0)
    if export_rva:
        base = offset_of(export_rva, 40)
        if base is None:
            raise RuntimeError(f"{path} export directory is outside the file image")
        count = u32(base + 20)
        functions = u32(base + 28)
        table = offset_of(functions, count * 4) if count else None
        for i in range(count if table is not None else 0):
            rva = u32(table + i * 4)
            if rva and not (export_rva <= rva < export_rva + export_size) and in_exec(image_base + rva):
                other.add(image_base + rva)

    # Base relocations: DIR64 slots hold absolute pointers; a pointee in an
    # executable section is an address-taken code location.
    reloc_rva, reloc_size = directory_entry(5)
    if reloc_rva:
        base = offset_of(reloc_rva, reloc_size)
        if base is None:
            raise RuntimeError(f"{path} base relocations are outside the file image")
        cursor = base
        while cursor < base + reloc_size:
            page = u32(cursor)
            block = u32(cursor + 4)
            if block < 8:
                break
            for j in range(cursor + 8, cursor + block, 2):
                entry = u16(j)
                if (entry >> 12) == 10:  # IMAGE_REL_BASED_DIR64
                    slot = offset_of(page + (entry & 0xFFF), 8)
                    if slot is not None and in_exec(u64(slot)):
                        other.add(u64(slot))
            cursor += block

    # Guard-CF valid indirect-call targets, when present.
    config_rva, config_size = directory_entry(10)
    if config_rva and config_size >= 0x94:
        base = offset_of(config_rva, 0x94)
        if base is not None:
            table_va = u64(base + 0x80)
            table_count = u64(base + 0x88)
            stride = (u32(base + 0x90) & 0xF0000000) >> 28
            entry_size = 4 + stride
            table = offset_of(table_va - image_base, table_count * entry_size) if table_va and table_count else None
            for i in range(table_count if table is not None else 0):
                rva = u32(table + i * entry_size)
                if in_exec(image_base + rva):
                    other.add(image_base + rva)

    return other


def _unwind_handler(data: bytes, offset_of, unwind_rva: int) -> int | None:
    """The exception-handler RVA of an UNWIND_INFO, following one CHAININFO link."""
    for _ in range(8):
        base = offset_of(unwind_rva, 4)
        if base is None:
            return None
        flags = data[base] >> 3
        codes = data[base + 2]
        tail = base + 4 + ((codes + 1) & ~1) * 2
        if flags & 0x4:  # UNW_FLAG_CHAININFO -> a RUNTIME_FUNCTION, recurse
            if offset_of(unwind_rva + (tail - base), 12) is None:
                return None
            unwind_rva = struct.unpack_from("<I", data, tail + 8)[0]
            continue
        if flags & 0x3:  # UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER
            return struct.unpack_from("<I", data, tail)[0]
        return None
    return None


@dataclass
class _ImageEntries:
    image_base: int
    branch_sources: dict  # target VA -> set of source VAs (direct jmp/jcc/call)
    computed_targets: set  # VAs a tracked immediate reaches via jmp/call *reg (or a stored pointer)
    other: set             # reliable non-branch code entries: handlers, exports, reloc, guard-CF

    def entered(self) -> set:
        # Addresses reached as code: a direct branch/call anywhere in the image,
        # the reliable structural entries, and a computed indirect target whose
        # address a basic block materialized (not every immediate -- a data
        # constant that merely coincides with code bytes is handled, per table,
        # by ImageBytes.table_masks_code instead).
        return set(self.branch_sources) | self.other | self.computed_targets

    def alternate_roots(self, pdb, start: int, end: int) -> set:
        """VAs strictly inside (start, end) reached other than by an intra-procedure edge:
        a reliable non-branch entry, a computed indirect target, or a branch
        target whose source is outside the procedure."""
        roots = {a for a in self.other | self.computed_targets if start < a < end}
        for target, sources in self.branch_sources.items():
            if start < target < end and any(not (start <= s < end) for s in sources):
                roots.add(target)
        return roots

    def non_branch_reference(self, address: int) -> bool:
        """True when address is reached indirectly (computed call/jmp, exported, relocated, guard-CF)."""
        return address in self.computed_targets or address in self.other


class _EntryCollector:
    """Gathers code entry points from rebuilt instruction streams.

    Run over the tables-as-data streams of a first scan pass, never the raw
    linear disassembly (there table bytes decode as bogus branches that would
    point garbage targets into other tables). Besides direct branch targets it
    follows an immediate image address materialized into a register within a
    basic block (movabs/mov/lea, with constant add/sub/inc/dec adjustments and
    the "mov RVA; add imagebase" idiom) to the `jmp *reg` / `call *reg` that uses
    it, or to a store of it when the procedure has an indirect jump/call. Those
    targets are computed entries; every materialized address is also recorded so
    ImageBytes.table_masks_code can test a constant that lands in table bytes.
    """

    def __init__(self, image):
        self.image = image
        self.branch_sources: dict[int, set[int]] = {}
        self.computed_targets: set[int] = set()
        self.immediate_addresses: set[int] = set()

    def record(self, insns) -> None:
        image = self.image
        index_of = {insn.address: i for i, insn in enumerate(insns)}
        leaders = {index_of[t] for insn in insns
                   if (t := code_map.direct_target(insn.mnemonic, insn.operands)) is not None and t in index_of
                   and insn.mnemonic not in ("call", "callq")}
        has_indirect = any((code_map.is_jump(insn.mnemonic) or insn.mnemonic in ("call", "callq"))
                           and insn.operands.startswith("*") for insn in insns)
        addr: dict[str, int] = {}     # reg family -> absolute image VA it holds
        rva: dict[str, int] = {}      # reg family -> an RVA awaiting + image base
        is_base: dict[str, bool] = {}  # reg family -> holds the image base
        for i, insn in enumerate(insns):
            if i in leaders:
                addr.clear(); rva.clear(); is_base.clear()
            operands = code_map.split_operands(insn.operands)
            target = code_map.direct_target(insn.mnemonic, insn.operands)
            if target is not None and image.section_bounds(target) is not None:
                self.branch_sources.setdefault(target, set()).add(insn.address)
            # Use of a tracked register as an indirect jump/call target.
            if (code_map.is_jump(insn.mnemonic) or insn.mnemonic in ("call", "callq")) \
                    and insn.operands.startswith("*%"):
                reg = code_map.family(insn.operands[1:])
                value = self._value(reg, addr, rva, image)
                if value is not None:
                    self.computed_targets.add(value)
            self._update(insn, operands, addr, rva, is_base, image, has_indirect)

    def _value(self, reg, addr, rva, image):
        if reg in addr:
            return addr[reg]
        if reg in rva and image.section_bounds(image.image_base + rva[reg]) is not None:
            return image.image_base + rva[reg]
        return None

    def _update(self, insn, operands, addr, rva, is_base, image, has_indirect) -> None:
        mnemonic = insn.mnemonic
        base = image.image_base
        # Materialize an image address into a register.
        if mnemonic.startswith(("mov", "lea")) and len(operands) == 2 and code_map.family(operands[1]):
            dest = code_map.family(operands[1])
            self._clear(dest, addr, rva, is_base)
            source = operands[0]
            if mnemonic.startswith("lea") and insn.annotation is not None:
                if insn.annotation == base:
                    is_base[dest] = True
                elif image.section_bounds(insn.annotation) is not None:
                    addr[dest] = insn.annotation
                    self.immediate_addresses.add(insn.annotation)
            elif source.startswith("$"):
                value = int(source[1:], 0) & 0xFFFFFFFFFFFFFFFF
                if image.section_bounds(value) is not None:
                    addr[dest] = value
                    self.immediate_addresses.add(value)
                elif value == base:
                    is_base[dest] = True
                else:
                    rva[dest] = value
                    if image.section_bounds(base + value) is not None:
                        self.immediate_addresses.add(base + value)
            elif (source.startswith("%") and mnemonic.startswith("mov")
                  and code_map.family(source) in addr):
                addr[dest] = addr[code_map.family(source)]  # reg-to-reg copy of an address
            return
        # Adjust a tracked register by a constant, or fold in the image base.
        if mnemonic.startswith(("add", "sub", "inc", "dec")):
            if mnemonic.startswith(("inc", "dec")) and len(operands) == 1 and code_map.family(operands[0]):
                self._adjust(code_map.family(operands[0]), 1 if mnemonic.startswith("inc") else -1,
                             addr, rva, image)
                return
            if len(operands) == 2 and code_map.family(operands[1]):
                dest = code_map.family(operands[1])
                if operands[0].startswith("$"):
                    delta = int(operands[0][1:], 0)
                    self._adjust(dest, delta if mnemonic.startswith("add") else -delta, addr, rva, image)
                    return
                if (operands[0].startswith("%") and mnemonic.startswith("add")
                        and is_base.get(code_map.family(operands[0])) and dest in rva):
                    addr[dest] = base + rva.pop(dest)
                    self.immediate_addresses.add(addr[dest])
                    return
                self._clear(dest, addr, rva, is_base)
                return
        # A store of a tracked address to memory, in a procedure with an indirect branch.
        if mnemonic.startswith("mov") and len(operands) == 2 and not code_map.family(operands[1]) \
                and code_map.family(operands[0]) in addr and has_indirect:
            self.computed_targets.add(addr[code_map.family(operands[0])])
            return
        # Any other write clears the written registers.
        written = code_map.written_families(insn)
        if written:
            for family in written:
                self._clear(family, addr, rva, is_base)

    @staticmethod
    def _clear(family, addr, rva, is_base) -> None:
        addr.pop(family, None)
        rva.pop(family, None)
        is_base.pop(family, None)

    def _adjust(self, family, delta, addr, rva, image) -> None:
        if family in addr:
            addr[family] += delta
            if image.section_bounds(addr[family]) is not None:
                self.immediate_addresses.add(addr[family])
        elif family in rva:
            rva[family] += delta


def _verify_coverage(result: ScanResult, image) -> None:
    """Every executable file-backed byte must be classified exactly once (no gap)."""
    covered = sorted(result.covered)
    for section_start, section_end in image.exec_ranges():
        cursor = section_start
        for start, end in covered:
            if end <= cursor or start >= section_end:
                continue
            if start > cursor:
                raise RuntimeError(f"{result.path}: executable bytes {cursor:#x}-{start:#x} were not classified "
                                   f"(disassembly gap or a record without raw bytes)")
            cursor = max(cursor, end)
            if cursor >= section_end:
                break
        if cursor < section_end:
            raise RuntimeError(f"{result.path}: executable bytes {cursor:#x}-{section_end:#x} were not classified "
                               f"(disassembly gap or a record without raw bytes)")


def scan(tool: list[str], path: str, allow: list[re.Pattern[str]], pdb: PdbInfo | None = None) -> ScanResult:
    command = _disassemble_command(tool, path)
    if pdb is None:
        with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                              errors="replace") as proc:
            assert proc.stdout is not None
            result = scan_lines(path, proc.stdout, allow, None, None)
            stderr = proc.stderr.read() if proc.stderr else ""
        if proc.returncode != 0:
            raise RuntimeError(f"disassembly of {path} failed: {stderr.strip()}")
        if result.instructions == 0:
            raise RuntimeError(f"no instructions disassembled from {path}")
        return result
    # PE: disassemble once to a temp file and read it twice.
    #  Pass 1 rebuilds each procedure's instruction stream (proving switch tables
    #   with the disassembly-independent structural entries only), and collects
    #   the real branch and address-taken targets from those tables-as-data
    #   streams.
    #  Pass 2 classifies with the full entry set, so a table whose bytes any real
    #   branch or address-taken reference enters is rejected (its bytes stay
    #   classified), and an alternate entry past a guard counts.
    image = _image_bytes(tool, path)
    other = _pe_structural_entries(path)
    base = image.image_base
    handle, scratch = tempfile.mkstemp(suffix=".dis")
    try:
        with os.fdopen(handle, "w", encoding="utf-8", errors="replace") as out:
            proc = subprocess.run(command, stdout=out, stderr=subprocess.PIPE, text=True, errors="replace",
                                  check=False)
        if proc.returncode != 0:
            raise RuntimeError(f"disassembly of {path} failed: {proc.stderr.strip()}")
        structural = _ImageEntries(base, {}, set(), other)
        image.set_entered(structural.entered())
        collector = _EntryCollector(image)
        with open(scratch, encoding="utf-8", errors="replace") as lines:
            scan_lines(path, lines, allow, pdb, image, structural, collector)
        entries = _ImageEntries(base, collector.branch_sources, collector.computed_targets, other)
        image.set_entered(entries.entered())
        image.set_immediates(collector.immediate_addresses, classify)
        with open(scratch, encoding="utf-8", errors="replace") as lines:
            result = scan_lines(path, lines, allow, pdb, image, entries)
    finally:
        os.unlink(scratch)
    if result.instructions == 0:
        raise RuntimeError(f"no instructions disassembled from {path}")
    _verify_coverage(result, image)
    return result


def report(result: ScanResult, max_report: int) -> None:
    counts = Counter(finding.feature for finding in result.violations)
    status = "FAIL" if result.violations else "OK"
    unknown = counts.get("undecodable", 0)
    print(f"{status} {result.path}: {result.instructions} instructions, "
          f"{len(result.violations) - unknown} above-floor, {unknown} undecodable")
    if result.violations and result.toolsets != frozenset({REVIEWED_TOOLSET}):
        seen = ", ".join(sorted(result.toolsets)) or "none found"
        print(f"  note: the reviewed CRT/STL exemptions are pinned to MSVC toolset {REVIEWED_TOOLSET}; this PDB's "
              f"toolset provenance is {{{seen}}}, so those exemptions were NOT applied -- re-review for this "
              f"toolset before trusting or dismissing these findings")
    if result.switch_tables or result.padding_bytes:
        print(f"  data in code: {result.switch_tables} proven switch tables ({result.table_bytes} bytes), "
              f"{result.padding_bytes} nop/int3 padding bytes after them")
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
