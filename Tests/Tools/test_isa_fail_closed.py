"""BLD-100: incomplete decoding and stronger instructions cannot inherit an exemption."""

import io
import struct
import unittest
from contextlib import redirect_stdout
from unittest import mock

from Tests.Tools.test_check_isa_baseline import checker
from Tests.Tools.test_isa_guard_dominance import WMEM_GUARDED, WMEM, guard, target, scan


class FailClosedTests(unittest.TestCase):
    def test_llvm_split_prefix_is_joined_to_the_adjacent_instruction(self):
        # Real LLVM output for __scrt_acquire_startup_lock in Shipping images.
        lines = ["1000: f0 \tlock", "1001: 48 0f b1 0d 00 00 00 00 \tcmpxchgq %rcx, (%rip)"]
        result = checker.scan_lines("image", lines, [])
        self.assertEqual(result.violations, [])
        self.assertEqual(result.instructions, 1)
        for following in ([], ["1002: c3 \tretq"], ["1001: ff \t<unknown>"]):
            with self.subTest(following=following):
                self.assertTrue(checker.scan_lines("image", lines[:1] + following, []).violations)

    def test_undecodable_bytes_remain_violations(self):
        # An older disassembler can print unknown for a newer ISA instruction.
        # A neighbouring valid instruction must not turn that into a passing scan.
        for text in ("<unknown>", "(bad)", ".byte 0x62"):
            with self.subTest(text=text):
                result = checker.scan_lines("image", [f"1000: 62 ff \t{text}", "1002: c3 \tretq"], [])
                self.assertTrue(result.violations)

    def test_report_distinguishes_unknown_bytes_from_identified_extensions(self):
        result = checker.scan_lines("image", ["1000: 62 ff \t<unknown>", "1002: c3 \tretq"], [])
        output = io.StringIO()
        with redirect_stdout(output):
            checker.report(result, 20)
        self.assertIn("FAIL image", output.getvalue())
        self.assertIn("0 above-floor, 1 undecodable", output.getvalue())

    def test_vector_extensions_do_not_inherit_avx_guard(self):
        for text in ("vaesenc %xmm0, %xmm1, %xmm2", "vpclmulqdq $0, %xmm0, %xmm1, %xmm2",
                     "vgf2p8mulb %xmm0, %xmm1, %xmm2", "vprotd $1, %xmm0, %xmm1"):
            with self.subTest(text=text):
                rows = list(WMEM_GUARDED)
                rows[2] = (0x09, text)
                self.assertTrue(scan(rows).violations)

    def test_xsave_review_authorizes_only_xgetbv(self):
        proc = checker.SPARK_XSAVE_PROCEDURE
        pe = checker.PdbInfo([checker.PdbRange(0x1000, 0x1020, proc, "MultiISA.obj",
                                             frozenset({checker.XSAVE}))], [], {}, frozenset({checker.REVIEWED_TOOLSET}))
        for text in ("xsave64 (%rcx)", "xrstor64 (%rcx)", "xsetbv"):
            for pdb in (None, pe):
                with self.subTest(text=text, pe=pdb is not None):
                    lines = [f"1000 <{proc}()>:", f"1000: {text}", "1004: retq"]
                    self.assertTrue(checker.scan_lines("image", lines, [], pdb).violations)

    def test_unknown_instruction_cannot_bridge_guard_flags(self):
        rows = [(0x00, guard(WMEM)), (0x07, "<unknown>"), (0x09, f"je {target(0x20)}"),
                (0x0b, "vmovdqu (%rcx), %ymm0"), (0x0f, "retq"), (0x20, "retq")]
        self.assertIn("AVX/AVX2 (ymm)", {finding.feature for finding in scan(rows).violations})

    def test_guard_requires_all_four_bytes_inside_section(self):
        for section, offset in ((0, 0), (2, 0), (1, 253), (1, 256), (1, 999999)):
            with self.subTest(section=section, offset=offset):
                lines = ["1 | S_PUB32 [size = 32] `_Avx2WmemEnabled`",
                         f" flags = none, addr = {section}:{offset}"]
                with self.assertRaises(RuntimeError):
                    checker._parse_pdb_guards(lines, 0x140000000, [(0x1000, 256, False)])

    def test_data_guard_can_reside_in_zero_filled_virtual_tail(self):
        # SparkCooker puts _Avx2WmemEnabled just after .data's raw bytes.
        # Code exemptions still require bytes actually present in the image.
        for executable, expected_size in ((False, 256), (True, 128)):
            data = bytearray(64 + 24 + 112 + 40)
            data[:2] = b"MZ"
            struct.pack_into("<I", data, 0x3c, 64)
            data[64:68] = b"PE\0\0"
            struct.pack_into("<HH", data, 68, 0x8664, 1)
            struct.pack_into("<H", data, 84, 112)
            struct.pack_into("<H", data, 88, 0x20b)
            struct.pack_into("<Q", data, 112, 0x140000000)
            struct.pack_into("<III", data, 208, 256, 0x1000, 128)
            struct.pack_into("<I", data, 236, 0x20000000 if executable else 0x80000000)
            with mock.patch("builtins.open", return_value=io.BytesIO(data)):
                self.assertEqual(checker._pe_sections("image")[1][0][1], expected_size)

    def test_mask_zero_instructions_require_avx512(self):
        for text in ("kmovw %k0, %eax", "kortestw %k0, %k0"):
            self.assertEqual(checker.classify(*checker.split_instruction(text)), "AVX-512")

    def test_procedure_lookup_preserves_overlaps_and_gaps(self):
        procedures = [checker.Procedure(10, 100, "outer", True),
                      checker.Procedure(20, 30, "inner", True),
                      checker.Procedure(200, 220, "separate", False)]
        info = checker.PdbInfo([], procedures, {})
        for address, name in ((9, None), (10, "outer"), (20, "inner"), (30, "outer"), (99, "outer"),
                              (100, None), (199, None), (200, "separate"), (220, None), (999, None)):
            with self.subTest(address=address):
                procedure = info.procedure_at(address)
                self.assertEqual(procedure.name if procedure else None, name)


if __name__ == "__main__":
    unittest.main()
