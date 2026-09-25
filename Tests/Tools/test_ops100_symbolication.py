"""OPS-100: fail-closed build-id symbolication (tools/ops/symbolicate_crash.py).

Grammar and bounds tests run everywhere. The store/resolve tests compile a
small C program with the host toolchain (cc, objcopy, addr2line), because the
tool is only meaningful against real ELF + DWARF; they run on Linux only.
"""

from __future__ import annotations

import importlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "ops"))
symbolicate = importlib.import_module("symbolicate_crash")

BUILD_ID = "0123456789abcdef0123456789abcdef01234567"

PROGRAM = """\
#include <stdio.h>
__attribute__((noinline)) int spark_symbolication_target(int value)
{
    return value * 3 + 1; /* TARGET_LINE */
}
int main(void)
{
    printf("%d\\n", spark_symbolication_target(2));
    return 0;
}
"""


def section(*records: str) -> str:
    return "\n".join(["header text", symbolicate.SECTION_BEGIN, *records, symbolicate.SECTION_END, "tail"]) + "\n"


class SectionGrammarTests(unittest.TestCase):
    def test_valid_section_parses(self) -> None:
        parsed = symbolicate.parse_symbolic_section(
            section(
                f"MODULE 3 build_id={BUILD_ID} bias=0x7f0000 name=libgame.so",
                "SYMFRAME 0 kind=pc module=3 offset=0x1234",
                "SYMFRAME 1 kind=ra module=- address=0xdeadbeef",
                "SYMFRAME 2 kind=ra module=3 offset=0x2000",
            )
        )
        self.assertEqual(parsed.modules[3].name, "libgame.so")
        self.assertEqual([frame.kind for frame in parsed.frames], ["pc", "ra", "ra"])
        self.assertIsNone(parsed.frames[1].module)
        self.assertEqual(parsed.frames[2].offset, 0x2000)

    def assert_refused(self, text: str, message: str) -> None:
        with self.assertRaisesRegex(symbolicate.SymbolicationError, message):
            symbolicate.parse_symbolic_section(text)

    def test_missing_section_is_refused(self) -> None:
        self.assert_refused("*** STACK TRACE ***\n  FRAME foo\n", "no symbolic-frame section")

    def test_truncated_section_is_refused(self) -> None:
        self.assert_refused(
            f"{symbolicate.SECTION_BEGIN}\nSYMFRAME 0 kind=pc module=- address=0x1\n", "truncated"
        )

    def test_duplicate_section_is_refused(self) -> None:
        one = section("SYMFRAME 0 kind=pc module=- address=0x1")
        self.assert_refused(one + one, "more than one")

    def test_malformed_record_is_refused(self) -> None:
        self.assert_refused(section("SYMFRAME 0 kind=pc module=- address=0x1 ; rm -rf /"), "malformed")
        self.assert_refused(section("MODULE 0 build_id=../../etc bias=0x0 name=x"), "malformed")
        self.assert_refused(section("MODULE 0 build_id=00 bias=0x0 name=a/b"), "malformed")

    def test_invalid_build_ids_are_refused(self) -> None:
        for build_id, message in (("abc", "odd number"), ("ab", "outside the accepted range"), ("aa" * 65, "outside")):
            self.assert_refused(
                section(f"MODULE 0 build_id={build_id} bias=0x0 name=x", "SYMFRAME 0 kind=pc module=0 offset=0x1"),
                message,
            )

    def test_build_id_bounds_match_the_crash_writer(self) -> None:
        # The C++ writer drops ids outside these bounds (module listed as
        # unmapped); if the reader's bounds drifted, one odd third-party
        # module would make the whole log unreadable.
        header = (ROOT / "SparkEngine" / "Source" / "Utils" / "CrashSymbolication.h").read_text(encoding="utf-8")
        for constant, expected in (
            ("kMinCrashBuildIdBytes", symbolicate.MIN_BUILD_ID_BYTES),
            ("kMaxCrashBuildIdBytes", symbolicate.MAX_BUILD_ID_BYTES),
        ):
            match = re.search(rf"inline constexpr size_t {constant} = (\d+);", header)
            self.assertIsNotNone(match, constant)
            self.assertEqual(int(match.group(1)), expected, constant)

    def test_sequence_and_references_are_enforced(self) -> None:
        self.assert_refused(section("SYMFRAME 1 kind=ra module=- address=0x1"), "out of sequence")
        self.assert_refused(section("SYMFRAME 0 kind=pc module=4 offset=0x1"), "undeclared MODULE 4")
        self.assert_refused(
            section("SYMFRAME 0 kind=ra module=- address=0x1", "SYMFRAME 1 kind=pc module=- address=0x2"),
            "only frame 0",
        )
        self.assert_refused(
            section(f"MODULE 0 build_id={BUILD_ID} bias=0x0 name=a", "SYMFRAME 0 kind=ra module=0 offset=0x0"),
            "offset 0",
        )
        self.assert_refused(
            section(
                f"MODULE 0 build_id={BUILD_ID} bias=0x0 name=a",
                f"MODULE 0 build_id={BUILD_ID} bias=0x0 name=b",
                "SYMFRAME 0 kind=pc module=0 offset=0x1",
            ),
            "duplicate MODULE",
        )
        self.assert_refused(
            section(f"MODULE 0 build_id={BUILD_ID} bias=0x0 name=a", "SYMFRAME 0 kind=pc module=- address=0x1"),
            "not referenced",
        )
        self.assert_refused(section(), "no frames")

    def test_frame_count_is_bounded(self) -> None:
        records = [f"SYMFRAME {index} kind=ra module=- address=0x1" for index in range(symbolicate.MAX_FRAMES + 1)]
        self.assert_refused(section(*records), f"more than {symbolicate.MAX_FRAMES}")

    def test_oversized_log_is_refused(self) -> None:
        with tempfile.NamedTemporaryFile() as handle:
            handle.truncate(symbolicate.MAX_LOG_BYTES + 1)
            with self.assertRaisesRegex(symbolicate.SymbolicationError, "exceeds"):
                symbolicate.read_crash_log(Path(handle.name))


@unittest.skipUnless(sys.platform.startswith("linux"), "ELF build-id symbolication is Linux-only")
class StoreAndResolveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.cc = shutil.which("cc") or shutil.which("gcc")
        missing = [name for name, found in (("cc", cls.cc), ("objcopy", shutil.which("objcopy")),
                                            ("addr2line", shutil.which("addr2line")), ("nm", shutil.which("nm")))
                   if not found]
        if missing:
            raise AssertionError(f"required binutils/toolchain programs missing: {missing}")
        cls.temp = tempfile.TemporaryDirectory()
        cls.work = Path(cls.temp.name)
        source = cls.work / "target.c"
        source.write_text(PROGRAM)
        cls.target_line = next(n for n, line in enumerate(PROGRAM.splitlines(), 1) if "TARGET_LINE" in line)
        cls.debug_binary = cls.compile(source, "with_debug", ["-g", "-O0", "-Wl,--build-id=sha1"])
        cls.no_debug_binary = cls.compile(source, "without_debug", ["-O0", "-Wl,--build-id=sha1"])
        cls.no_build_id_binary = cls.compile(source, "without_build_id", ["-g", "-O0", "-Wl,--build-id=none"])
        with open(cls.debug_binary, "rb") as handle:
            cls.build_id = symbolicate.inspect_elf(handle).build_id
        symbols = subprocess.run(["nm", str(cls.debug_binary)], check=True, capture_output=True, text=True).stdout
        cls.target_offset = next(
            int(line.split()[0], 16) for line in symbols.splitlines() if line.endswith(" spark_symbolication_target")
        )

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temp.cleanup()

    @classmethod
    def compile(cls, source: Path, name: str, flags: list[str]) -> Path:
        output = cls.work / name
        subprocess.run([cls.cc, *flags, str(source), "-o", str(output)], check=True)
        return output

    def setUp(self) -> None:
        self.case = tempfile.TemporaryDirectory()
        self.store = Path(self.case.name) / "store"
        self.store.mkdir()
        self.root = symbolicate.open_store_root(self.store)

    def tearDown(self) -> None:
        self.case.cleanup()

    def write_log(self, build_id: str | None = None) -> Path:
        log = Path(self.case.name) / "crash.log"
        log.write_text(
            section(
                f"MODULE 0 build_id={build_id or self.build_id} bias=0x555555554000 name=with_debug",
                f"SYMFRAME 0 kind=pc module=0 offset={hex(self.target_offset)}",
                "SYMFRAME 1 kind=ra module=- address=0x7fff0000",
            )
        )
        return log

    def resolve(self, log: Path) -> list[dict[str, object]]:
        parsed = symbolicate.parse_symbolic_section(symbolicate.read_crash_log(log))
        return symbolicate.resolve(parsed, self.root, "addr2line")

    def test_store_then_resolve_to_function_and_line(self) -> None:
        entry = symbolicate.store_binary(self.root, self.debug_binary, "objcopy")
        self.assertEqual(entry, self.root / ".build-id" / self.build_id[:2] / f"{self.build_id[2:]}.debug")
        frames = self.resolve(self.write_log())
        self.assertEqual(frames[0]["status"], "resolved")
        self.assertEqual(frames[0]["function"], "spark_symbolication_target")
        self.assertEqual(Path(str(frames[0]["file"])).name, "target.c")
        # The function's entry maps to its opening line, just above the body.
        self.assertIn(frames[0]["line"], range(self.target_line - 2, self.target_line + 1))
        self.assertEqual(frames[1]["status"], "no-module")

    def test_storing_the_same_binary_twice_is_idempotent(self) -> None:
        first = symbolicate.store_binary(self.root, self.debug_binary, "objcopy")
        second = symbolicate.store_binary(self.root, self.debug_binary, "objcopy")
        self.assertEqual(first, second)
        self.assertEqual(len(list(first.parent.iterdir())), 1, "temporary split files must not remain")

    def test_store_refuses_binary_without_debug_info(self) -> None:
        with self.assertRaisesRegex(symbolicate.SymbolicationError, "no DWARF"):
            symbolicate.store_binary(self.root, self.no_debug_binary, "objcopy")

    def test_store_refuses_binary_without_build_id(self) -> None:
        with self.assertRaisesRegex(symbolicate.SymbolicationError, "no GNU build-id"):
            symbolicate.store_binary(self.root, self.no_build_id_binary, "objcopy")

    def test_missing_symbols_are_reported_not_guessed(self) -> None:
        frames = self.resolve(self.write_log())
        self.assertEqual(frames[0]["status"], "missing-symbols")
        self.assertNotIn("function", frames[0])

    def test_wrong_build_id_is_refused(self) -> None:
        # File another binary's debug info under this crash's build-id path.
        other = symbolicate.store_binary(self.root, self.debug_binary, "objcopy")
        forged_id = "ff" * 20
        forged = self.root / ".build-id" / "ff" / f"{forged_id[2:]}.debug"
        forged.parent.mkdir()
        shutil.copyfile(other, forged)
        with self.assertRaisesRegex(symbolicate.SymbolicationError, "build-id mismatch"):
            self.resolve(self.write_log(forged_id))

    def test_symlinked_entry_and_directory_are_refused(self) -> None:
        entry = symbolicate.store_binary(self.root, self.debug_binary, "objcopy")
        outside = Path(self.case.name) / "outside.debug"
        shutil.move(entry, outside)
        entry.symlink_to(outside)
        with self.assertRaisesRegex(symbolicate.SymbolicationError, "refused"):
            self.resolve(self.write_log())

        entry.unlink()
        shutil.move(outside, entry)
        moved = Path(self.case.name) / "moved"
        shutil.move(entry.parent, moved)
        entry.parent.symlink_to(moved)
        with self.assertRaisesRegex(symbolicate.SymbolicationError, "not a real directory"):
            self.resolve(self.write_log())

    def test_symlinked_store_root_is_refused(self) -> None:
        link = Path(self.case.name) / "store-link"
        link.symlink_to(self.store)
        with self.assertRaisesRegex(symbolicate.SymbolicationError, "must be a real directory"):
            symbolicate.open_store_root(link)

    def test_cli_exit_codes(self) -> None:
        tool = str(ROOT / "tools" / "ops" / "symbolicate_crash.py")
        stored = subprocess.run(
            [sys.executable, tool, "store", "--store", str(self.store), str(self.debug_binary)],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(stored.returncode, 0, stored.stderr)
        resolved = subprocess.run(
            [sys.executable, tool, "resolve", "--store", str(self.store), "--log", str(self.write_log()), "--json"],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(resolved.returncode, 0, resolved.stderr)
        self.assertIn("spark_symbolication_target", resolved.stdout)
        refused = subprocess.run(
            [sys.executable, tool, "resolve", "--store", str(self.store), "--log", os.devnull],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(refused.returncode, 2)
        self.assertIn("refused", refused.stderr)


if __name__ == "__main__":
    unittest.main()
