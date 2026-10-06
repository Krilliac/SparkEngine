"""BLD-100: switch tables inside .text are data only when their dispatch proves them.

tools/isa_code_map.py rebuilds a PE procedure's instruction stream around the
MSVC x64 switch tables it can prove, so table bytes neither fail the ISA scan
as undecodable bytes nor hide above-floor "instructions". Every way a table
could be mistaken for code, or code for a table, must leave the bytes in the
stream. The records here are synthetic in the shape llvm-objdump prints for
real MinSizeRel images (Spark::Json::Detail::EscapeString, std::variant
visitation); a fake image supplies the table bytes.
"""

import struct
import unittest

from Tests.Tools.test_check_isa_baseline import checker

cm = checker.code_map
BASE = 0x140000000
P = BASE + 0x1000  # procedure start


class FakeImage:
    def __init__(self, data: bytes, start: int = P, redecoded=None, data_sections=b""):
        self.image_base = BASE
        self._start, self._data = start, data
        self.redecoded = redecoded or {}
        self.calls = []
        self._data_sections = data_sections

    def read(self, address, size):
        offset = address - self._start
        if offset < 0 or offset + size > len(self._data):
            return None
        return self._data[offset:offset + size]

    def section_bounds(self, address):
        return None

    def set_entered(self, addresses):
        self.entered = sorted(addresses)

    def entered_within(self, lo, hi):
        return any(lo <= a < hi for a in getattr(self, "entered", ()))

    def exec_ranges(self):
        return [(self._start, self._start + len(self._data))]

    def set_immediates(self, addresses, classifier):
        self._immediates = sorted(addresses); self._classifier = classifier

    def immediates_within(self, lo, hi):
        return [a for a in getattr(self, "_immediates", ()) if lo <= a < hi]

    def table_masks_code(self, lo, hi):
        return False

    def redecode(self, start, stop):
        self.calls.append((start, stop))
        return [r for r in self.redecoded.get(start, []) if r.address < stop]

    def data_contains(self, needle):
        return needle in self._data_sections


def rec(offset, size, mnemonic, operands="", annotation=None):
    return cm.Record(P + offset, size, mnemonic, operands, annotation, False, f"{mnemonic} {operands}")


def rva(offset):
    return P + offset - BASE


# EscapeString's shape: bound check, byte index table, RVA jump table.
def switch_records(jump_table=0x38, index_table=0x40, bound=2):
    return [
        rec(0x00, 7, "leaq", "-0x1007(%rip), %r13", BASE),
        rec(0x07, 3, "cmpl", f"${bound:#x}, %eax"),
        rec(0x0a, 2, "ja", f"{P + 0x30:#x} <.text+0x30>"),
        rec(0x0c, 2, "cltq"),
        rec(0x0e, 9, "movzbl", f"{rva(index_table):#x}(%rax,%r13), %eax"),
        rec(0x17, 8, "movl", f"{rva(jump_table):#x}(%r13,%rax,4), %ecx"),
        rec(0x1f, 3, "addq", "%r13, %rcx"),
        rec(0x22, 2, "jmpq", "*%rcx"),
        rec(0x24, 5, "movl", "$0x1, %eax"),
        rec(0x29, 1, "retq"),
        rec(0x2a, 5, "movl", "$0x2, %eax"),
        rec(0x2f, 1, "retq"),
        rec(0x30, 2, "xorl", "%eax, %eax"),
        rec(0x32, 1, "retq"),
        *[rec(offset, 1, "int3") for offset in range(0x33, 0x38)],
        # The linear sweep decodes the tables as instructions.
        rec(0x38, 5, "vpaddd", "%ymm0, %ymm1, %ymm2"),
        rec(0x3d, 1, "<undecodable>"),
        rec(0x3e, 5, "addb", "%al, (%rax)"),
    ]


def switch_bytes(entries=(0x24, 0x2a), selectors=(0, 1, 1)):
    data = bytearray(b"\x90" * 0x38)
    data += b"".join(struct.pack("<I", rva(entry)) for entry in entries)
    data += bytes(selectors)
    return bytes(data)


END = P + 0x43


def mapped(records, data, end=END, **image_options):
    image = FakeImage(data, **image_options)
    return cm.map_procedure(records, P, end, image), image


class BoundProofTests(unittest.TestCase):
    def test_zero_extended_byte_index_preserves_a_proven_bound(self):
        # MSVC 14.51 uses this form for __GSHandlerCheckCommon: the byte
        # compared by the ja is copied into the table index register.
        records = [
            rec(0, 3, "cmpb", "$0x20, %cl"),
            rec(3, 2, "ja", f"{P + 0x30:#x} <.text+0x30>"),
            rec(5, 3, "movzbl", "%cl, %eax"),
            rec(8, 9, "movzbl", f"{rva(0x40):#x}(%r13,%rax), %eax"),
        ]
        preds = [[], [0], [1], [2]]
        self.assertEqual(cm._bound(records, 3, "a", set()), 33)
        self.assertEqual(cm._bound_all_paths(records, preds, 3, "a"), 33)

        # A bound on a different byte cannot prove the table's extent.
        records[2] = rec(5, 3, "movzbl", "%dl, %eax")
        self.assertIsNone(cm._bound(records, 3, "a", set()))
        self.assertIsNone(cm._bound_all_paths(records, preds, 3, "a"))

    def test_inequality_branch_fallthrough_proves_one_index_value(self):
        records = [
            rec(0, 3, "cmpb", "$0x20, %cl"),
            rec(3, 2, "jne", f"{P + 0x30:#x} <.text+0x30>"),
            rec(5, 3, "movzbl", "%cl, %eax"),
            rec(8, 9, "movzbl", f"{rva(0x40):#x}(%r13,%rax), %eax"),
        ]
        preds = [[], [0], [1], [2]]
        self.assertEqual(cm._bound_all_paths(records, preds, 3, "a"), 33)

        # The opposite fall-through means the index is anything except 32.
        records[1] = rec(3, 2, "je", f"{P + 0x30:#x} <.text+0x30>")
        self.assertIsNone(cm._bound_all_paths(records, preds, 3, "a"))

    def test_full_register_xor_proves_zero_but_byte_xor_does_not(self):
        records = [rec(0, 2, "xorl", "%eax, %eax"),
                   rec(2, 9, "movzbl", f"{rva(0x40):#x}(%r13,%rax), %eax")]
        preds = [[], [0]]
        self.assertEqual(cm._bound_all_paths(records, preds, 1, "a"), 1)
        records[0] = rec(0, 2, "xorb", "%al, %al")
        self.assertIsNone(cm._bound_all_paths(records, preds, 1, "a"))


class SwitchTableTests(unittest.TestCase):
    def assertTableBytesAreData(self, result):
        self.assertEqual([(t.start - P, t.end - P, t.kind) for t in result.tables],
                         [(0x38, 0x40, "jump"), (0x40, 0x43, "index")])
        self.assertEqual(result.records[-1].address, P + 0x37)
        self.assertNotIn("vpaddd", [r.mnemonic for r in result.records])
        self.assertNotIn("<undecodable>", [r.mnemonic for r in result.records])
        self.assertEqual(result.dispatch_targets, {P + 0x22: (P + 0x24, P + 0x2a)})

    def test_bounded_index_and_jump_tables_are_proven(self):
        result, image = mapped(switch_records(), switch_bytes())
        self.assertTableBytesAreData(result)
        self.assertEqual(image.calls, [])

    def assertTablesRejected(self, result):
        self.assertEqual(result.tables, [])
        self.assertTrue({"vpaddd", "<undecodable>"} & {r.mnemonic for r in result.records})

    def test_direct_branch_into_a_table_rejects_it(self):
        records = switch_records()
        records[12] = rec(0x30, 2, "jmp", f"{P + 0x3e:#x} <.text+0x3e>")
        self.assertTablesRejected(mapped(records, switch_bytes())[0])

    def test_fallthrough_into_a_table_rejects_it(self):
        records = [r for r in switch_records() if r.mnemonic != "int3"]
        records[13] = rec(0x32, 6, "movl", "$0x3, %eax")  # replaces the retq; falls into 0x38
        self.assertTablesRejected(mapped(records, switch_bytes())[0])

    def test_entry_outside_the_procedure_rejects_the_table(self):
        self.assertTablesRejected(mapped(switch_records(), switch_bytes(entries=(0x24, 0x900)))[0])

    def test_entry_inside_an_instruction_rejects_the_table(self):
        self.assertTablesRejected(mapped(switch_records(), switch_bytes(entries=(0x24, 0x25)))[0])

    def test_base_must_be_the_image_base_on_every_path(self):
        for wrong in (rec(0x00, 7, "leaq", "-0x1007(%rip), %r13", BASE + 0x10),  # not the image base
                      rec(0x00, 7, "movq", "%rdx, %r13")):
            records = switch_records()
            records[0] = wrong
            with self.subTest(wrong=wrong.text):
                self.assertTablesRejected(mapped(records, switch_bytes())[0])

    def test_a_case_entering_after_the_bound_check_rejects_the_table(self):
        # The second case jumps back to the cltq, past "cmpl $0x2; ja".
        records = switch_records()
        records[10] = rec(0x2a, 5, "jmp", f"{P + 0x0c:#x} <.text+0xc>")
        result = mapped(records, switch_bytes())[0]
        self.assertTablesRejected(result)

    def test_a_case_that_enters_after_the_bound_check_rejects_the_table(self):
        # The table names the cltq as a case, which enters past "cmpl $0x2; ja".
        self.assertTablesRejected(mapped(switch_records(), switch_bytes(entries=(0x24, 0x0c)))[0])

    def test_a_larger_bound_cannot_claim_following_code(self):
        # cmp $0x5 claims six selectors, past the procedure end.
        self.assertTablesRejected(mapped(switch_records(bound=5), switch_bytes())[0])

    def test_out_of_step_entry_is_decoded_again(self):
        records = switch_records()
        shifted = [rec(0x01, 6, "<undecodable>")] + records[1:]
        result, image = mapped(shifted, switch_bytes(), redecoded={P: records})
        self.assertEqual(image.calls[0], (P, END))
        self.assertTableBytesAreData(result)

    def test_unexplained_bytes_stay_undecodable(self):
        records = switch_records()[:-3]  # nothing decodes the table bytes and no table is proven
        records[5] = rec(0x17, 8, "movl", f"{rva(0x38):#x}(%r13,%rax,8), %ecx")  # scale 8: not the idiom
        result, image = mapped(records, switch_bytes())
        self.assertEqual(result.tables, [])
        self.assertTrue(any(r.mnemonic == "<undecodable>" and r.address == P + 0x38 for r in result.records))
        self.assertLessEqual(len(image.calls), cm.MAX_REDECODES)

    def test_unbounded_variant_table_takes_the_minus_one_slot(self):
        # std::variant: "movsbq 0x20(%rdi), %rax" indexes the table with -1..n-1.
        records = [
            rec(0x00, 7, "leaq", "-0x1007(%rip), %r13", BASE),
            rec(0x07, 5, "movsbq", "0x20(%rdi), %rax"),
            rec(0x0c, 8, "movl", f"{rva(0x24):#x}(%r13,%rax,4), %eax"),
            rec(0x14, 3, "addq", "%r13, %rax"),
            rec(0x17, 2, "jmpq", "*%rax"),
            rec(0x19, 5, "movl", "$0x1, %eax"),
            rec(0x1e, 1, "retq"),
            rec(0x1f, 1, "retq"),
            rec(0x20, 4, "vpaddd", "%xmm0, %xmm1, %xmm2"),  # the -1 slot, decoded as an instruction
            rec(0x24, 4, "<undecodable>"),
            rec(0x28, 4, "<undecodable>"),
        ]
        data = bytearray(b"\x90" * 0x1c)
        # The bytes before the -1 slot (the movl/retq at 0x19-0x1f) also name an
        # instruction start; only reachability keeps them out of the table.
        data += struct.pack("<I", rva(0x19))
        data += b"".join(struct.pack("<I", rva(entry)) for entry in (0x1f, 0x19, 0x1f))
        result, _ = mapped(records, bytes(data), end=P + 0x2c)
        self.assertEqual([(t.start - P, t.end - P) for t in result.tables], [(0x20, 0x2c)])
        self.assertEqual(result.records[-1].address, P + 0x1f)

        # The slot before -1 is reachable code (the retq at 0x1f), so it is never taken.
        self.assertTrue(all(t.start >= P + 0x20 for t in result.tables))


class ExternalEntryTests(unittest.TestCase):
    """A proven table may not contain bytes reached as code from elsewhere."""

    def test_cross_procedure_entry_into_a_table_rejects_it(self):
        # Baseline: the table is proven data.
        baseline, _ = mapped(switch_records(), switch_bytes())
        self.assertTrue(baseline.tables)
        # A tail jump from another procedure targets a byte inside the jump
        # table (its slots also decode as instructions). The bytes are code, so
        # the table must be rejected and the bytes classified.
        image = FakeImage(switch_bytes(), start=P)
        image.set_entered([P + 0x3a])  # inside the jump table [0x38, 0x40)
        result = cm.map_procedure(switch_records(), P, END, image)
        self.assertEqual(result.tables, [])
        self.assertTrue(any(r.mnemonic in ("vpaddd", "<undecodable>") for r in result.records))

    def test_address_taken_into_the_index_table_rejects_it(self):
        image = FakeImage(switch_bytes(), start=P)
        image.set_entered([P + 0x41])  # inside the index table [0x40, 0x43)
        self.assertEqual(cm.map_procedure(switch_records(), P, END, image).tables, [])

    def test_entry_outside_any_table_leaves_the_tables_proven(self):
        image = FakeImage(switch_bytes(), start=P)
        image.set_entered([P + 0x24, P + 0x30])  # real case targets, not table bytes
        self.assertTrue(cm.map_procedure(switch_records(), P, END, image).tables)


class CallerGuardedTests(unittest.TestCase):
    HELPER = "std::_Countl_zero_lzcnt<unsigned __int64>"
    ISA = BASE + 0x3020
    TEXT = BASE + 0x1000

    def info(self):
        procedures = [checker.Procedure(self.TEXT, self.TEXT + 0x20, "std::_Checked_x86_x64_countl_zero", False),
                      checker.Procedure(self.TEXT + 0x40, self.TEXT + 0x46, self.HELPER, False),
                      checker.Procedure(self.TEXT + 0x50, self.TEXT + 0x60, "Other", False)]
        return checker.PdbInfo([], procedures, {self.ISA: checker.REVIEWED_MSVC_GUARDS["__isa_available"]},
                               frozenset({checker.REVIEWED_TOOLSET}))

    def lines(self, caller=None, other=None):
        t = self.TEXT
        caller = caller or [f"{t:x}:     \tcmpl\t$0x5, 0x1000(%rip)    # {self.ISA:#x}",
                            f"{t + 7:x}:     \tjge\t{t + 0x40:#x} <.text+0x40>",
                            f"{t + 13:x}:     \tjmp\t{t + 0x30:#x} <.text+0x30>"]
        return ([f"{t:016x} <.text>:"] + caller
                + [f"{t + 0x40:x}:     \tlzcntq\t%rcx, %rax", f"{t + 0x45:x}:     \tretq"]
                + (other or [f"{t + 0x50:x}:     \tretq"]))

    def scan(self, lines, image="default", entries=None):
        image = FakeImage(b"") if image == "default" else image
        return checker.scan_lines("image.exe", lines, [], self.info(), image, entries)

    def entries(self, **fields):
        base = dict(image_base=BASE, branch_sources={}, computed_targets=set(), other=set())
        base.update(fields)
        return checker._ImageEntries(**base)

    def test_computed_rva_reference_keeps_the_finding(self):
        # "mov $RVA,%reg; add imagebase; call *reg" -- an address-taken helper
        # reference the per-call branch logic never sees. It must withdraw the
        # exemption just like a directly taken address.
        entries = self.entries(computed_targets={self.TEXT + 0x40})
        self.assertTrue(self.scan(self.lines(), entries=entries).violations)
        other_entry = self.entries(other={self.TEXT + 0x40})  # reloc/export/guard-CF reference
        self.assertTrue(self.scan(self.lines(), entries=other_entry).violations)

    def test_closed_guarded_reference_set_is_still_allowed(self):
        # With entries present but no reference to the helper beyond the guarded
        # tail jump, the exemption holds.
        result = self.scan(self.lines(), entries=self.entries())
        self.assertEqual(result.violations, [])
        self.assertEqual(result.allowed["LZCNT"], 1)

    def test_helper_reached_only_through_the_guard_edge_is_allowed(self):
        result = self.scan(self.lines())
        self.assertEqual(result.violations, [])
        self.assertEqual(result.allowed["LZCNT"], 1)

    def test_unguarded_call_keeps_the_finding(self):
        other = [f"{self.TEXT + 0x50:x}:     \tcallq\t{self.TEXT + 0x40:#x} <.text+0x40>",
                 f"{self.TEXT + 0x55:x}:     \tretq"]
        self.assertEqual([f.feature for f in self.scan(self.lines(other=other)).violations], ["LZCNT"])

    def test_weaker_guard_keeps_the_finding(self):
        caller = self.lines()[1:4]
        caller[0] = caller[0].replace("$0x5", "$0x4")
        self.assertEqual([f.feature for f in self.scan(self.lines(caller=caller)).violations], ["LZCNT"])

    def test_taken_address_keeps_the_finding(self):
        other = [f"{self.TEXT + 0x50:x}:     \tleaq\t-0x17(%rip), %rax    # {self.TEXT + 0x40:#x}",
                 f"{self.TEXT + 0x57:x}:     \tretq"]
        self.assertTrue(self.scan(self.lines(other=other)).violations)

    def test_address_in_a_data_section_keeps_the_finding(self):
        pointer = struct.pack("<Q", self.TEXT + 0x40)
        self.assertTrue(self.scan(self.lines(), FakeImage(b"", data_sections=b"xx" + pointer)).violations)
        self.assertTrue(self.scan(self.lines(), None).violations)  # nothing to rule data references out

    def test_other_instruction_in_the_helper_keeps_the_finding(self):
        lines = self.lines()
        lines[4] = f"{self.TEXT + 0x40:x}:     \ttzcntq\t%rcx, %rax"
        lines.insert(5, f"{self.TEXT + 0x44:x}:     \tlzcntl\t%ecx, %eax")
        self.assertTrue(self.scan(lines).violations)


class ComputedTargetImage:
    """Minimal image for the entry collector: only section membership matters."""

    def __init__(self, lo=BASE, hi=BASE + 0x100000):
        self.image_base = BASE
        self._lo, self._hi = lo, hi

    def section_bounds(self, address):
        return (self._lo, self._hi) if self._lo <= address < self._hi else None


class ComputedTargetTests(unittest.TestCase):
    """The entry collector follows an immediate address into jmp/call *reg."""

    def collect(self, records):
        collector = checker._EntryCollector(ComputedTargetImage())
        collector.record(records)
        return collector

    def test_movabs_then_indirect_call_is_a_computed_target(self):
        # finding 2(a): movabs AVXblock,%rax; call *%rax on the scalar path.
        avx = P + 0x400
        collector = self.collect([rec(0x00, 10, "movabs", f"$0x{avx:x}, %rax"),
                                  rec(0x0a, 2, "call", "*%rax"), rec(0x0c, 1, "retq")])
        self.assertEqual(collector.computed_targets, {avx})
        self.assertIn(avx, collector.immediate_addresses)

    def test_movabs_adjusted_by_dec_resolves_to_the_helper(self):
        # finding 2(b): movabs helper+1,%rax; dec %rax; call *%rax.
        helper = P + 0x500
        collector = self.collect([rec(0x00, 10, "movabs", f"$0x{helper + 1:x}, %rax"),
                                  rec(0x0a, 3, "dec", "%rax"), rec(0x0d, 2, "call", "*%rax")])
        self.assertEqual(collector.computed_targets, {helper})

    def test_mov_rva_plus_image_base_idiom(self):
        target = BASE + 0x400
        collector = self.collect([rec(0x00, 5, "mov", "$0x400, %eax"),
                                  rec(0x05, 7, "lea", "0x0(%rip), %rbx", annotation=BASE),
                                  rec(0x0c, 3, "add", "%rbx, %rax"), rec(0x0f, 2, "jmp", "*%rax")])
        self.assertEqual(collector.computed_targets, {target})

    def test_stored_pointer_counts_when_the_procedure_has_an_indirect_branch(self):
        x = P + 0x600
        records = [rec(0x00, 10, "movabs", f"$0x{x:x}, %rax"), rec(0x0a, 3, "mov", "%rax, (%rcx)"),
                   rec(0x0d, 2, "jmp", "*%rdx"), rec(0x0f, 1, "retq")]
        self.assertIn(x, self.collect(records).computed_targets)
        # Without any indirect branch in the procedure, a stored pointer is not a target.
        self.assertEqual(self.collect(records[:2] + [rec(0x0d, 1, "retq")]).computed_targets, set())

    def test_clobbered_or_cross_block_register_is_not_a_target(self):
        avx = P + 0x400
        # A branch target (leader) between the load and the use clears tracking.
        records = [rec(0x00, 10, "movabs", f"$0x{avx:x}, %rax"), rec(0x0a, 2, f"je", f"0x{P + 0x0c:x}"),
                   rec(0x0c, 2, "call", "*%rax"), rec(0x0e, 1, "retq")]
        self.assertEqual(self.collect(records).computed_targets, set())
        # A plain register overwrite clears it too.
        records = [rec(0x00, 10, "movabs", f"$0x{avx:x}, %rax"), rec(0x0a, 2, "xorl", "%eax, %eax"),
                   rec(0x0c, 2, "call", "*%rax")]
        self.assertEqual(self.collect(records).computed_targets, set())


class TableMasksCodeTests(unittest.TestCase):
    """A materialized address inside proven-table bytes rejects the table only
    when the bytes there decode as an above-floor or undecodable instruction."""

    def image(self, decode):
        import tempfile
        handle, path = tempfile.mkstemp(suffix=".bin")
        import os
        with os.fdopen(handle, "wb") as out:
            out.write(b"\x00" * 0x100)
        img = cm.ImageBytes(path, BASE, [(P, 0x100, 0)], decode)
        os.unlink(path)
        return img

    def test_above_floor_bytes_at_a_pointed_offset_reject_the_table(self):
        slot = P + 0x40
        img = self.image(lambda s, e: [cm.Record(s, 3, "vzeroupper", "", None, False, "vzeroupper")])
        img.set_immediates({slot}, checker.classify)
        self.assertTrue(img.table_masks_code(P + 0x38, P + 0x48))

    def test_undecodable_bytes_at_a_pointed_offset_keep_the_table(self):
        # A byte/jump table's own bytes decode as undecodable garbage; a data
        # constant pointing into them is tolerated here. (A computed jump INTO
        # them is rejected separately as an image-wide code entry.)
        slot = P + 0x40
        img = self.image(lambda s, e: [cm.Record(s, 1, "<undecodable>", "", None, False, "(bad)")])
        img.set_immediates({slot}, checker.classify)
        self.assertFalse(img.table_masks_code(P + 0x38, P + 0x48))

    def test_floor_safe_bytes_keep_the_table(self):
        # A data constant that merely coincides with table bytes decoding as a
        # floor instruction before a terminator does not reject the table.
        slot = P + 0x40
        img = self.image(lambda s, e: [cm.Record(s, 1, "retq", "", None, False, "retq")])
        img.set_immediates({slot}, checker.classify)
        self.assertFalse(img.table_masks_code(P + 0x38, P + 0x48))

    def test_no_pointer_into_the_table_keeps_it(self):
        img = self.image(lambda s, e: [cm.Record(s, 3, "vzeroupper", "", None, False, "vzeroupper")])
        img.set_immediates({P + 0x200}, checker.classify)
        self.assertFalse(img.table_masks_code(P + 0x38, P + 0x48))


class CoverageTests(unittest.TestCase):
    """_verify_coverage fails unless every executable file-backed byte is classified."""

    class Image:
        def __init__(self, ranges):
            self._ranges = ranges

        def exec_ranges(self):
            return self._ranges

    def verify(self, covered, ranges=((P, P + 0x100),)):
        result = checker.ScanResult(path="image.exe")
        result.covered = list(covered)
        checker._verify_coverage(result, self.Image(list(ranges)))

    def test_complete_coverage_passes(self):
        self.verify([(P, P + 0x80), (P + 0x80, P + 0x100)])

    def test_a_gap_fails(self):
        # A record without raw bytes (size 0) leaves [P+0x40, P+0x80) uncovered.
        with self.assertRaises(RuntimeError):
            self.verify([(P, P + 0x40), (P + 0x80, P + 0x100)])

    def test_a_trailing_gap_fails(self):
        with self.assertRaises(RuntimeError):
            self.verify([(P, P + 0xC0)])

    def test_a_leading_gap_fails(self):
        with self.assertRaises(RuntimeError):
            self.verify([(P + 0x10, P + 0x100)])

    def test_overlap_still_requires_full_coverage(self):
        # Folded procedures may overlap; coverage still holds if nothing is missing.
        self.verify([(P, P + 0x90), (P + 0x40, P + 0x100)])


if __name__ == "__main__":
    unittest.main()
