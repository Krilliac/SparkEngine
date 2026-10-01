"""BLD-100: inline MSVC CRT/STL code is exempt only behind a reviewed guard edge.

tools/check_isa_baseline.py exempts an above-floor instruction inlined into an
arbitrary PE procedure only when it is reachable from the procedure entry and
unreachable once the edges a reviewed guard compare implies are removed. The
guard table binds features: _Avx2WmemEnabled(WeakValue) != 0 excuses AVX/AVX2,
__isa_available >= 5 excuses AVX/AVX2/LZCNT, and __isa_available >= 6 excuses
only the reviewed AVX512F/VL instructions vpmaxuq/vpminuq. The disassembly and llvm-pdbutil text here
are synthetic in the formats the real tools print, so this runs on every host.
"""

import unittest

from Tests.Tools.test_check_isa_baseline import checker

BASE = 0x140000000
TEXT = BASE + 0x1000
SECTIONS = [(0x1000, 0x1000, True), (0x2000, 0x1000, False), (0x3000, 0x1000, False)]
WMEM = BASE + 0x3000
ISA = BASE + 0x3020
OTHER = BASE + 0x3040
DEFAULT_PUBLICS = (("_Avx2WmemEnabled", "0003:0000"), ("_Avx2WmemEnabledWeakValue", "0003:0000"),
                   ("__isa_available", "0003:0032"), ("SomethingElse", "0003:0064"))


def pdb_info(size=0x100, publics=DEFAULT_PUBLICS):
    symbols = ("Mod 0000 | `D:\\app\\GatewayAreaControl.obj`:\n"
               "  4 | S_GPROC32 [size = 52] `wmemchr`\n"
               f"    parent = 0, end = 60, addr = 0001:0000, code size = {size}\n")
    public_text = "".join(f"  1 | S_PUB32 [size = 32] `{name}`\n           flags = none, addr = {address}\n"
                          for name, address in publics)
    ranges, procedures = checker._parse_pdb_symbols(symbols.splitlines(), BASE, SECTIONS)
    guards = checker._parse_pdb_guards(public_text.splitlines(), BASE, SECTIONS)
    return checker.PdbInfo(ranges, procedures, guards)


def scan(rows, entries=None, **options):
    """rows: (offset, llvm-objdump instruction text); addresses are TEXT + offset."""
    lines = [f"{TEXT:016x} <.text>:"] + [f"{TEXT + offset:x}:     \t{text}" for offset, text in rows]
    return checker.scan_lines("image.exe", lines, [], pdb_info(**options), None, entries)


def entries(**fields):
    base = dict(image_base=BASE, branch_sources={}, address_taken=set(), other=set())
    base.update(fields)
    return checker._ImageEntries(**base)


def guard(address, immediate="$0x0"):
    return f"cmpl\t{immediate}, 0x1000(%rip)    # {address:#x}"


def target(offset):
    return f"{TEXT + offset:#x} <.text+{offset:#x}>"


# wmemchr's shape: skip the AVX2 loop when the guard is zero.
WMEM_GUARDED = [
    (0x00, guard(WMEM)),
    (0x07, f"je\t{target(0x20)}"),
    (0x09, "vmovdqu\t(%rcx), %ymm0"),
    (0x0d, "vzeroupper"),
    (0x10, "retq"),
    (0x20, "movdqu\t(%rcx), %xmm0"),
    (0x24, "retq"),
]


class GuardDominanceTests(unittest.TestCase):
    def assertAllowed(self, result, *features):
        self.assertEqual(result.violations, [], result.violations)
        for feature in features:
            self.assertGreater(result.allowed[feature], 0, feature)

    def assertViolation(self, result, feature):
        self.assertIn(feature, {finding.feature for finding in result.violations})

    def test_ymm_reachable_only_through_the_nonzero_edge_is_allowed(self):
        # Fails before the guard analysis: the pre-change checker flags every one.
        result = scan(WMEM_GUARDED)
        self.assertAllowed(result, "AVX/AVX2 (ymm)", "AVX (VEX)")
        self.assertEqual(result.allowed["AVX/AVX2 (ymm)"] + result.allowed["AVX (VEX)"], 2)

    def test_both_symbol_names_resolve_the_same_guard(self):
        self.assertAllowed(scan(WMEM_GUARDED, publics=(("_Avx2WmemEnabledWeakValue", "0003:0000"),)), "AVX (VEX)")
        self.assertAllowed(scan(WMEM_GUARDED, publics=(("_Avx2WmemEnabled", "0003:0000"),)), "AVX (VEX)")

    def test_msvc_register_zeroed_compare_is_the_same_guard(self):
        # The real wmemcmp: xorl %eax,%eax; ... cmpl %eax, guard(%rip); movs; je.
        rows = [(0x00, "xorl\t%eax, %eax"), (0x02, "movq\t%rdx, %rdi"),
                (0x05, f"cmpl\t%eax, 0x1000(%rip)    # {WMEM:#x}"), (0x0b, "movq\t%rcx, %r9"),
                (0x0e, "leaq\t0x10(%rip), %r11"), (0x15, f"je\t{target(0x40)}"),
                (0x17, "vmovdqu\t(%r9), %ymm1"), (0x1b, "vzeroupper"), (0x1e, "retq"), (0x40, "retq")]
        self.assertAllowed(scan(rows), "AVX/AVX2 (ymm)")
        clobbered = rows[:2] + [(0x03, "movl\t%edx, %eax")] + rows[2:]
        self.assertViolation(scan(clobbered), "AVX/AVX2 (ymm)")
        not_zeroed = [(0x00, "movl\t%edx, %eax")] + rows[1:]
        self.assertViolation(scan(not_zeroed), "AVX/AVX2 (ymm)")

    def test_second_unguarded_path_to_the_same_block_is_a_violation(self):
        # jne jumps straight to the AVX2 block (0x0d), past the guard.
        rows = [(0x00, "testl\t%edx, %edx"), (0x02, f"jne\t{target(0x0d)}")]
        rows +=[(offset + 0x04, text.replace(target(0x20), target(0x24))) for offset, text in WMEM_GUARDED]
        self.assertViolation(scan(rows), "AVX/AVX2 (ymm)")

    def test_inverted_polarity_is_a_violation(self):
        # jne to the AVX2 block, whose fallthrough (guard == 0) reaches it too.
        rows = [(0x00, guard(WMEM)), (0x07, f"jne\t{target(0x09)}"), (0x09, "vmovdqu\t(%rcx), %ymm0"),
                (0x0d, "retq")]
        self.assertViolation(scan(rows), "AVX/AVX2 (ymm)")
        # AVX2 on the zero path, scalar on the non-zero path.
        rows = [(0x00, guard(WMEM)), (0x07, f"jne\t{target(0x20)}"), (0x09, "vmovdqu\t(%rcx), %ymm0"),
                (0x0d, "retq"), (0x20, "retq")]
        self.assertViolation(scan(rows), "AVX/AVX2 (ymm)")

    def lzcnt_rows(self, address=ISA, immediate="$0x5", condition="jl"):
        # <bit> countl_zero: lzcnt only when __isa_available >= 5, bsr otherwise.
        return [(0x00, guard(address, immediate)), (0x07, f"{condition}\t{target(0x10)}"),
                (0x09, "lzcntq\t%rax, %rax"), (0x0e, f"jmp\t{target(0x18)}"),
                (0x10, "bsrq\t%rax, %rdx"), (0x18, "retq")]

    def test_isa_available_guard_allows_lzcnt(self):
        self.assertAllowed(scan(self.lzcnt_rows()), "LZCNT")

    def test_weaker_or_unsigned_isa_guard_is_a_violation(self):
        self.assertViolation(scan(self.lzcnt_rows(immediate="$0x4")), "LZCNT")
        # Unsigned: the fallthrough also admits negative values.
        self.assertViolation(scan(self.lzcnt_rows(condition="jb")), "LZCNT")

    def test_guard_features_are_bound(self):
        self.assertViolation(scan(self.lzcnt_rows(address=WMEM, immediate="$0x0", condition="je")), "LZCNT")
        # __isa_available >= 5 proves AVX2 (and LZCNT), nothing stronger.
        for text, feature in (("vfmadd231ps\t%ymm2, %ymm1, %ymm0", "FMA"),
                              ("shlxq\t%rsi, %rdi, %rax", "BMI2"),
                              ("62 f2 ed 08 3f d1           \tvpmaxuq\t%xmm1, %xmm2, %xmm2", "AVX-512")):
            rows = self.lzcnt_rows()
            rows[2] = (0x09, text)
            with self.subTest(feature=feature):
                self.assertViolation(scan(rows), feature)

    def test_isa_available_avx2_level_allows_auto_vectorized_avx2(self):
        # Sha256State::Finalize: "cmpl $0x5, __isa_available; movq; jl scalar".
        for text, feature in (("vpsrlvq\t%xmm0, %xmm6, %xmm1", "AVX (VEX)"), ("vzeroupper", "AVX (VEX)"),
                              ("vpaddd\t%ymm0, %ymm1, %ymm2", "AVX/AVX2 (ymm)")):
            for immediate, allowed in (("$0x5", True), ("$0x4", False)):
                rows = self.lzcnt_rows(immediate=immediate)
                rows[2] = (0x09, text)
                with self.subTest(text=text, immediate=immediate):
                    if allowed:
                        self.assertAllowed(scan(rows), feature)
                    else:
                        self.assertViolation(scan(rows), feature)

    def test_isa_available_avx512_level_allows_only_reviewed_evex_instructions(self):
        # cgltf_calc_index_bound: "cmpl $0x6, __isa_available; jl scalar" before vpmaxuq.
        evex = "62 f2 ed 08 3f d1           \tvpmaxuq\t%xmm1, %xmm2, %xmm2"
        for immediate, allowed in (("$0x6", True), ("$0x5", False), ("$0x4", False)):
            rows = self.lzcnt_rows(immediate=immediate)
            rows[2] = (0x09, evex)
            with self.subTest(immediate=immediate):
                if allowed:
                    self.assertAllowed(scan(rows), "AVX-512")
                else:
                    self.assertViolation(scan(rows), "AVX-512")
        # vpermb is AVX512_VBMI, which __isa_available >= 6 does not prove.
        rows = self.lzcnt_rows(immediate="$0x6")
        rows[2] = (0x09, "62 f2 75 08 8d c2           \tvpermb\t%xmm2, %xmm1, %xmm0")
        self.assertViolation(scan(rows), "AVX-512")

    def test_evex_xmm_instruction_under_the_wmem_guard_is_a_violation(self):
        rows = list(WMEM_GUARDED)
        rows[2] = (0x09, "62 f2 ed 08 3f d1           \tvpmaxuq\t%xmm1, %xmm2, %xmm2")
        self.assertViolation(scan(rows), "AVX-512")

    def test_separately_bitted_vex_is_a_violation_under_the_isa_guard(self):
        # AVX-VNNI vpdpbusd has its own CPUID bit; __isa_available >= 5 does not
        # establish it, so the level-5 guard cannot excuse it.
        rows = self.lzcnt_rows()
        rows[2] = (0x09, "vpdpbusd\t%ymm0, %ymm1, %ymm2")
        self.assertViolation(scan(rows), "AVX-VNNI")

    def test_alternate_entry_past_the_guard_is_a_violation(self):
        # A tail jump from another procedure lands on the AVX2 block, bypassing
        # the guard. Without the alternate entry the block is exempt; with it,
        # the block is reachable without the guard edge, so it is a violation.
        self.assertAllowed(scan(WMEM_GUARDED), "AVX/AVX2 (ymm)")
        avx_block = TEXT + 0x09
        external = {avx_block: {TEXT + 0x9000}}  # a source outside the procedure
        self.assertViolation(scan(WMEM_GUARDED, entries=entries(branch_sources=external)), "AVX/AVX2 (ymm)")
        # A reliable address-taken AVX2 block (reloc/guard-CF/export) is likewise
        # reachable without the guard.
        self.assertViolation(scan(WMEM_GUARDED, entries=entries(other={avx_block})),
                             "AVX/AVX2 (ymm)")
        # An immediate-scanned address alone does NOT withdraw the exemption: a
        # data constant may coincide with a code address (false positives there
        # wrongly failed wmemcmp).
        self.assertAllowed(scan(WMEM_GUARDED, entries=entries(address_taken={avx_block})),
                           "AVX/AVX2 (ymm)")
        # An intra-procedure branch to the same block is a normal edge, still exempt.
        self.assertAllowed(scan(WMEM_GUARDED, entries=entries(branch_sources={avx_block: {TEXT + 0x20}})),
                           "AVX/AVX2 (ymm)")

    def test_compare_against_unreviewed_global_is_a_violation(self):
        self.assertViolation(scan(self.lzcnt_rows(address=OTHER)), "LZCNT")

    def test_indirect_jump_in_the_procedure_is_a_violation(self):
        rows = WMEM_GUARDED[:-1] + [(0x24, "jmpq\t*%rax")]
        self.assertViolation(scan(rows), "AVX/AVX2 (ymm)")

    def test_unreachable_instruction_is_a_violation(self):
        rows = WMEM_GUARDED + [(0x30, "vmovdqu\t(%rdx), %ymm2")]
        result = scan(rows)
        self.assertEqual([finding.address for finding in result.violations], [f"{TEXT + 0x30:x}"])

    def test_missing_or_ambiguous_guard_symbol_is_a_violation(self):
        self.assertViolation(scan(WMEM_GUARDED, publics=(("__isa_available", "0003:0032"),)), "AVX/AVX2 (ymm)")
        ambiguous = (("_Avx2WmemEnabled", "0003:0000"), ("_Avx2WmemEnabled", "0003:0016"))
        self.assertViolation(scan(WMEM_GUARDED, publics=ambiguous), "AVX/AVX2 (ymm)")

    def test_branch_target_between_compare_and_jump_is_a_violation(self):
        rows = [(0x00, "testl\t%edx, %edx"), (0x02, f"jne\t{target(0x0b)}"), (0x04, guard(WMEM)),
                (0x0b, "movq\t%rcx, %r9"), (0x0e, f"je\t{target(0x20)}"), (0x10, "vmovdqu\t(%rcx), %ymm0"),
                (0x14, "retq"), (0x20, "retq")]
        self.assertViolation(scan(rows), "AVX/AVX2 (ymm)")

    def test_code_outside_every_procedure_is_a_violation(self):
        self.assertViolation(scan(WMEM_GUARDED, size=0x09), "AVX/AVX2 (ymm)")

    def test_gnu_objdump_format_is_understood(self):
        lines = [f"{TEXT:016x} <.text>:",
                 f"   {TEXT:x}:\tcmpl   $0x5,0x1000(%rip)        # {ISA:#x}",
                 f"   {TEXT + 7:x}:\tjl     {TEXT + 0x10:#x}",
                 f"   {TEXT + 9:x}:\tlzcnt  %rax,%rax",
                 f"   {TEXT + 0xe:x}:\tjmp    {TEXT + 0x18:#x}",
                 f"   {TEXT + 0x10:x}:\tbsr    %rax,%rdx",
                 f"   {TEXT + 0x18:x}:\tret"]
        self.assertAllowed(checker.scan_lines("image.exe", lines, [], pdb_info()), "LZCNT")


class IntervalTests(unittest.TestCase):
    def test_condition_sets(self):
        self.assertEqual(checker._intervals("lt", 5, True), [(5, checker.INT32_MAX)])
        self.assertEqual(checker._intervals("eq", 0, True), [(checker.INT32_MIN, -1), (1, checker.INT32_MAX)])
        self.assertEqual(checker._intervals("ult", 5, False), [(0, 4)])
        self.assertEqual(checker._intervals("ult", 5, True), [(checker.INT32_MIN, -1), (5, checker.INT32_MAX)])
        self.assertEqual(sorted(checker._intervals("ule", -1, False)),
                         [(checker.INT32_MIN, -1), (0, checker.INT32_MAX)])


if __name__ == "__main__":
    unittest.main()
