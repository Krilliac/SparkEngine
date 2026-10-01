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


class CallerGuardedTests(unittest.TestCase):
    HELPER = "std::_Countl_zero_lzcnt<unsigned __int64>"
    ISA = BASE + 0x3020
    TEXT = BASE + 0x1000

    def info(self):
        procedures = [checker.Procedure(self.TEXT, self.TEXT + 0x20, "std::_Checked_x86_x64_countl_zero", False),
                      checker.Procedure(self.TEXT + 0x40, self.TEXT + 0x46, self.HELPER, False),
                      checker.Procedure(self.TEXT + 0x50, self.TEXT + 0x60, "Other", False)]
        return checker.PdbInfo([], procedures, {self.ISA: checker.REVIEWED_MSVC_GUARDS["__isa_available"]})

    def lines(self, caller=None, other=None):
        t = self.TEXT
        caller = caller or [f"{t:x}:     \tcmpl\t$0x5, 0x1000(%rip)    # {self.ISA:#x}",
                            f"{t + 7:x}:     \tjge\t{t + 0x40:#x} <.text+0x40>",
                            f"{t + 13:x}:     \tjmp\t{t + 0x30:#x} <.text+0x30>"]
        return ([f"{t:016x} <.text>:"] + caller
                + [f"{t + 0x40:x}:     \tlzcntq\t%rcx, %rax", f"{t + 0x45:x}:     \tretq"]
                + (other or [f"{t + 0x50:x}:     \tretq"]))

    def scan(self, lines, image="default"):
        image = FakeImage(b"") if image == "default" else image
        return checker.scan_lines("image.exe", lines, [], self.info(), image)

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


if __name__ == "__main__":
    unittest.main()
