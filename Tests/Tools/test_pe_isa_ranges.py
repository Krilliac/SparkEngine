"""BLD-100: exact PDB ranges never excuse neighboring code or unreviewed ISA.

Synthetic llvm-pdbutil text, so these run on every host. Two range sources are
covered: exact reviewed procedure records (`dump --symbols`) and DBI section
contributions of the reviewed MSVC vector_algorithms.obj (`dump --modules
--section-contribs`), which carry no S_GPROC32 records for most of their code.
"""

import unittest

from Tests.Tools.test_check_isa_baseline import checker

BASE = 0x180000000
SECTIONS = [(0x1000, 256, True), (0x2000, 256, False)]
TOOLSET_LIB = r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\lib\x64"
VECTOR_MODULE = r"D:\a\_work\1\s\Intermediate" + checker.REVIEWED_MSVC_MODULE_SUFFIXES[1]
MEMCPY_MODULE = r"D:\a\_work\1\s\Intermediate" + checker.REVIEWED_MSVC_MEMORY_MODULES["memcpy"]
CPU_DISP_MODULE = r"D:\a\_work\1\s\Intermediate" + checker.REVIEWED_MSVC_XSAVE_MODULES[0]


def contributions(*entries, modules=None, header=True):
    """Build `dump --modules --section-contribs` text.

    modules: [(path, obj library)]; entries: (mod, "SSSS:OOOO", size, code?).
    """
    modules = modules if modules is not None else [(VECTOR_MODULE, TOOLSET_LIB + r"\msvcprt.lib")]
    lines = ["", "                          Modules                           ",
             "============================================================"]
    for index, (path, library) in enumerate(modules):
        lines += [f"  Mod {index:04} | `{path}`:",
                  "  SC[???]  | mod = 65535, 65535:0000, size = -1, data crc = 0, reloc crc = 0",
                  "          none",
                  f"  Obj: `{library}`: ",
                  "  debug stream: 533, # files: 53, has ec info: false"]
    if header:
        lines += ["", "                   Section Contributions                    ",
                  "============================================================"]
    for mod, address, size, code in entries:
        lines.append(f"  SC[.text]   | mod = {mod}, {address}, size = {size}, data crc = 1, reloc crc = 0")
        if code:
            lines += ["                IMAGE_SCN_CNT_CODE | IMAGE_SCN_LNK_COMDAT | IMAGE_SCN_ALIGN_16BYTES | ",
                      "                IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ"]
        else:
            lines.append("                IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ")
    return lines


def parse_contributions(lines, sections=SECTIONS):
    return checker._parse_section_contributions(lines, BASE, sections)


class SectionContributionTests(unittest.TestCase):
    def test_contribution_without_procedure_record_is_exempt_for_avx2(self):
        # The pre-change parser ignored SC lines, so these bytes had no range at all.
        ranges, _ = parse_contributions(contributions((0, "0001:0016", 32, True)))
        self.assertEqual([(item.start, item.end) for item in ranges], [(BASE + 0x1010, BASE + 0x1030)])
        for feature in ("AVX (VEX)", "AVX/AVX2 (ymm)", "LZCNT"):
            self.assertTrue(checker._pdb_allows(feature, BASE + 0x1010, ranges))

    def test_half_open_bounds_exclude_neighbouring_bytes(self):
        ranges, _ = parse_contributions(contributions((0, "0001:0016", 32, True)))
        for delta, expected in ((-1, False), (0, True), (31, True), (32, False)):
            self.assertEqual(checker._pdb_allows("AVX/AVX2 (ymm)", BASE + 0x1010 + delta, ranges), expected)

    def test_unreviewed_modules_and_toolsets_are_not_exempt(self):
        library = TOOLSET_LIB + r"\msvcprt.lib"
        for module, obj in ((r"D:\app\vector_algorithms.obj", library),
                            ("vector_algorithms.obj", library),
                            (VECTOR_MODULE + ".other", library),
                            (VECTOR_MODULE, r"D:\app\vector_algorithms.obj"),
                            (VECTOR_MODULE, library.replace("14.44.35207", "14.45.00000"))):
            with self.subTest(module=module, obj=obj):
                ranges, _ = parse_contributions(contributions((0, "0001:0016", 32, True), modules=[(module, obj)]))
                self.assertEqual(ranges, [])

    def test_other_features_stay_violations_inside_the_contribution(self):
        ranges, _ = parse_contributions(contributions((0, "0001:0016", 32, True)))
        for feature in ("FMA", "AVX-512", "F16C", "BMI1", "BMI2", "AES-NI", "XSAVE"):
            with self.subTest(feature=feature):
                self.assertFalse(checker._pdb_allows(feature, BASE + 0x1010, ranges))

    def test_invalid_contributions_fail_closed(self):
        for entry in ((7, "0001:0016", 32, True),  # unknown module index
                      (0, "0001:0016", 0, True),  # zero size
                      (0, "0000:0016", 32, True),  # section 0
                      (0, "0003:0016", 32, True),  # past the last section
                      (0, "0001:0250", 32, True),  # past the section end
                      (0, "0002:0016", 32, True)):  # code in a non-executable section
            with self.subTest(entry=entry), self.assertRaises(RuntimeError):
                parse_contributions(contributions(entry))
        with self.assertRaises(RuntimeError):
            parse_contributions(contributions((0, "0001:0016", 32, True), header=False))

    def test_data_contributions_of_the_reviewed_module_grant_nothing(self):
        ranges, libraries = parse_contributions(contributions((0, "0002:0016", 32, False)))
        self.assertEqual(ranges, [])
        self.assertEqual(libraries[0], (VECTOR_MODULE, TOOLSET_LIB + r"\msvcprt.lib"))

    def test_procedure_and_contribution_for_the_same_bytes_count_once(self):
        contribution, _ = parse_contributions(contributions((0, "0001:0016", 32, True)))
        procedure = checker.PdbRange(BASE + 0x1010, BASE + 0x1030, "memcpy", MEMCPY_MODULE)
        info = checker.PdbInfo(contribution + [procedure], [], {})
        result = checker.scan_lines("x.dll", [f"{BASE + 0x1010:x}:     \tvmovdqu\t(%rcx), %ymm0"], [], info)
        self.assertEqual(result.allowed["AVX/AVX2 (ymm)"], 1)
        self.assertEqual(result.violations, [])


class ProcedureRangeTests(unittest.TestCase):
    def parse(self, *, module=MEMCPY_MODULE, symbol="memcpy", address="0001:0016", size=32,
              executable=True, suffix="", libraries=None):
        dump = (f"Mod 0000 | `{module}`:\n"
                f"  188 | S_GPROC32 [size = 80] `{symbol}`\n"
                f"    parent = 0, end = 292, addr = {address}, code size = {size}\n" + suffix)
        ranges, _ = checker._parse_pdb_symbols(dump.splitlines(), BASE, [(0x1000, 256, executable)], libraries)
        return ranges

    def test_decimal_offsets_match_real_pdbutil_output(self):
        # LLVM prints offset 16 as 0016; the PE address is 0x...1010, not ...1016.
        ranges = self.parse()
        self.assertEqual([(item.start, item.end) for item in ranges], [(BASE + 0x1010, BASE + 0x1030)])

    def test_exact_memory_pairs_allow_avx_only(self):
        for symbol, suffix in checker.REVIEWED_MSVC_MEMORY_MODULES.items():
            ranges = self.parse(module="D:\\build" + suffix, symbol=symbol)
            self.assertEqual(len(ranges), 1)
            self.assertTrue(checker._pdb_allows("AVX/AVX2 (ymm)", ranges[0].start, ranges))
            for feature in ("FMA", "LZCNT", "XSAVE", "AES-NI"):
                self.assertFalse(checker._pdb_allows(feature, ranges[0].start, ranges))
            self.assertEqual(self.parse(module="D:\\build" + suffix, symbol=symbol + "_extra"), [])
            self.assertEqual(self.parse(module=VECTOR_MODULE, symbol=symbol), [])

    def test_vector_procedures_need_no_symbol_list(self):
        # The contribution ranges cover vector_algorithms.obj; a procedure record alone grants nothing.
        self.assertEqual(self.parse(module=VECTOR_MODULE, symbol="__std_reverse_trivially_swappable_1"), [])

    def test_xsave_pairs_are_scoped_to_xsave(self):
        library = TOOLSET_LIB + r"\msvcrt.lib"
        ranges = self.parse(module=CPU_DISP_MODULE, symbol="__isa_available_init",
                            libraries={0: (CPU_DISP_MODULE, library)})
        self.assertEqual(len(ranges), 1)
        self.assertTrue(checker._pdb_allows("XSAVE", ranges[0].start, ranges))
        self.assertFalse(checker._pdb_allows("AVX (VEX)", ranges[0].start, ranges))
        self.assertEqual(self.parse(module=CPU_DISP_MODULE, symbol="__isa_available_init"), [])  # no library proof
        self.assertEqual(self.parse(module=r"D:\app\cpu_disp.obj", symbol="__isa_available_init",
                                    libraries={0: (r"D:\app\cpu_disp.obj", library)}), [])
        ranges = self.parse(module=r"D:\app\main.obj", symbol=checker.SPARK_XSAVE_PROCEDURE)
        self.assertEqual([item.features for item in ranges], [frozenset({"XSAVE"})])
        self.assertEqual(self.parse(module=r"D:\app\main.obj", symbol="Spark::DetectCpuFeatures"), [])

    def test_name_exemption_is_dropped_when_another_procedure_shares_the_bytes(self):
        folded = ("Mod 0001 | `D:\\app\\other.obj`:\n  200 | S_GPROC32 [size = 80] `Other`\n"
                  "    parent = 0, end = 300, addr = 0001:0016, code size = 32\n")
        self.assertEqual(self.parse(module=r"D:\app\main.obj", symbol=checker.SPARK_XSAVE_PROCEDURE, suffix=folded), [])

    def test_invalid_section_or_extent_is_rejected(self):
        for options in ({"address": "0000:0016"}, {"address": "0002:0016"},
                        {"address": "0001:0250"}, {"size": 0}, {"executable": False}):
            with self.subTest(options=options), self.assertRaises(RuntimeError):
                self.parse(**options)

    def test_missing_procedure_extent_fails_closed(self):
        dump = f"Mod 0 | `{MEMCPY_MODULE}`:\n  188 | S_GPROC32 [size = 80] `memcpy`\n"
        with self.assertRaises(RuntimeError):
            checker._parse_pdb_symbols(dump.splitlines(), BASE, [(0x1000, 256, True)])

    def test_other_symbol_record_cannot_supply_procedure_extent(self):
        dump = (f"Mod 0 | `{MEMCPY_MODULE}`:\n  188 | S_GPROC32 [size = 80] `memcpy`\n"
                "  256 | S_COFFGROUP [size = 28] `.text`\n  addr = 0001:0016, code size = 32\n")
        with self.assertRaises(RuntimeError):
            checker._parse_pdb_symbols(dump.splitlines(), BASE, [(0x1000, 256, True)])

    def test_every_procedure_extent_is_recorded_for_guard_analysis(self):
        dump = ("Mod 0000 | `D:\\app\\a.obj`:\n  4 | S_GPROC32 [size = 52] `A`\n"
                "    parent = 0, end = 60, addr = 0001:0000, code size = 16\n"
                "  64 | S_LPROC32 [size = 52] `B`\n    parent = 0, end = 120, addr = 0001:0008, code size = 16\n"
                "  124 | S_LPROC32 [size = 52] ``anonymous namespace'::C<1,`anonymous namespace'::T>`\n"
                "    parent = 0, end = 180, addr = 0001:0064, code size = 16\n")
        _, procedures = checker._parse_pdb_symbols(dump.splitlines(), BASE, [(0x1000, 256, True)])
        self.assertEqual([(p.name, p.start - BASE, p.ambiguous) for p in procedures],
                         [("A", 0x1000, True), ("B", 0x1008, True),
                          ("`anonymous namespace'::C<1,`anonymous namespace'::T>", 0x1040, False)])


if __name__ == "__main__":
    unittest.main()
