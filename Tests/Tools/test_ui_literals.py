"""Guard the per-lane UI literal ratchet (tools/ui/check_ui_literals.py) and its codemod.

The ratchet only means something while it really scans the lane files, so the tree check
fails when it scans fewer than MIN_SCANNED_FILES files, and a self-test fixture proves that
a literal above its baseline is reported as a failure rather than passing silently.
"""

from __future__ import annotations

import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "ui"))

import check_ui_literals as literals  # noqa: E402  (tools/ui is put on the path above)
import migrate_ui_literals as migrate  # noqa: E402

MIN_SCANNED_FILES = 50
EXPECTED_LANES = {"P1", "P2", "P3", "P4", "P5", "L2", "GM", "L1", "LA", "IN", "TD"}

FIXTURE = """\
#include <imgui.h>

// ImVec4(0.6f, 0.6f, 0.6f, 1.0f) in a comment is not counted.
void DrawFixture()
{
    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Failed");
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%d items", 3);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SetNextItemWidth(-1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 80);
    if (ImGui::Button("Apply", ImVec2(96, 0)))
    {
    }
    ImGui::BeginChild("Fill", ImVec2(0, 0));
    ImGui::GetWindowDrawList()->AddText(ImVec2(0, 0), IM_COL32(255, 255, 255, 200), "label");
    const char* text = "ImVec4(1, 0, 0, 1)";
    ImGui::Text("%s", text);
}
"""
FIXTURE_LITERALS = 6


class UiLiteralRatchetTests(unittest.TestCase):
    def _run_check(self, root: Path, baselines: Path, *extra: str) -> tuple[int, str]:
        stdout, stderr = io.StringIO(), io.StringIO()
        saved = sys.stdout, sys.stderr
        sys.stdout, sys.stderr = stdout, stderr
        try:
            code = literals.main(["--root", str(root), "--baselines", str(baselines), "check", *extra])
        finally:
            sys.stdout, sys.stderr = saved
        return code, stdout.getvalue() + stderr.getvalue()

    def test_tree_is_within_every_lane_baseline(self) -> None:
        baselines = literals.load_baselines(literals.BASELINE_DIR)
        self.assertEqual({b.lane for b in baselines}, EXPECTED_LANES)
        result = literals.check(ROOT, baselines, out=io.StringIO())
        print(f"\nUI literal ratchet scanned {result.scanned} files "
              f"({result.missing} listed but not present yet), {result.total} literals")
        self.assertGreaterEqual(result.scanned, MIN_SCANNED_FILES,
                                f"the ratchet scanned only {result.scanned} files; lane lists are broken")
        self.assertGreater(result.total, 0, "no literal counted anywhere: the scanner stopped scanning")
        self.assertEqual(result.regressions, [], "\n".join(result.regressions))

    def test_scanner_counts_the_fixture_exactly(self) -> None:
        sites = literals.scan_text(FIXTURE)
        self.assertEqual(len(sites), FIXTURE_LITERALS, [site.rule for site in sites])
        self.assertEqual(sum(site.rule == "colour-imvec4" for site in sites), 2)
        self.assertEqual(sum(site.rule == "colour-im-col32" for site in sites), 1)
        self.assertEqual(sum(site.rule == "item-width" for site in sites), 1)
        self.assertEqual(sum(site.rule == "column-width" for site in sites), 1)
        self.assertEqual(sum(site.rule == "size-imvec2" for site in sites), 1)

    def test_self_test_fixture_above_its_baseline_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "src").mkdir()
            (root / "src" / "Fixture.cpp").write_text(FIXTURE, encoding="utf-8")
            baselines = root / "baselines"
            baselines.mkdir()
            record = {"schema_version": 1, "lane": "ZZ", "description": "fixture", "allowlisted_tables": {},
                      "total": FIXTURE_LITERALS, "files": {"src/Fixture.cpp": FIXTURE_LITERALS}}
            (baselines / "ZZ.json").write_text(json.dumps(record), encoding="utf-8")
            code, output = self._run_check(root, baselines)
            self.assertEqual(code, 0, output)
            self.assertIn("scanned 1 files", output)

            record["files"]["src/Fixture.cpp"] = FIXTURE_LITERALS - 1
            record["total"] = FIXTURE_LITERALS - 1
            (baselines / "ZZ.json").write_text(json.dumps(record), encoding="utf-8")
            code, output = self._run_check(root, baselines)
            self.assertEqual(code, 1, output)
            self.assertIn("src/Fixture.cpp holds 6 UI literals, baseline 5", output)

            code, output = self._run_check(root, baselines, "--min-files", "2")
            self.assertEqual(code, 1, output)

    def test_scanning_nothing_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            baselines = root / "baselines"
            baselines.mkdir()
            record = {"schema_version": 1, "lane": "ZZ", "allowlisted_tables": {}, "total": 0,
                      "files": {"src/NotThere.cpp": 0}}
            (baselines / "ZZ.json").write_text(json.dumps(record), encoding="utf-8")
            code, output = self._run_check(root, baselines)
            self.assertEqual(code, 1, output)
            self.assertIn("scanned no files", output)

    def test_a_file_listed_by_two_lanes_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            baselines = Path(temp)
            for lane in ("AA", "BB"):
                record = {"schema_version": 1, "lane": lane, "allowlisted_tables": {}, "total": 0,
                          "files": {"src/Shared.cpp": 0}}
                (baselines / f"{lane}.json").write_text(json.dumps(record), encoding="utf-8")
            with self.assertRaises(literals.LiteralCheckError):
                literals.load_baselines(baselines)

    def test_allowlisted_table_is_exempt(self) -> None:
        text = ("constexpr ImU32 HudPalette[] = {\n    IM_COL32(1, 2, 3, 255),\n    IM_COL32(4, 5, 6, 255),\n};\n"
                "ImU32 loose = IM_COL32(7, 8, 9, 255);\n")
        self.assertEqual(len(literals.scan_text(text)), 3)
        self.assertEqual(len(literals.scan_text(text, ["HudPalette"])), 1)

    def test_stripper_keeps_line_structure_for_crlf(self) -> None:
        text = 'a /* x\r\ny */ b // c\r\nd "e\\"f" \'g\'\r\n'
        stripped = literals.strip_comments_and_strings(text)
        self.assertEqual(len(stripped), len(text))
        self.assertEqual(stripped.count("\r\n"), text.count("\r\n"))
        self.assertEqual(literals.strip_comments_and_strings(stripped), stripped)

    def test_codemod_rewrites_the_table_and_respects_play_truth(self) -> None:
        guards = migrate.play_truth_strings()
        self.assertIn("State preview only (no gameplay)", guards)
        source = FIXTURE + '    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "State preview only (no gameplay)");\n'
        updated, skipped, ambiguous, rewritten = migrate.migrate_text(source, guards)
        self.assertIn('Spark::Ui::StatusText(Spark::Ui::Status::Error, "Failed");', updated)
        self.assertIn('ImGui::TextColored(Spark::Ui::ColV(Spark::Ui::Role::TextSecondary), "%d items", 3);', updated)
        self.assertIn("ImGui::SetNextItemWidth(Spark::Ui::Px(120.0f));", updated)
        self.assertIn("ImGui::SetNextItemWidth(-1);", updated)
        self.assertIn("ImGuiTableColumnFlags_WidthFixed, Spark::Ui::Px(80));", updated)
        self.assertIn('ImGui::Button("Apply", Spark::Ui::Px2(96, 0))', updated)
        self.assertIn('ImGui::BeginChild("Fill", ImVec2(0, 0));', updated)
        self.assertIn("Spark::Ui::Col(Spark::Ui::Role::TextPrimary, .78f)", updated)
        self.assertIn('#include "SparkUI/Runtime.h"', updated)
        self.assertIn('#include "SparkUI/Widgets.h"', updated)
        self.assertIn("// ImVec4(0.6f, 0.6f, 0.6f, 1.0f) in a comment is not counted.", updated)
        self.assertEqual(len(skipped), 1)
        self.assertEqual(ambiguous, [])
        self.assertEqual(rewritten, 6)
        self.assertEqual(len(literals.scan_text(updated)), 1, "only the play-truth line keeps its literal")

    def test_update_only_lowers(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "src").mkdir()
            (root / "src" / "Fixture.cpp").write_text(FIXTURE, encoding="utf-8")
            baselines = root / "baselines"
            baselines.mkdir()
            record = {"schema_version": 1, "lane": "ZZ", "allowlisted_tables": {}, "total": 9,
                      "files": {"src/Fixture.cpp": 9}}
            (baselines / "ZZ.json").write_text(json.dumps(record), encoding="utf-8")
            saved = sys.stdout
            sys.stdout = io.StringIO()
            try:
                self.assertEqual(literals.main(["--root", str(root), "--baselines", str(baselines), "update",
                                                "--lane", "ZZ"]), 0)
            finally:
                sys.stdout = saved
            lowered = json.loads((baselines / "ZZ.json").read_text(encoding="utf-8"))
            self.assertEqual(lowered["files"]["src/Fixture.cpp"], FIXTURE_LITERALS)
            self.assertEqual(lowered["total"], FIXTURE_LITERALS)


if __name__ == "__main__":
    unittest.main(verbosity=2)
