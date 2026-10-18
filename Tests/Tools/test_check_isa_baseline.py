#!/usr/bin/env python3
"""BLD-100 / OD-04: tools/check_isa_baseline.py rejects above-floor instructions.

The stable-v1 CPU floor is x86-64 with SSE4.2 and POPCNT. These tests compile
real fixtures and run the checker on the linked or assembled result:

* an ELF object built at the floor passes, while the same source built with
  -mavx2 -mfma, -mbmi/-mbmi2, -mlzcnt or -mf16c fails and names the feature;
* the legacy-encoded extensions above the floor (AES-NI, PCLMULQDQ, SHA-NI,
  GFNI, RDRAND, RDSEED, ADX, XSAVE, TSX) fail as well; XGETBV is exempt only
  inside a procedure named exactly Spark::Detail::ReadXcr0, and only XSAVE;
* the TZCNT encoding GCC/Clang emit at the floor for ctz (REP BSF) is
  reported as informational, not as a violation;
* --allow-symbol exempts only the named CPUID-dispatched function;
* a PE/COFF DLL linked with lld-link is disassembled and judged the same way;
  AVX2 in a reviewed MSVC vector_algorithms contribution (exact module path and
  MSVC 14.44.35207 library) is exempt, a neighbouring module, another toolset
  or another ISA family is not;
* a non-x86 image, a missing file and an empty file are errors (exit 2),
  never a vacuous pass;
* the instruction classifier does not mistake SSE mnemonics (pextrd, andnps)
  or disassembler annotations for above-floor instructions.
"""

from __future__ import annotations

import importlib.util
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CHECKER = REPO_ROOT / "tools" / "check_isa_baseline.py"

X86_HOST = platform.machine().lower() in {"x86_64", "amd64"}
CC = shutil.which("gcc") or shutil.which("clang")
CXX = shutil.which("g++") or shutil.which("clang++")
CLANG = shutil.which("clang")
LLD_LINK = shutil.which("lld-link")
LLVM_LIB = shutil.which("llvm-lib")
DISASSEMBLER = shutil.which("objdump") or shutil.which("llvm-objdump")

FIXTURE_SOURCE = """
unsigned CountTrailing(unsigned value) { return value ? __builtin_ctz(value) : 32u; }
unsigned long long ClearLowest(unsigned long long value) { return value & (value - 1); }
unsigned long long AndNot(unsigned long long a, unsigned long long b) { return ~a & b; }
unsigned long long ShiftLeft(unsigned long long value, unsigned count) { return value << (count & 63); }
unsigned CountLeading(unsigned value) { return value ? __builtin_clz(value) : 32u; }
void MultiplyAdd(float* out, const float* a, const float* b, int count)
{
    for (int i = 0; i < count; ++i)
        out[i] = out[i] * a[i] + b[i];
}
float HalfToFloat(unsigned short half) { return _cvtsh_ss(half); }
"""

HALF_INCLUDE = "#include <immintrin.h>\n"

# The same inline asm Utils/MultiISA.h uses on GCC/Clang.
XGETBV_SOURCE = """__attribute__((noinline)) unsigned long long {name}(void)
{{
    unsigned low, high;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    return ((unsigned long long)high << 32) | low;
}}
"""


def _load_checker():
    spec = importlib.util.spec_from_file_location("check_isa_baseline", CHECKER)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


checker = _load_checker()


def _run_checker(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-B", str(CHECKER), *args], capture_output=True, text=True, check=False, timeout=120
    )


class ClassifierTests(unittest.TestCase):
    def _classify(self, text: str) -> str | None:
        mnemonic, operands = checker.split_instruction(text)
        return checker.classify(mnemonic, operands)

    def test_floor_instructions_pass(self) -> None:
        for text in (
            "popcnt %rdi,%rax",
            "crc32q %rsi,%rax",
            "pextrd $0x1,%xmm0,%eax",
            "pextrq $0x1,%xmm0,%rax",
            "andnps %xmm1,%xmm0",
            "pshufb %xmm1,%xmm0",
            "rep stos %rax,%es:(%rdi)",
            "bsr %edi,%eax",
            "adc %rsi,%rax",
            "rdtsc",
            "shl %cl,%rax",
            "call 1030 <vfmadd_wrapper> # 0x1030",
            "cpuid",
            "rdtscp",
            "pause",
            "prefetcht0 (%rax)",
            "clflush (%rax)",
            "movnti %eax,(%rdx)",
            "movntdq %xmm0,(%rax)",
            "insertps $0x10,%xmm1,%xmm0",
            "extractps $0x1,%xmm0,%eax",
            "pmulhrsw %xmm1,%xmm0",
            "test %eax,%eax",
            "xchg %eax,%ebx",
            "xorps %xmm0,%xmm0",
            "xacquire lock incl (%rax)",
        ):
            with self.subTest(text=text):
                self.assertIsNone(self._classify(text))

    def test_above_floor_instructions_are_named(self) -> None:
        cases = {
            "vmovss (%rdi),%xmm0": "AVX (VEX)",
            "vzeroupper": "AVX (VEX)",
            "vmovups (%rdx),%ymm0": "AVX/AVX2 (ymm)",
            "vfmadd231ps %ymm2,%ymm1,%ymm0": "FMA",
            "vfnmsub213ss %xmm2,%xmm1,%xmm0": "FMA",
            "vcvtph2ps %xmm0,%xmm0": "F16C",
            "vaddps %zmm1,%zmm2,%zmm3": "AVX-512",
            "vaddps %xmm1,%xmm2,%xmm3{%k1}": "AVX-512",
            "kmovw %k1,%eax": "AVX-512",
            "andn %rsi,%rdi,%rax": "BMI1",
            "blsr %rdi,%rax": "BMI1",
            "shlxq %rsi,%rdi,%rax": "BMI2",
            "shlx %rsi,%rdi,%rax": "BMI2",
            "lzcnt %edi,%eax": "LZCNT",
            "lzcntl\t%edi, %eax": "LZCNT",
            "movbe (%rdi),%eax": "MOVBE",
            "aesenc %xmm1,%xmm0": "AES-NI",
            "aeskeygenassist $0x1,%xmm1,%xmm0": "AES-NI",
            "pclmulqdq $0x11,%xmm1,%xmm0": "PCLMULQDQ",
            "pclmullqlqdq %xmm1,%xmm0": "PCLMULQDQ",
            "sha256rnds2 %xmm0,%xmm1,%xmm2": "SHA",
            "sha1msg1 %xmm1,%xmm0": "SHA",
            "gf2p8mulb %xmm1,%xmm0": "GFNI",
            "rdrand %eax": "RDRAND",
            "rdseed %rax": "RDSEED",
            "adcx %rsi,%rax": "ADX",
            "adoxq %rsi,%rax": "ADX",
            "vaesenc %xmm2,%xmm1,%xmm0": "VAES",
        }
        for text, feature in cases.items():
            with self.subTest(text=text):
                self.assertEqual(self._classify(text), feature)

    def test_non_v2_scalar_families_are_named(self) -> None:
        # classify() returned None for all of these before XSAVE/TSX/... were added.
        cases = {
            "xgetbv": "XSAVE",
            "xsetbv": "XSAVE",
            "xsaveopt (%rax)": "XSAVE",
            "xsaveopt64 (%rax)": "XSAVE",
            "xsavec (%rax)": "XSAVE",
            "xrstor64 (%rax)": "XSAVE",
            "xrstors (%rax)": "XSAVE",
            "xbegin 0x1234": "TSX",
            "xend": "TSX",
            "xabort $0xff": "TSX",
            "xtest": "TSX",
            "rdfsbase %rax": "FSGSBASE",
            "wrgsbaseq %rax": "FSGSBASE",
            "extrq $0x8,$0x0,%xmm0": "SSE4a",
            "insertq %xmm1,%xmm0": "SSE4a",
            "movntsd %xmm0,(%rax)": "SSE4a",
            "femms": "3DNow!",
            "pfadd %mm1,%mm0": "3DNow!",
            "pi2fd %mm1,%mm0": "3DNow!",
            "pf2id %mm1,%mm0": "3DNow!",
            "clflushopt (%rax)": "CLFLUSHOPT",
            "clwb (%rax)": "CLWB",
            "rdpid %rax": "RDPID",
            "umwait %ecx": "WAITPKG",
            "tpause %ecx": "WAITPKG",
            "movdiri %eax,(%rdx)": "MOVDIRI",
            "movdir64b (%rax),%rdx": "MOVDIR64B",
            "serialize": "SERIALIZE",
            "rdpkru": "PKU",
            "ldtilecfg (%rax)": "AMX",
            "tileloadd (%rax,%rcx,1),%tmm0": "AMX",
            "tdpbssd %tmm2,%tmm1,%tmm0": "AMX",
        }
        for text, feature in cases.items():
            with self.subTest(text=text):
                self.assertEqual(self._classify(text), feature)

    def test_evex_encoding_needs_avx512_even_on_xmm(self) -> None:
        # MSVC's auto-vectorizer emits vpmaxuq xmm (EVEX-only, AVX512F+VL); by its
        # operands alone it looks like a VEX instruction.
        cases = {
            "62 f2 ed 08 3f d1           \tvpmaxuq\t%xmm1, %xmm2, %xmm2": "AVX-512",
            "62 f2 ed 08 3f d1    \tvpmaxuq %xmm1,%xmm2,%xmm2": "AVX-512",
            "c5 f9 6f c1                 \tvmovdqa\t%xmm1, %xmm0": "AVX (VEX)",
            "66 0f 6f c1                 \tmovdqa\t%xmm1, %xmm0": None,
            "f3 48 0f bc c7              \ttzcntq\t%rdi, %rax": "TZCNT",
            "vmovdqa\t%xmm1, %xmm0": "AVX (VEX)",
        }
        for text, feature in cases.items():
            with self.subTest(text=text):
                evex, instruction = checker.split_raw_bytes(text)
                self.assertEqual(checker.classify(*checker.split_instruction(instruction), evex), feature)

    def test_separately_bitted_vex_extensions_are_not_avx(self) -> None:
        # Each has its own CPUID bit, so an AVX2 guard must never cover it.
        cases = {
            "vpdpbusd %xmm0, %xmm1, %xmm2": "AVX-VNNI",
            "vpdpwssd %ymm0, %ymm1, %ymm2": "AVX-VNNI",
            "vpmadd52luq %xmm0, %xmm1, %xmm2": "AVX-IFMA",
            "vcvtneps2bf16 %ymm0, %xmm1": "AVX-NE-CONVERT",
            "vbcstnebf162ps (%rax), %ymm0": "AVX-NE-CONVERT",
            "vaesenc %xmm0, %xmm1, %xmm2": "VAES",
            "vgf2p8mulb %xmm0, %xmm1, %xmm2": "GFNI",
        }
        for text, feature in cases.items():
            with self.subTest(text=text):
                self.assertEqual(self._classify(text), feature)

    def test_unrecognized_vex_mnemonic_fails_closed(self) -> None:
        # A VEX vector instruction llvm might print that AVX/AVX2 does not
        # establish must not fall through to plain AVX.
        for text in ("vfutureop %ymm0, %ymm1, %ymm2", "vpnewthing %xmm0, %xmm1, %xmm2"):
            with self.subTest(text=text):
                self.assertEqual(self._classify(text), "AVX (unrecognized VEX)")

    def test_real_avx_avx2_vex_mnemonics_stay_avx(self) -> None:
        for text, feature in (("vpaddd %ymm0, %ymm1, %ymm2", "AVX/AVX2 (ymm)"),
                              ("vmovdqu %xmm1, %xmm0", "AVX (VEX)"),
                              ("vcmpgt_oqpd %ymm0, %ymm1, %ymm2", "AVX/AVX2 (ymm)"),
                              ("vfmadd231ps %ymm2, %ymm1, %ymm0", "FMA")):
            with self.subTest(text=text):
                self.assertEqual(self._classify(text), feature)

    def test_undecodable_bytes_are_violations_not_instructions(self) -> None:
        # These may be data or newer instructions. Neither proves floor safety.
        lines = ["140001000: 62 f2 ff        \t<unknown>", "  140001003:\t62 ff    \t(bad)",
                 "140001005: c5 f9 6f c1     \tvmovdqa\t%xmm1, %xmm0"]
        result = checker.scan_lines("image", lines, [])
        self.assertEqual(result.instructions, 1)
        self.assertEqual([finding.feature for finding in result.violations],
                         ["undecodable", "undecodable", "AVX (VEX)"])

    def test_tzcnt_is_informational(self) -> None:
        self.assertEqual(self._classify("tzcnt %edi,%eax"), "TZCNT")
        self.assertEqual(self._classify("rep bsf %edi,%eax"), None)

    def test_annotation_does_not_count_as_operand(self) -> None:
        # llvm-objdump annotates FMA with "# xmm0 = ..."; a legacy instruction
        # followed by a symbol comment must not pick up register names from it.
        self.assertEqual(self._classify("vfmadd132ss (%rsi), %xmm1, %xmm0 # xmm0 = (xmm0 * mem) + xmm1"), "FMA")
        self.assertIsNone(self._classify("verw (%rax) # ymm0"))


@unittest.skipUnless(X86_HOST and CC and DISASSEMBLER, "needs an x86-64 host with a C compiler and objdump")
class ElfFixtureTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.source = self.tmp / "fixture.c"
        self.source.write_text(HALF_INCLUDE + FIXTURE_SOURCE, encoding="utf-8")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _compile(self, name: str, *flags: str) -> Path:
        output = self.tmp / f"{name}.o"
        # The half-float helper needs F16C to compile at all; floor builds get
        # a floor-only replacement so the source stays otherwise identical.
        source = self.source
        if "-mf16c" not in flags:
            source = self.tmp / f"{name}.c"
            source.write_text(
                FIXTURE_SOURCE.replace(
                    "float HalfToFloat(unsigned short half) { return _cvtsh_ss(half); }",
                    "float HalfToFloat(unsigned short half) { return (float)half; }",
                ),
                encoding="utf-8",
            )
        subprocess.run([CC, "-O3", "-c", str(source), "-o", str(output), *flags], check=True, timeout=120)
        return output

    def test_floor_build_passes(self) -> None:
        result = _run_checker(str(self._compile("floor", "-msse4.2", "-mpopcnt")))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("0 above-floor", result.stdout)

    def test_avx2_fma_build_fails(self) -> None:
        result = _run_checker(str(self._compile("avx2", "-mavx2", "-mfma")))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("violation FMA", result.stdout)
        self.assertIn("violation AVX/AVX2 (ymm)", result.stdout)
        self.assertIn("MultiplyAdd", result.stdout)

    def test_bmi_build_fails(self) -> None:
        result = _run_checker(str(self._compile("bmi", "-msse4.2", "-mbmi", "-mbmi2")))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertRegex(result.stdout, r"violation BMI[12]")

    def test_lzcnt_build_fails(self) -> None:
        result = _run_checker(str(self._compile("lzcnt", "-msse4.2", "-mlzcnt")))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("violation LZCNT", result.stdout)
        self.assertIn("CountLeading", result.stdout)

    def test_f16c_build_fails(self) -> None:
        result = _run_checker(str(self._compile("f16c", "-msse4.2", "-mf16c")))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("violation F16C", result.stdout)

    def test_allow_symbol_exempts_only_the_named_function(self) -> None:
        obj = str(self._compile("lzcnt_allowed", "-msse4.2", "-mlzcnt"))
        allowed = _run_checker("--allow-symbol", "^CountLeading$", obj)
        self.assertEqual(allowed.returncode, 0, allowed.stdout + allowed.stderr)
        self.assertIn("allowed (cpuid-dispatched) LZCNT", allowed.stdout)

        avx_obj = str(self._compile("avx_allowed", "-mavx2", "-mfma"))
        partial = _run_checker("--allow-symbol", "^CountLeading$", avx_obj)
        self.assertEqual(partial.returncode, 1, partial.stdout + partial.stderr)

    def test_legacy_encoded_extension_builds_fail(self) -> None:
        # AES-NI, PCLMULQDQ, SHA-NI, GFNI, RDRAND/RDSEED and ADX are not VEX-encoded,
        # so only the mnemonic classifier (not the xmm/ymm operand test) sees them.
        cases = (
            ("aes", ("-maes",), "__m128i F(__m128i a, __m128i k) { return _mm_aesenc_si128(a, k); }", "AES-NI"),
            (
                "pclmul",
                ("-mpclmul",),
                "__m128i F(__m128i a, __m128i b) { return _mm_clmulepi64_si128(a, b, 0x11); }",
                "PCLMULQDQ",
            ),
            ("sha", ("-msha",), "__m128i F(__m128i a, __m128i b) { return _mm_sha1msg1_epu32(a, b); }", "SHA"),
            ("gfni", ("-mgfni",), "__m128i F(__m128i a, __m128i b) { return _mm_gf2p8mul_epi8(a, b); }", "GFNI"),
            ("rdrnd", ("-mrdrnd",), "int F(unsigned* v) { return _rdrand32_step(v); }", "RDRAND"),
            ("rdseed", ("-mrdseed",), "int F(unsigned* v) { return _rdseed32_step(v); }", "RDSEED"),
            # GCC lowers _addcarryx_u32 to plain ADC, so ADX comes from inline
            # asm -- one of the routes the configure-time flag check cannot see.
            (
                "adx",
                (),
                "unsigned long F(unsigned long a, unsigned long b)"
                ' { __asm__("adcx %1, %0" : "+r"(a) : "r"(b)); return a; }',
                "ADX",
            ),
        )
        for name, flags, body, feature in cases:
            with self.subTest(feature=feature):
                source = self.tmp / f"{name}.c"
                source.write_text("#include <immintrin.h>\n" + body + "\n", encoding="utf-8")
                output = self.tmp / f"{name}.o"
                subprocess.run(
                    [CC, "-O2", "-msse4.2", *flags, "-c", str(source), "-o", str(output)], check=True, timeout=120
                )
                result = _run_checker(str(output))
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(f"violation {feature}", result.stdout)

    def _build(self, name: str, suffix: str, source: str, *flags: str) -> str:
        path = self.tmp / f"{name}{suffix}"
        path.write_text(source, encoding="utf-8")
        output = self.tmp / f"{name}.o"
        compiler = CXX if suffix == ".cpp" else CC
        command = [compiler, "-O2", "-msse4.2", *flags, "-c", str(path), "-o", str(output)]
        subprocess.run(command, check=True, timeout=120)
        return str(output)

    def test_xsave_and_tsx_builds_fail(self) -> None:
        xgetbv = self._build("xgetbv", ".c", XGETBV_SOURCE.format(name="Probe"))
        result = _run_checker(xgetbv)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("violation XSAVE", result.stdout)
        rtm = self._build("rtm", ".c", "#include <immintrin.h>\nunsigned F(void) { return _xbegin(); }\n", "-mrtm")
        result = _run_checker(rtm)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("violation TSX", result.stdout)

    @unittest.skipUnless(CXX, "needs a C++ compiler")
    def test_xgetbv_is_exempt_only_in_read_xcr0_and_only_for_xsave(self) -> None:
        read_xcr0 = "namespace Spark { namespace Detail {\n%s\n} }\n"
        allowed = self._build("readxcr0", ".cpp", read_xcr0 % XGETBV_SOURCE.format(name="ReadXcr0"))
        result = _run_checker(allowed)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("allowed (cpuid-dispatched) XSAVE: 1", result.stdout)
        for name, body in (("other", read_xcr0 % XGETBV_SOURCE.format(name="ReadXcr0Other")),
                           ("outer", "namespace Spark {\n%s\n}\n" % XGETBV_SOURCE.format(name="ReadXcr0"))):
            with self.subTest(name=name):
                result = _run_checker(self._build(name, ".cpp", body))
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("violation XSAVE", result.stdout)
        with_avx2 = XGETBV_SOURCE.format(name="ReadXcr0").replace(
            "__asm__ volatile(", '__asm__ volatile("vpxor %%ymm0, %%ymm0, %%ymm0" ::: "xmm0");\n    __asm__ volatile(')
        result = _run_checker(self._build("avx2", ".cpp", read_xcr0 % with_avx2))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("violation AVX/AVX2 (ymm)", result.stdout)
        self.assertIn("allowed (cpuid-dispatched) XSAVE", result.stdout)

    def test_llvm_objdump_agrees(self) -> None:
        llvm_objdump = shutil.which("llvm-objdump")
        if not llvm_objdump:
            self.skipTest("llvm-objdump not installed")
        floor = _run_checker("--objdump", llvm_objdump, str(self._compile("floor_llvm", "-msse4.2", "-mpopcnt")))
        self.assertEqual(floor.returncode, 0, floor.stdout + floor.stderr)
        avx = _run_checker("--objdump", llvm_objdump, str(self._compile("avx_llvm", "-mavx2", "-mfma")))
        self.assertEqual(avx.returncode, 1, avx.stdout + avx.stderr)
        self.assertIn("violation FMA", avx.stdout)


@unittest.skipUnless(CLANG and LLD_LINK and DISASSEMBLER, "needs clang and lld-link for a PE/COFF fixture")
class PeFixtureTests(unittest.TestCase):
    SOURCE = """
int _fltused = 0;
__declspec(dllexport) void Scale(float* out, const float* a, const float* b)
{
    for (int i = 0; i < 64; ++i)
        out[i] = out[i] * a[i] + b[i];
}
"""

    # The Microsoft build path of the /MD STL's vector_algorithms.obj and the
    # reviewed toolset's library directory, both as the PDB records them.
    REVIEWED_OBJECT = ("Intermediate/crt/github/stl/msbuild/stl_base/md/msvcp_base_md_kernel32.vcxproj"
                       "/objr/amd64/vector_algorithms.obj")

    def _compile(self, source: Path, obj: Path, *flags: str) -> None:
        obj.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [CLANG, "--target=x86_64-pc-windows-msvc", "-O2", "-g", "-gcodeview",
             *flags, "-c", str(source), "-o", str(obj)],
            check=True,
            timeout=120,
        )

    def _link(self, tmp: Path, name: str, *flags: str, reviewed: bool = False, extra_source: str = "",
              toolset: str = "14.44.35207", source_text: str | None = None, suffix: str = ".c") -> Path:
        """Link one DLL. reviewed=True puts the code in an MSVC-shaped vector_algorithms.obj
        inside <toolset>/lib/x64/msvcprt.lib; extra_source becomes a separate neighbouring object."""
        work = tmp / name
        work.mkdir()
        source = work / f"pe{suffix}"
        contents = source_text if source_text is not None else self.SOURCE
        if reviewed:
            contents = contents.replace("Scale", "__std_reverse_trivially_swappable_1")
        source.write_text(contents, encoding="utf-8")
        inputs = []
        if reviewed:
            obj = work / self.REVIEWED_OBJECT
            self._compile(source, obj, *flags)
            library = work / "MSVC" / toolset / "lib" / "x64" / "msvcprt.lib"
            library.parent.mkdir(parents=True)
            subprocess.run([LLVM_LIB, f"/out:{library}", str(obj)], check=True, timeout=120)
            inputs += ["/include:__std_reverse_trivially_swappable_1", str(library)]
        else:
            obj = work / f"{name}.obj"
            self._compile(source, obj, *flags)
            inputs.append(str(obj))
        if extra_source:
            neighbor = work / "neighbor.c"
            neighbor.write_text(extra_source, encoding="utf-8")
            self._compile(neighbor, work / "neighbor.obj", *flags)
            inputs.insert(0, str(work / "neighbor.obj"))
        dll = tmp / f"{name}.dll"
        pdb = tmp / f"{name}.pdb"
        subprocess.run(
            [LLD_LINK, "/dll", "/noentry", "/nodefaultlib", "/debug", f"/pdb:{pdb}", f"/out:{dll}", *inputs],
            check=True,
            timeout=120,
        )
        return dll

    def test_pe_avx2_fails_and_floor_passes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            avx2_dll = self._link(tmp, "avx2", "-mavx2", "-mfma")
            avx2 = _run_checker("--pdb", str(avx2_dll.with_suffix(".pdb")), str(avx2_dll))
            self.assertEqual(avx2.returncode, 1, avx2.stdout + avx2.stderr)
            self.assertIn("violation AVX/AVX2 (ymm)", avx2.stdout)
            floor_dll = self._link(tmp, "floor", "-msse4.2")
            floor = _run_checker("--pdb", str(floor_dll.with_suffix(".pdb")), str(floor_dll))
            self.assertEqual(floor.returncode, 0, floor.stdout + floor.stderr)

    def test_pe_ignores_allow_symbol_regex(self) -> None:
        # A PE exemption comes only from reviewed PDB ranges, never from a regex.
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            dll = self._link(tmp, "regex", "-mavx2")
            result = _run_checker("--allow-symbol", ".*", "--pdb", str(dll.with_suffix(".pdb")), str(dll))
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("violation AVX/AVX2 (ymm)", result.stdout)
            self.assertNotIn("allowed (cpuid-dispatched)", result.stdout)

    def test_pe_requires_matching_pdb(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            image = self._link(tmp, "missing", "-msse4.2")
            missing = _run_checker(str(image))
            self.assertEqual(missing.returncode, 2, missing.stdout + missing.stderr)
            malformed = tmp / "malformed.pdb"
            malformed.write_bytes(b"not a pdb")
            malformed_result = _run_checker("--pdb", str(malformed), str(image))
            self.assertEqual(malformed_result.returncode, 2, malformed_result.stdout + malformed_result.stderr)

    def test_pe_rejects_pdb_from_another_image(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            first = self._link(tmp, "first", "-msse4.2")
            second = self._link(tmp, "second", "-msse4.2")
            result = _run_checker("--pdb", str(second.with_suffix(".pdb")), str(first))
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)

    @unittest.skipUnless(LLVM_LIB, "needs llvm-lib for the MSVC-shaped runtime library")
    def test_pe_reviewed_vector_algorithms_contribution_is_exempt(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            image = self._link(tmp, "vector_algorithms", "-mavx2", reviewed=True)
            ranges = checker._pdb_info(str(image.with_suffix(".pdb")), str(image)).ranges
            self.assertEqual([item.symbol for item in ranges], ["<section contribution>"])
            result = _run_checker("--pdb", str(image.with_suffix(".pdb")), str(image))
            # This metadata fixture tests the contribution selector, not CPUID dispatch.
            # Runtime guard provenance is reviewed separately in the MSVC source.
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("allowed (cpuid-dispatched) AVX/AVX2 (ymm)", result.stdout)

    @unittest.skipUnless(LLVM_LIB, "needs llvm-lib for the MSVC-shaped runtime library")
    def test_pe_neighbour_module_other_toolset_and_extra_isa_are_not_exempt(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            neighbor = self.SOURCE.replace("Scale", "Unreviewed").replace("int _fltused = 0;", "")
            image = self._link(tmp, "neighbor", "-mavx2", reviewed=True, extra_source=neighbor)
            result = _run_checker("--pdb", str(image.with_suffix(".pdb")), str(image))
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("allowed (cpuid-dispatched)", result.stdout)
            self.assertIn("Unreviewed", result.stdout)
            image = self._link(tmp, "toolset", "-mavx2", reviewed=True, toolset="14.45.00000")
            result = _run_checker("--pdb", str(image.with_suffix(".pdb")), str(image))
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertNotIn("allowed (cpuid-dispatched)", result.stdout)
            image = self._link(tmp, "fma", "-mavx2", "-mfma", reviewed=True)
            result = _run_checker("--pdb", str(image.with_suffix(".pdb")), str(image))
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("violation FMA", result.stdout)

    def test_pe_xgetbv_is_exempt_only_in_read_xcr0(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            body = XGETBV_SOURCE.replace("__attribute__((noinline))", "__declspec(dllexport) __declspec(noinline)")
            source = "namespace Spark { namespace Detail {\n%s\n} }\n"
            image = self._link(tmp, "readxcr0", "-msse4.2", suffix=".cpp",
                               source_text=source % body.format(name="ReadXcr0"))
            result = _run_checker("--pdb", str(image.with_suffix(".pdb")), str(image))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("allowed (cpuid-dispatched) XSAVE: 1", result.stdout)
            image = self._link(tmp, "other", "-msse4.2", suffix=".cpp",
                               source_text=source % body.format(name="ReadXcr0Other"))
            result = _run_checker("--pdb", str(image.with_suffix(".pdb")), str(image))
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("violation XSAVE", result.stdout)


@unittest.skipUnless(DISASSEMBLER, "needs objdump or llvm-objdump")
class ErrorTests(unittest.TestCase):
    def test_missing_and_empty_files_are_errors(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            empty = Path(directory) / "empty"
            empty.write_bytes(b"")
            self.assertEqual(_run_checker(str(Path(directory) / "missing")).returncode, 2)
            self.assertEqual(_run_checker(str(empty)).returncode, 2)

    @unittest.skipUnless(CLANG, "needs clang to build a non-x86 object")
    def test_non_x86_image_is_an_error_not_a_pass(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "arm.c"
            source.write_text("int Add(int a, int b) { return a + b; }\n", encoding="utf-8")
            obj = Path(directory) / "arm.o"
            subprocess.run(
                [CLANG, "--target=aarch64-linux-gnu", "-O2", "-c", str(source), "-o", str(obj)],
                check=True,
                timeout=120,
            )
            result = _run_checker(str(obj))
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
