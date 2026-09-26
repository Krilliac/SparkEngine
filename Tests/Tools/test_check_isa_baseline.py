#!/usr/bin/env python3
"""BLD-100 / OD-04: tools/check_isa_baseline.py rejects above-floor instructions.

The stable-v1 CPU floor is x86-64 with SSE4.2 and POPCNT. These tests compile
real fixtures and run the checker on the linked or assembled result:

* an ELF object built at the floor passes, while the same source built with
  -mavx2 -mfma, -mbmi/-mbmi2, -mlzcnt or -mf16c fails and names the feature;
* the legacy-encoded extensions above the floor (AES-NI, PCLMULQDQ, SHA-NI,
  GFNI, RDRAND, RDSEED, ADX) fail as well;
* the TZCNT encoding GCC/Clang emit at the floor for ctz (REP BSF) is
  reported as informational, not as a violation;
* --allow-symbol exempts only the named CPUID-dispatched function;
* a PE/COFF DLL linked with lld-link is disassembled and judged the same way;
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
CLANG = shutil.which("clang")
LLD_LINK = shutil.which("lld-link")
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
            "vaesenc %xmm2,%xmm1,%xmm0": "AVX (VEX)",
        }
        for text, feature in cases.items():
            with self.subTest(text=text):
                self.assertEqual(self._classify(text), feature)

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

    def _link(self, tmp: Path, name: str, *flags: str) -> Path:
        source = tmp / "pe.c"
        source.write_text(self.SOURCE, encoding="utf-8")
        obj = tmp / f"{name}.obj"
        dll = tmp / f"{name}.dll"
        subprocess.run(
            [CLANG, "--target=x86_64-pc-windows-msvc", "-O2", *flags, "-c", str(source), "-o", str(obj)],
            check=True,
            timeout=120,
        )
        subprocess.run(
            [LLD_LINK, "/dll", "/noentry", "/nodefaultlib", f"/out:{dll}", str(obj)], check=True, timeout=120
        )
        return dll

    def test_pe_avx2_fails_and_floor_passes(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            avx2 = _run_checker(str(self._link(tmp, "avx2", "-mavx2", "-mfma")))
            self.assertEqual(avx2.returncode, 1, avx2.stdout + avx2.stderr)
            self.assertIn("violation AVX/AVX2 (ymm)", avx2.stdout)
            floor = _run_checker(str(self._link(tmp, "floor", "-msse4.2")))
            self.assertEqual(floor.returncode, 0, floor.stdout + floor.stderr)


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
