"""Guard tools/ui/check_icon_glyphs.py: every icon codepoint the UI draws exists in the loaded font.

The tree check must pass and must really scan (definition and use counts above a floor), and RED
fixtures prove each rule reports a failure instead of passing silently, including the exact
defect that shipped: ICON_FA_FOLDER_OPEN encoding U+F056 ("circle-minus") under a U+F076 comment.
"""

from __future__ import annotations

import io
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "ui"))

import check_icon_glyphs as glyphs  # noqa: E402  (tools/ui is put on the path above)

MIN_DEFINITIONS = 170
FONTS_CPP = "constexpr std::array<ImWchar, 3> kIconRanges = {0xF000, 0xF8FF, 0};\n"
GOOD_HEADER = """\
#pragma once
#define ICON_FA_FOLDER "\\xef\\x81\\xbb"      // U+F07B folder
#define ICON_FA_FOLDER_OPEN "\\xef\\x81\\xbc" // U+F07C folder-open
#define ICON_FA_PLUS "\\xef\\x81\\xa7"        // U+F067 plus
"""


class IconGlyphCheckTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name)
        (self.root / "fonts").mkdir()
        shutil.copyfile(ROOT / glyphs.FONT, self.root / "fonts" / "icons.ttf")
        (self.root / "Fonts.cpp").write_text(FONTS_CPP, encoding="utf-8")
        (self.root / "src").mkdir()

    def tearDown(self) -> None:
        self._temp.cleanup()

    def _check(self, header: str, source: str = "", fonts_cpp: str = FONTS_CPP, minimum: int = 1) -> glyphs.Result:
        (self.root / "Fonts.cpp").write_text(fonts_cpp, encoding="utf-8")
        (self.root / "src" / "Icons.h").write_text(header, encoding="utf-8")
        (self.root / "src" / "Use.cpp").write_text(source, encoding="utf-8")
        return glyphs.check(self.root, font_path=Path("fonts/icons.ttf"), fonts_cpp=Path("Fonts.cpp"),
                            definition_files={Path("src/Icons.h"): minimum}, scan_roots=(Path("src"),))

    def test_tree_passes_and_really_scans(self) -> None:
        result = glyphs.check(ROOT)
        print(f"\nicon glyph check: {result.definitions} definitions, {result.uses} literal uses")
        self.assertEqual(result.errors, [], "\n".join(result.errors))
        self.assertGreaterEqual(result.definitions, MIN_DEFINITIONS)
        self.assertGreater(result.uses, result.definitions, "sources outside the definition files were not scanned")

    def test_cli_reports_ok(self) -> None:
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = glyphs.main(["check"])
        self.assertEqual(code, 0, err.getvalue())
        self.assertIn("check_icon_glyphs: OK", out.getvalue())

    def test_font_reader_resolves_custom_and_standard_glyph_names(self) -> None:
        font = glyphs.read_icon_font(ROOT / glyphs.FONT)
        self.assertGreater(len(font.glyph_names), 1000)
        self.assertEqual(font.glyph_names[0xF07C], "folder-open")
        self.assertEqual(font.glyph_names[0xF056], "circle-minus")
        self.assertEqual(font.glyph_names[0xF067], "plus")  # post index < 258: Mac standard name

    def test_fixture_header_passes(self) -> None:
        result = self._check(GOOD_HEADER, 'const char* k = ICON_FA_FOLDER_OPEN " Open";\n')
        self.assertEqual(result.errors, [])
        self.assertEqual(result.definitions, 3)

    def test_shipped_folder_open_defect_fails(self) -> None:
        header = '#define ICON_FA_FOLDER_OPEN "\\xef\\x81\\x96" // U+F076 (alt open folder)\n'
        result = self._check(header)
        self.assertTrue(any("comment must start with 'U+XXXX glyph-name'" in e and "circle-minus" in e
                            for e in result.errors), result.errors)

    def test_comment_codepoint_disagreeing_with_bytes_fails(self) -> None:
        header = '#define ICON_FA_FOLDER_OPEN "\\xef\\x81\\x96" // U+F07C folder-open\n'
        result = self._check(header)
        self.assertTrue(any("bytes encode U+F056 (circle-minus) but the comment says U+F07C" in e
                            for e in result.errors), result.errors)

    def test_glyph_name_disagreeing_with_font_fails(self) -> None:
        header = '#define ICON_FA_FOLDER_OPEN "\\xef\\x81\\x96" // U+F056 folder-open\n'
        result = self._check(header)
        self.assertTrue(any("is 'circle-minus'" in e for e in result.errors), result.errors)

    def test_codepoint_missing_from_font_fails(self) -> None:
        font = glyphs.read_icon_font(ROOT / glyphs.FONT)
        missing = next(cp for cp in range(0xF000, 0xF8FF) if cp not in font.glyph_names)
        source = f'const char* k = "\\u{missing:04X}";\n'
        result = self._check(GOOD_HEADER, source)
        self.assertTrue(any(f"U+{missing:04X} is not in" in e for e in result.errors), result.errors)

    def test_codepoint_outside_loaded_ranges_fails(self) -> None:
        narrow = "constexpr std::array<ImWchar, 3> kIconRanges = {0xF000, 0xF0FF, 0};\n"
        result = self._check(GOOD_HEADER, 'const char* k = "\\xef\\x86\\xb2";\n', fonts_cpp=narrow)
        self.assertTrue(any("U+F1B2 is outside the icon ranges" in e for e in result.errors), result.errors)

    def test_too_few_definitions_fails(self) -> None:
        result = self._check(GOOD_HEADER, minimum=4)
        self.assertTrue(any("expected at least 4" in e for e in result.errors), result.errors)

    def test_missing_icon_ranges_declaration_is_an_error(self) -> None:
        with self.assertRaises(ValueError):
            self._check(GOOD_HEADER, fonts_cpp="// no ranges here\n")


if __name__ == "__main__":
    unittest.main(verbosity=2)
