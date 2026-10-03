"""Which bytes of an MSVC x64 PE procedure are instructions, and which are data.

BLD-100: tools/check_isa_baseline.py disassembles every executable byte of a
shipped image. A linear sweep cannot tell code from data, and MSVC x64 places
switch tables inside .text, inside the extent its PDB records for the
procedure. Decoded as instructions, those tables print as undecodable bytes or
as arbitrary "instructions" (including VEX/EVEX ones), and they can push the
sweep out of step with the real instruction stream that follows them.

This module proves, per procedure, that specific byte ranges are switch tables,
and rebuilds the instruction stream around them. Nothing here exempts an
instruction: it only decides which bytes are instructions. Bytes it cannot
explain stay instructions (or undecodable records), so every failure to prove
a table leaves the scan failing.

A table is accepted only when all of this holds:

* the dispatch is MSVC's x64 idiom, matched instruction by instruction:
      leaq  -X(%rip), %base            # the image base (from the PE header)
      ...
      [movzbl IDX(%base,%i), %j]       # optional byte index table
      movl  JT(%base,%j,4), %r32       # 32-bit RVA jump table
      addq  %base, %r64
      jmpq  *%r64
  and every definition of %base that reaches the load (walked backwards over
  the procedure's control-flow graph) is that image-base leaq;
* its extent comes from the bound check MSVC emits ("cmp $N, idx" + ja/jae on
  the straight-line path to the load, nothing entering in between), or, when
  MSVC omitted the bound because the default is unreachable (std::variant
  visitation), from the run of consecutive entries that each name an
  instruction start of the same procedure, forwards and (for std::variant's
  index -1) backwards, never over bytes of code reachable from the entry;
* every jump-table entry names an instruction boundary of the rebuilt stream in
  the same procedure, outside every table;
* no instruction of the rebuilt stream overlaps a table, no direct branch
  targets a table, and no reachable instruction falls through into one.

The rebuilt stream starts at the procedure entry, steps over each table, and
re-decodes (with the same disassembler) wherever the sweep lost step. A 0x90
(nop) or 0xCC (int3) byte between tables, or between a table and the
procedure end, is accepted as one byte: both are complete, floor-safe
one-byte instructions whatever their purpose.
"""

from __future__ import annotations

import re
import struct
from collections.abc import Callable
from dataclasses import dataclass

# ---------------------------------------------------------------------------
# AT&T operand helpers shared with the guard-dominance analysis.

REG32 = {"eax": "a", "ebx": "b", "ecx": "c", "edx": "d", "esi": "si", "edi": "di", "ebp": "bp", "esp": "sp",
         **{f"r{n}d": f"r{n}" for n in range(8, 16)}}
REG_FAMILY = {
    **{name: family for name, family in REG32.items()},
    **{"r" + name[1:]: family for name, family in REG32.items() if name.startswith("e")},
    **{f"r{n}": f"r{n}" for n in range(8, 16)}, **{f"r{n}w": f"r{n}" for n in range(8, 16)},
    **{f"r{n}b": f"r{n}" for n in range(8, 16)},
    "ax": "a", "bx": "b", "cx": "c", "dx": "d", "si": "si", "di": "di", "bp": "bp", "sp": "sp",
    "al": "a", "ah": "a", "bl": "b", "bh": "b", "cl": "c", "ch": "c", "dl": "d", "dh": "d",
    "sil": "si", "dil": "di", "bpl": "bp", "spl": "sp",
}
RETURN_FAMILIES = {"a", "d"}
BRANCH_TARGET_RE = re.compile(r"^(?:0x)?([0-9a-fA-F]+)\b")
MEMORY_RE = re.compile(r"^(-?(?:0x)?[0-9a-fA-F]+)?\((%\w+)?(?:,(%\w+)(?:,(\d))?)?\)$")

TERMINATORS = {"ret", "retq", "retl", "ud2", "int3", "hlt"}
UNCONDITIONAL_JUMPS = {"jmp", "jmpq"}
PADDING_BYTES = {0x90, 0xCC}  # nop, int3
MAX_TABLE_ENTRIES = 4096
MAX_ROUNDS = 8  # table/rebuild rounds per procedure before giving up on its tables
MAX_REDECODES = 16  # per region; after that, unexplained bytes are reported without retrying


def split_operands(operands: str) -> list[str]:
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


def family(operand: str | None) -> str | None:
    if not operand or not operand.startswith("%"):
        return None
    return REG_FAMILY.get(operand[1:].lower())


def is_jump(mnemonic: str) -> bool:
    return mnemonic.startswith("j") or mnemonic.startswith("loop")


def direct_target(mnemonic: str, operands: str) -> int | None:
    """Target of a direct jmp/jcc/call, or None for indirect and other instructions."""
    if not (is_jump(mnemonic) or mnemonic in ("call", "callq")) or operands.startswith("*"):
        return None
    match = BRANCH_TARGET_RE.match(operands)
    return int(match.group(1), 16) if match else None


def falls_through(mnemonic: str) -> bool:
    return mnemonic not in TERMINATORS and mnemonic not in UNCONDITIONAL_JUMPS


def continues(insn: "Record", noreturn: frozenset[int]) -> bool:
    """Execution can continue at the next instruction (a direct call to a
    procedure the PDB flags noreturn does not return)."""
    if not falls_through(insn.mnemonic):
        return False
    return not (insn.mnemonic in ("call", "callq") and direct_target(insn.mnemonic, insn.operands) in noreturn)


# Registers an instruction writes without naming them as its destination.
_IMPLICIT_WRITES = {
    "cltq": {"a"}, "cwtl": {"a"}, "cbtw": {"a"}, "cqto": {"d"}, "cltd": {"d"}, "cwtd": {"d"},
    "cpuid": {"a", "b", "c", "d"}, "rdtsc": {"a", "d"}, "rdtscp": {"a", "c", "d"}, "xgetbv": {"a", "d"},
    "syscall": {"c", "r11"}, "leave": {"bp", "sp"},
}
_STRING_OPS = re.compile(r"^(movs|stos|lods|scas|cmps|ins|outs)[bwlq]?$")
_NON_WRITING = ("cmp", "test", "push", "ucomi", "comi", "ptest", "vptest", "nop")


def written_families(insn: "Record") -> set[str] | None:
    """Register families an instruction may write; None when it cannot be told."""
    mnemonic, operands = insn.mnemonic, split_operands(insn.operands)
    if mnemonic == "<undecodable>":
        return None
    written = set(_IMPLICIT_WRITES.get(mnemonic, ()))
    if mnemonic in ("call", "callq"):
        # A call defines the return registers. The other volatile registers it
        # may clobber are not values compiled code can rely on, and MSVC keeps
        # a value in one across a call to a callee it knows preserves it, so a
        # clobber is not a definition that can reach a later use.
        return written | RETURN_FAMILIES
    if _STRING_OPS.match(mnemonic):
        return written | {"si", "di", "c", "a"}
    if mnemonic.rstrip("bwlq") in ("mul", "div", "idiv") or (mnemonic.startswith("imul") and len(operands) == 1):
        written |= {"a", "d"}
    if mnemonic.startswith(("xchg", "xadd", "cmpxchg")):
        written |= {family(op) for op in operands if family(op)} | {"a"}
    if is_jump(mnemonic) or mnemonic.startswith(_NON_WRITING) or mnemonic in ("bt", "btl", "btq", "btw"):
        return written
    if operands and family(operands[-1]):
        written.add(family(operands[-1]))
    return written


# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Record:
    """One disassembled instruction (or undecodable byte) with its encoded size."""

    address: int
    size: int
    mnemonic: str  # lower case; "<undecodable>" when the disassembler could not decode it
    operands: str
    annotation: int | None
    evex: bool = False
    text: str = ""

    @property
    def end(self) -> int:
        return self.address + self.size


@dataclass(frozen=True)
class SwitchTable:
    start: int
    end: int
    dispatch: int  # address of the indirect jmp
    kind: str  # "jump" (32-bit RVAs) or "index" (bytes selecting a jump-table entry)
    targets: tuple[int, ...] = ()


@dataclass
class CodeMap:
    records: list[Record]  # the rebuilt instruction stream, in address order
    tables: list[SwitchTable]
    dispatch_targets: dict[int, tuple[int, ...]]  # indirect jmp address -> case addresses
    padding: int = 0  # 0x90/0xCC bytes accepted between tables / before the end


class ImageBytes:
    """Executable-section file bytes of a PE image."""

    def __init__(self, path: str, image_base: int, sections: list[tuple[int, int, int]],
                 redecode: Callable[[int, int], list[Record]],
                 data_sections: list[tuple[int, int, int]] = ()):
        # sections: (virtual address, size backed by file bytes, file offset)
        self.image_base = image_base
        self._redecode = redecode
        self._sections = []
        self._data = []
        self._entered: list[int] = []  # sorted; set via set_entered()
        self._immediates: list[int] = []  # sorted materialized image addresses
        self._classifier = None           # check_isa_baseline.classify, for table_masks_code
        with open(path, "rb") as handle:
            for target, ranges in ((self._sections, sections), (self._data, data_sections)):
                for address, size, offset in ranges:
                    handle.seek(offset)
                    data = handle.read(size)
                    if len(data) != size:
                        raise RuntimeError(f"{path} is truncated inside a section")
                    target.append((address, data))

    def data_contains(self, needle: bytes) -> bool:
        """True when a non-executable section's file bytes contain needle."""
        return any(needle in data for _, data in self._data)

    def set_entered(self, addresses) -> None:
        """Record image-wide code entry addresses (sorted) for table rejection."""
        self._entered = sorted(addresses)

    def entered_within(self, lo: int, hi: int) -> bool:
        """True when any recorded entry address lies in [lo, hi)."""
        import bisect as _bisect
        index = _bisect.bisect_left(self._entered, lo)
        return index < len(self._entered) and self._entered[index] < hi

    def set_immediates(self, addresses, classifier) -> None:
        """Record materialized image addresses (sorted) and the ISA classifier.

        Used by table_masks_code to decide whether a data constant that points
        into proven-table bytes actually names an above-floor instruction.
        """
        self._immediates = sorted(addresses)
        self._classifier = classifier

    def immediates_within(self, lo: int, hi: int) -> list[int]:
        import bisect as _bisect
        start = _bisect.bisect_left(self._immediates, lo)
        stop = _bisect.bisect_left(self._immediates, hi)
        return self._immediates[start:stop]

    def table_masks_code(self, lo: int, hi: int) -> bool:
        """True when a materialized image address points into [lo, hi) and the
        bytes there decode as an above-floor instruction before a terminator --
        a real above-floor sequence a pointer names, not a data constant that
        merely coincides with floor-safe table bytes.

        Undecodable bytes at a pointed offset do NOT reject the table: a jump or
        index table's own bytes decode as undecodable garbage (observed for MSVC
        byte index tables), and a data constant pointing into them is the
        coincidence this check is meant to tolerate. A computed *jump* into such
        bytes is a different thing and is rejected separately, as an image-wide
        code entry (entered_within), not here."""
        if self._classifier is None:
            return False
        for address in self.immediates_within(lo, hi):
            for record in self.redecode(address, hi):
                if not (lo <= record.address < hi):
                    break
                if record.mnemonic == "<undecodable>":
                    break  # table data, not proof of an above-floor instruction
                feature = self._classifier(record.mnemonic, record.operands, record.evex)
                if feature is not None and feature != "TZCNT":
                    return True
                if record.mnemonic in TERMINATORS or record.mnemonic in UNCONDITIONAL_JUMPS:
                    break
        return False

    def read(self, address: int, size: int) -> bytes | None:
        for start, data in self._sections:
            if start <= address and address + size <= start + len(data):
                return data[address - start:address - start + size]
        return None

    def section_bounds(self, address: int) -> tuple[int, int] | None:
        for start, data in self._sections:
            if start <= address < start + len(data):
                return start, start + len(data)
        return None

    def exec_ranges(self) -> list[tuple[int, int]]:
        """(start, end) of every executable file-backed section, in address order."""
        return sorted((start, start + len(data)) for start, data in self._sections)

    def redecode(self, start: int, stop: int) -> list[Record]:
        return self._redecode(start, stop)


# ---------------------------------------------------------------------------
# Switch dispatch recognition.


def _predecessors(records: list[Record], index_of: dict[int, int],
                  extra_edges: dict[int, tuple[int, ...]], noreturn: frozenset[int]) -> list[list[int]]:
    preds: list[list[int]] = [[] for _ in records]
    for i, insn in enumerate(records):
        if continues(insn, noreturn) and i + 1 < len(records):
            preds[i + 1].append(i)
        target = direct_target(insn.mnemonic, insn.operands)
        if target is not None and insn.mnemonic not in ("call", "callq") and target in index_of:
            preds[index_of[target]].append(i)
        for case in extra_edges.get(insn.address, ()):
            if case in index_of:
                preds[index_of[case]].append(i)
    return preds


def _base_reaches_as_image_base(records: list[Record], preds: list[list[int]], load: int, base: str,
                                image_base: int) -> bool:
    """Every definition of base that reaches records[load] is "leaq ...(%rip), base # image base"."""
    # An instruction with no known predecessor is entered through an indirect
    # jump of this procedure (a case of a switch whose table is not resolved
    # yet, possibly this one), so its predecessors are those jumps.
    indirect = [i for i, insn in enumerate(records) if is_jump(insn.mnemonic) and insn.operands.startswith("*")]
    seen, stack = set(), list(preds[load]) or list(indirect)
    if not stack:
        return False
    while stack:
        node = stack.pop()
        if node in seen:
            continue
        seen.add(node)
        insn = records[node]
        written = written_families(insn)
        if written is None:
            return False
        if base in written:
            operands = split_operands(insn.operands)
            if not (insn.mnemonic == "leaq" and insn.annotation == image_base and len(operands) == 2
                    and operands[0].endswith("(%rip)") and family(operands[1]) == base):
                return False
            continue
        if node == 0:
            return False  # the procedure entry leaves base undefined on this path
        stack.extend(preds[node] or indirect)
    return True


def _bound(records: list[Record], before: int, index: str, leaders: set[int]) -> int | None:
    """Entry count from "cmp $N, idx; ja/jae default" on the straight-line path to records[before].

    None when there is no such bound, or when a branch enters the path after the
    compare. Register-to-register sign/zero extensions move the tracked index
    between families.
    """
    j = before - 1
    while j >= 0:
        if j + 1 in leaders:
            return None
        insn = records[j]
        operands = split_operands(insn.operands)
        if insn.mnemonic in ("ja", "jnbe", "jae", "jnb"):
            if j == 0 or j in leaders:
                return None
            compare = records[j - 1]
            compare_ops = split_operands(compare.operands)
            # The compiler sized the table by the bound it checks, whatever the
            # compare width (cmpb when it knows the index is a zero-extended byte).
            if (compare.mnemonic in ("cmpb", "cmpw", "cmpl", "cmpq") and len(compare_ops) == 2
                    and compare_ops[0].startswith("$") and family(compare_ops[1]) == index):
                limit = int(compare_ops[0][1:], 0)
                if limit < 0:
                    return None
                count = limit + 1 if insn.mnemonic in ("ja", "jnbe") else limit
                return count
            return None
        if insn.mnemonic == "cltq" and index == "a":
            j -= 1
            continue
        if (insn.mnemonic in ("movslq", "movl") and len(operands) == 2 and family(operands[1]) == index
                and family(operands[0])):
            index = family(operands[0])
            j -= 1
            continue
        written = written_families(insn)
        if written is None or index in written:
            return None
        if is_jump(insn.mnemonic) or insn.mnemonic in ("call", "callq") or not falls_through(insn.mnemonic):
            return None
        j -= 1
    return None


def _bound_all_paths(records: list[Record], preds: list[list[int]], before: int, index: str) -> int | None:
    """Entry count when every path into records[before] bounds the index, else None.

    MSVC /O2 enters a dispatch past its bound check: tail-merged case code jumps
    to the index extension with a constant index ("movl $0x26, %ebx; leal
    -0xe(%rbx), %eax; jmp", asCCompiler::CompileOverloadedDualOperator), and one
    switch's cases jump into the next switch past its identical bound
    (GLDevice::UpdateTexture). Walking every predecessor path back from the table
    load, each must end at the fall-through of "cmp $N, idx; ja/jae" (whose
    compare is the only way into the jcc) or at a constant definition of idx
    before any other write of it. Register renames (movslq/movl/cltq) and
    "leal K(%reg), idx" are followed. The count is the largest any path allows;
    a path from the procedure entry, from code with no known predecessor, or
    through the jcc's taken (above) edge returns None.
    """
    best = 0
    seen = set()
    stack = [(before, index, 0)]  # (node, tracked family, offset: idx = family + offset)
    while stack:
        node, tracked, offset = stack.pop()
        if (node, tracked, offset) in seen:
            continue
        seen.add((node, tracked, offset))
        if node == 0 or not preds[node]:
            return None
        for p in preds[node]:
            insn = records[p]
            operands = split_operands(insn.operands)
            if insn.mnemonic in ("ja", "jnbe", "jae", "jnb"):
                if p + 1 != node or direct_target(insn.mnemonic, insn.operands) == records[node].address:
                    return None  # the above edge carries no bound
                compare = records[p - 1] if p > 0 and preds[p] == [p - 1] else None
                compare_ops = split_operands(compare.operands) if compare is not None else []
                if (compare is not None and compare.mnemonic in ("cmpb", "cmpw", "cmpl", "cmpq")
                        and len(compare_ops) == 2 and compare_ops[0].startswith("$")
                        and family(compare_ops[1]) == tracked and offset == 0):
                    limit = int(compare_ops[0][1:], 0)
                    if limit < 0:
                        return None
                    best = max(best, limit + 1 if insn.mnemonic in ("ja", "jnbe") else limit)
                    continue
                stack.append((p, tracked, offset))
                continue
            written = written_families(insn)
            if written is None:
                return None
            if tracked not in written or (insn.mnemonic == "cltq" and tracked == "a"):
                stack.append((p, tracked, offset))
                continue
            if insn.mnemonic in ("movslq", "movl") and len(operands) == 2 and family(operands[1]) == tracked:
                if family(operands[0]):
                    stack.append((p, family(operands[0]), offset))
                    continue
                if operands[0].startswith("$"):
                    value = int(operands[0][1:], 0) + offset
                    if not 0 <= value < MAX_TABLE_ENTRIES:
                        return None
                    best = max(best, value + 1)
                    continue
            memory = MEMORY_RE.match(operands[0]) if insn.mnemonic == "leal" and len(operands) == 2 else None
            if (memory is not None and memory.group(2) is not None and memory.group(3) is None
                    and family(operands[1]) == tracked and family(memory.group(2))):
                stack.append((p, family(memory.group(2)), offset + int(memory.group(1) or "0", 16)))
                continue
            return None
    return best or None


def _entry_target(image: ImageBytes, address: int) -> int | None:
    data = image.read(address, 4)
    return image.image_base + struct.unpack("<I", data)[0] if data is not None else None


def _reachable(records: list[Record], index_of: dict[int, int], dispatch: dict[int, tuple[int, ...]],
               noreturn: frozenset[int]) -> set[int]:
    seen, stack = {0}, [0]
    while stack:
        node = stack.pop()
        insn = records[node]
        successors = []
        if continues(insn, noreturn) and node + 1 < len(records):
            successors.append(node + 1)
        target = direct_target(insn.mnemonic, insn.operands)
        if target is not None and insn.mnemonic not in ("call", "callq") and target in index_of:
            successors.append(index_of[target])
        successors.extend(index_of[case] for case in dispatch.get(insn.address, ()) if case in index_of)
        for successor in successors:
            if successor not in seen:
                seen.add(successor)
                stack.append(successor)
    return seen


def find_switch_tables(records: list[Record], start: int, end: int, image: ImageBytes,
                       noreturn: frozenset[int] = frozenset(), reachable_only: bool = False) -> list[SwitchTable]:
    """Recognise MSVC x64 switch tables of one procedure.

    On a linear decode (reachable_only) the bytes of tables not found yet still
    decode as "instructions", whose branch targets would be taken for leaders;
    only instructions reachable from the entry (through the tables found so far)
    then count. On a rebuilt stream, which holds no table bytes, every
    instruction counts.
    """
    index_of = {insn.address: i for i, insn in enumerate(records)}

    def is_dispatch(n: int) -> bool:
        return records[n].mnemonic in UNCONDITIONAL_JUMPS and re.fullmatch(r"\*%r\w+", records[n].operands) is not None

    # Seed the rounds with every dispatch's candidate tables (idiom, bound,
    # extent and entries proven; image base assumed). The base proof treats an
    # instruction with no known predecessor -- a case of a switch whose table is
    # not known yet -- as entered from every indirect jump of the procedure, so
    # starting from no tables left a dispatch unproven while another switch's
    # unknown cases lay on its backward paths: a chain of N
    # switches needed N rounds (D3D11/D3D12 CreatePipelineState: 10 and 14, more
    # than MAX_ROUNDS), and switches whose base registers are reused elsewhere
    # blocked each other for good (Vulkan CreatePipelineState, cgltf_validate).
    # Each round below re-proves every dispatch, base included, over the CFG the
    # previous round's tables imply; only a fixed point is accepted.
    seed_live = _reachable(records, index_of, {}, noreturn) if reachable_only and records else range(len(records))
    seed_leaders = {index_of[t] for n in seed_live
                    if (t := direct_target(records[n].mnemonic, records[n].operands)) is not None and t in index_of
                    and records[n].mnemonic not in ("call", "callq")}
    seed_preds = _predecessors(records, index_of, {}, noreturn)
    tables: dict[int, list[SwitchTable]] = {}
    for n in range(len(records)):
        if is_dispatch(n):
            found = _dispatch_tables(records, n, seed_preds, seed_leaders, index_of, start, end, image, {}, noreturn,
                                     assume_base=True)
            if found:
                tables[records[n].address] = found
    # A dispatch inside another switch's case is only reachable through that
    # switch's table, and a case that enters between a bound check and its jump
    # voids the bound; so every dispatch is matched again with the cases found
    # so far as leaders and predecessors, until the set no longer changes.
    for _ in range(MAX_ROUNDS):
        edges = {dispatch: tuple(t for table in found for t in table.targets) for dispatch, found in tables.items()}
        live = (_reachable(records, index_of, edges, noreturn) if reachable_only and records
                else range(len(records)))
        leaders = {index_of[t] for n in live
                   if (t := direct_target(records[n].mnemonic, records[n].operands)) is not None and t in index_of
                   and records[n].mnemonic not in ("call", "callq")}
        leaders |= {index_of[t] for targets in edges.values() for t in targets if t in index_of}
        preds = _predecessors(records, index_of, edges, noreturn)
        if reachable_only:
            # The initial linear sweep still decodes table bytes as instructions.
            # Their unreachable branches must not become incoming code paths to
            # the base/bound proof. Rebuilt-stream confirmation below uses every
            # remaining instruction, with no such predecessor filtering.
            preds = [[p for p in incoming if p in live] for incoming in preds]
        matched = {}
        for i in live:
            if records[i].mnemonic in UNCONDITIONAL_JUMPS and re.fullmatch(r"\*%r\w+", records[i].operands):
                found = _dispatch_tables(records, i, preds, leaders, index_of, start, end, image, edges, noreturn)
                if found:
                    matched[records[i].address] = found
        if matched == tables:
            return sorted((table for found in tables.values() for table in found), key=lambda table: table.start)
        tables = matched
    return []  # no stable set of tables


def _walk_back_to(records: list[Record], i: int, leaders: set[int], register: str, accept) -> int | None:
    """Index of the nearest record before i that writes register, if accept() takes it.

    Records in between must neither write register nor transfer control, and no
    branch may enter between the found record and i.
    """
    j = i - 1
    while j >= 0:
        if j + 1 in leaders:
            return None
        insn = records[j]
        written = written_families(insn)
        if written is None:
            return None
        if register in written:
            return j if accept(insn) else None
        if is_jump(insn.mnemonic) or not falls_through(insn.mnemonic) or insn.mnemonic in ("call", "callq"):
            return None
        j -= 1
    return None


def _dispatch_tables(records, i, preds, leaders, index_of, start, end, image, edges, noreturn,
                     assume_base: bool = False) -> list[SwitchTable]:
    """Tables of the dispatch at records[i]; assume_base skips the image-base proofs (candidates only)."""
    register = family(records[i].operands[1:])

    def is_add(insn: Record) -> bool:
        operands = split_operands(insn.operands)
        return insn.mnemonic == "addq" and len(operands) == 2 and family(operands[1]) == register \
            and family(operands[0]) is not None

    def is_table_load(insn: Record) -> bool:
        operands = split_operands(insn.operands)
        memory = MEMORY_RE.match(operands[0]) if insn.mnemonic == "movl" and len(operands) == 2 else None
        return (memory is not None and memory.group(1) is not None and family(memory.group(2)) is not None
                and family(memory.group(3)) is not None and memory.group(4) == "4"
                and family(operands[1]) == register)

    add = _walk_back_to(records, i, leaders, register, is_add)
    load = _walk_back_to(records, add, leaders, register, is_table_load) if add is not None else None
    if load is None:
        return []
    memory = MEMORY_RE.match(split_operands(records[load].operands)[0])
    base = family(memory.group(2))
    # Both the table base and the added base must be the image base.
    if not assume_base and not (
            _base_reaches_as_image_base(records, preds, load, base, image.image_base)
            and _base_reaches_as_image_base(records, preds, add, family(split_operands(records[add].operands)[0]),
                                            image.image_base)):
        return []
    jump_table = image.image_base + int(memory.group(1), 16)
    index = family(memory.group(3))
    before = load
    index_table = None

    def is_byte_load(insn: Record) -> bool:
        operands = split_operands(insn.operands)
        byte = MEMORY_RE.match(operands[0]) if insn.mnemonic == "movzbl" and len(operands) == 2 else None
        return (byte is not None and byte.group(1) is not None and byte.group(4) in (None, "1")
                and base in (family(byte.group(2)), family(byte.group(3))))

    selector = _walk_back_to(records, load, leaders, index, is_byte_load)
    if selector is not None:
        if not assume_base and not _base_reaches_as_image_base(records, preds, selector, base, image.image_base):
            return []
        byte = MEMORY_RE.match(split_operands(records[selector].operands)[0])
        other = byte.group(3) if family(byte.group(2)) == base else byte.group(2)
        index_table = image.image_base + int(byte.group(1), 16)
        index = family(other)
        before = selector
        if index is None:
            return []
    count = _bound(records, before, index, leaders)
    if count is None:
        count = _bound_all_paths(records, preds, before, index)
    first = 0
    if count is None:
        if index_table is not None:
            return []  # an unbounded index table has no provable extent
        code_bytes: set[int] = set()

        def valid(slot: int) -> bool:
            target = _entry_target(image, slot)
            return (target is not None and start <= target < jump_table and target in index_of
                    and start <= slot and slot + 4 <= end
                    and not any(slot + k in code_bytes for k in range(4)))

        def run(step: int) -> int:
            taken = 0
            while abs(taken) < MAX_TABLE_ENTRIES and valid(jump_table + 4 * (taken + min(step, 0))):
                taken += step
            return taken

        # Reachable code, counting the cases the forward run names, is never a slot.
        forward = [_entry_target(image, jump_table + 4 * n) for n in range(run(1))]
        case_edges = {**edges, records[i].address: tuple(forward)}
        for node in _reachable(records, index_of, case_edges, noreturn):
            code_bytes.update(range(records[node].address, records[node].end))
        count = run(1)
        # std::variant visits index -1 (valueless_by_exception) through the
        # slot before the table base. The index source is often not on the
        # straight-line path, so backward slots are taken on the same terms:
        # each names an instruction start, and none overlaps reachable code.
        first = run(-1)
        if count == 0:
            return []
    elif count > MAX_TABLE_ENTRIES:
        return []
    found = []
    if index_table is not None:
        if count > 256:
            return []
        selectors = image.read(index_table, count)
        if selectors is None:
            return []
        found.append(SwitchTable(index_table, index_table + count, records[i].address, "index"))
        first, count = 0, max(selectors) + 1
    table_start = jump_table + 4 * first
    table_end = jump_table + 4 * count
    if not (start <= table_start and table_end <= end) or any(not (start <= t.start and t.end <= end) for t in found):
        return []
    targets = []
    for slot in range(table_start, table_end, 4):
        target = _entry_target(image, slot)
        if target is None or not start <= target < end:
            return []
        targets.append(target)
    found.append(SwitchTable(table_start, table_end, records[i].address, "jump", tuple(targets)))
    return found


# ---------------------------------------------------------------------------
# Rebuilding the instruction stream around the tables.


def rebuild(records: list[Record], start: int, end: int, image: ImageBytes,
            tables: list[SwitchTable]) -> tuple[list[Record], int]:
    """Tile [start, end) with tables, instructions and 0x90/0xCC padding.

    Instructions come from the linear sweep where it is in step, and from a
    re-decode where it is not. A byte nothing explains is returned as an
    undecodable record, so it still fails the scan.
    """
    by_address = {insn.address: insn for insn in records}
    tables = sorted(tables, key=lambda table: table.start)
    out: list[Record] = []
    padding = 0
    cursor = start
    redecoded_from = None
    budget = MAX_REDECODES
    while cursor < end:
        table = next((t for t in tables if t.start <= cursor < t.end), None)
        if table is not None:
            cursor = table.end
            continue
        limit = min([t.start for t in tables if t.start > cursor] + [end])
        insn = by_address.get(cursor)
        if insn is not None and insn.size > 0 and insn.end <= limit:
            out.append(insn)
            cursor = insn.end
            continue
        if tables and _after_table(cursor, tables, out) and image.read(cursor, 1)[0] in PADDING_BYTES:
            padding += 1
            cursor += 1
            continue
        if redecoded_from != cursor and budget > 0:
            redecoded_from = cursor
            budget -= 1
            for fresh in image.redecode(cursor, limit):
                if cursor <= fresh.address < limit:
                    by_address[fresh.address] = fresh
            continue
        # Still out of step: this byte is not explained.
        raw = image.read(cursor, 1)
        out.append(Record(cursor, 1, "<undecodable>", "", None, False,
                          f"{raw[0]:02x} (unexplained byte)" if raw else "(unreadable byte)"))
        cursor += 1
    return out, padding


def _after_table(cursor: int, tables: list[SwitchTable], out: list[Record]) -> bool:
    """True when only tables and padding lie between the last instruction and cursor."""
    last_code = out[-1].end if out else None
    last_table = max((t.end for t in tables if t.end <= cursor), default=None)
    return last_table is not None and (last_code is None or last_code <= last_table)


def validate(records: list[Record], start: int, tables: list[SwitchTable],
             noreturn: frozenset[int] = frozenset()) -> SwitchTable | None:
    """Return a table the rebuilt stream contradicts, or None when every table holds."""
    if not records or records[0].address != start:
        return tables[0] if tables else None
    index_of = {insn.address: i for i, insn in enumerate(records)}
    dispatch: dict[int, tuple[int, ...]] = {}
    for table in tables:
        if table.kind == "jump":
            dispatch[table.dispatch] = dispatch.get(table.dispatch, ()) + table.targets

    def inside(address: int) -> SwitchTable | None:
        return next((t for t in tables if t.start <= address < t.end), None)

    for table in tables:
        if table.dispatch not in index_of or any(t not in index_of for t in table.targets):
            return table
    for insn in records:
        target = direct_target(insn.mnemonic, insn.operands)
        if target is not None and inside(target):
            return inside(target)
    for node in _reachable(records, index_of, dispatch, noreturn):
        insn = records[node]
        if continues(insn, noreturn) and inside(insn.end):
            return inside(insn.end)
    return None


def map_procedure(records: list[Record], start: int, end: int, image: ImageBytes,
                  noreturn: frozenset[int] = frozenset()) -> CodeMap:
    """Rebuild one procedure's instruction stream, proving each switch table it removes."""
    if not records or records[0].address != start:
        # The sweep entered the procedure out of step (after a table in the
        # previous one); decode it again from its entry.
        records = [insn for insn in image.redecode(start, end) if start <= insn.address < end]
    tables = find_switch_tables(records, start, end, image, noreturn, reachable_only=True) if records else []
    # Accept a set of tables only when the stream rebuilt around them
    # contradicts none of them and, re-read without any table bytes in it,
    # yields exactly the same tables (a fixed point).
    for _ in range(MAX_ROUNDS):
        rebuilt, padding = rebuild(records, start, end, image, tables)
        wrong = validate(rebuilt, start, tables, noreturn)
        # A table whose bytes are reached as code from anywhere in the image (a
        # cross-procedure branch, an address-taken value, a .pdata/export/reloc
        # or guard-CF entry) is not data; keep those bytes classified.
        if wrong is None:
            wrong = next((t for t in tables if image.entered_within(t.start, t.end)), None)
        if wrong is None:
            wrong = next((t for t in tables if image.table_masks_code(t.start, t.end)), None)
        if wrong is not None:
            # Drop every table of the contradicted dispatch and rebuild without it.
            tables = [t for t in tables if t.dispatch != wrong.dispatch]
            continue
        confirmed = find_switch_tables(rebuilt, start, end, image, noreturn)
        if confirmed == tables:
            break
        tables = confirmed
    else:
        tables = []
        rebuilt, padding = rebuild(records, start, end, image, tables)
    dispatch: dict[int, tuple[int, ...]] = {}
    for table in tables:
        if table.kind == "jump":
            dispatch[table.dispatch] = dispatch.get(table.dispatch, ()) + table.targets
    return CodeMap(rebuilt, tables, dispatch, padding)


def map_gap(records: list[Record], start: int, end: int, image: ImageBytes) -> list[Record]:
    """Rebuild the bytes between procedures (padding, or code without a PDB procedure)."""
    rebuilt, _ = rebuild(records, start, end, image, [])
    return rebuilt
