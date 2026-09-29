"""BLD-100: exact PDB ranges never excuse neighboring code or unreviewed ISA."""

import unittest

from Tests.Tools.test_check_isa_baseline import checker


class PdbRangeTests(unittest.TestCase):
    MODULE = r"D:\crt\src\stl\vector_algorithms.obj"
    SYMBOL = "__std_reverse_trivially_swappable_1"
    BASE = 0x180000000

    def parse(self, *, module=MODULE, symbol=SYMBOL, address="0001:0016", size=32,
              executable=True, suffix=""):
        dump = (f"Mod 0000 | `{module}`:\n"
                f"  188 | S_GPROC32 [size = 80] `{symbol}`\n"
                f"    parent = 0, end = 292, addr = {address}, code size = {size}\n" + suffix)
        return checker._parse_pdb_ranges(dump.splitlines(), self.BASE, [(0x1000, 256, executable)])

    def test_decimal_offsets_match_real_pdbutil_output(self):
        # LLVM prints offset 16 as 0016; the PE address is 0x...1010, not ...1016.
        ranges = self.parse()
        self.assertEqual(len(ranges), 1)
        self.assertEqual((ranges[0].start, ranges[0].end), (self.BASE + 0x1010, self.BASE + 0x1030))

    def test_half_open_range_never_exempts_neighboring_instructions(self):
        ranges = self.parse()
        for delta, expected in ((-1, False), (0, True), (31, True), (32, False)):
            self.assertEqual(checker._pdb_allows("AVX/AVX2 (ymm)", ranges[0].start + delta, ranges), expected)

    def test_cpuid_avx2_guard_does_not_authorize_other_features(self):
        ranges = self.parse()
        for feature in ("FMA", "F16C", "AVX-512", "BMI1", "BMI2", "LZCNT", "AES-NI"):
            with self.subTest(feature=feature):
                self.assertFalse(checker._pdb_allows(feature, ranges[0].start, ranges))

    def test_exact_symbol_and_module_are_both_required(self):
        for symbol in ("Scale", "__std_find_evil", self.SYMBOL + "_extra"):
            self.assertEqual(self.parse(symbol=symbol), [])
        for module in ("vector_algorithms.obj", r"D:\app\vector_algorithms.obj", self.MODULE + ".other"):
            self.assertEqual(self.parse(module=module), [])

    def test_observed_msvc_runtime_provenance_and_exact_memory_pairs(self):
        for suffix in checker.REVIEWED_MSVC_MODULE_SUFFIXES[2:]:
            self.assertEqual(len(self.parse(module="D:\\build" + suffix)), 1)
        for symbol, suffix in checker.REVIEWED_MSVC_MEMORY_MODULES.items():
            self.assertEqual(len(self.parse(module="D:\\build" + suffix, symbol=symbol)), 1)
            self.assertEqual(self.parse(module="D:\\build" + suffix, symbol=symbol + "_extra"), [])
            self.assertEqual(self.parse(module=self.MODULE, symbol=symbol), [])

    def test_invalid_section_or_extent_is_rejected(self):
        for options in ({"address": "0000:0016"}, {"address": "0002:0016"},
                        {"address": "0001:0250"}, {"size": 0}, {"executable": False}):
            with self.subTest(options=options), self.assertRaises(RuntimeError):
                self.parse(**options)

    def test_missing_procedure_extent_fails_closed(self):
        dump = f"Mod 0 | `{self.MODULE}`:\n  188 | S_GPROC32 [size = 80] `{self.SYMBOL}`\n"
        with self.assertRaises(RuntimeError):
            checker._parse_pdb_ranges(dump.splitlines(), self.BASE, [(0x1000, 256, True)])

    def test_other_symbol_record_cannot_supply_procedure_extent(self):
        dump = (f"Mod 0 | `{self.MODULE}`:\n  188 | S_GPROC32 [size = 80] `{self.SYMBOL}`\n"
                "  256 | S_COFFGROUP [size = 28] `.text`\n  addr = 0001:0016, code size = 32\n")
        with self.assertRaises(RuntimeError):
            checker._parse_pdb_ranges(dump.splitlines(), self.BASE, [(0x1000, 256, True)])


if __name__ == "__main__":
    unittest.main()
