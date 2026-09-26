#!/usr/bin/env python3
"""MOD-360: tools/check-module-asset-refs.py fails closed for enforced modules.

Fixture repositories exercise each way an enforced module's asset references can
be wrong -- a missing file, a case-only mismatch, a run-time-composed path
prefix, and every disagreement between the module reference record
(``GameModules/<Module>/asset-references.json``), the files on disk and the
repository integrity manifest -- and require a non-zero exit for each. A module
outside ``ENFORCED_MODULES`` with the same defect is reported but does not fail.
The last case runs the checker against the real repository for OpenWorld.
"""
from __future__ import annotations

import contextlib
import hashlib
import importlib.util
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "check-module-asset-refs.py"
ENFORCED = "SparkGameOpenWorld"
REPORT_ONLY = "SparkGameMMOFPS"


def _load_checker():
    spec = importlib.util.spec_from_file_location("check_module_asset_refs", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


CHECKER = _load_checker()


class FixtureRepo:
    """A minimal repository: one module source file, an asset tree and both manifests."""

    def __init__(self, root: Path) -> None:
        self.root = root
        (root / "Assets").mkdir()
        (root / "tools" / "asset-integrity").mkdir(parents=True)
        self.integrity: list[dict[str, str]] = []
        self.references: list[dict[str, str]] = []
        self.write_policy(["fixture-rule"])

    def write_policy(self, rule_ids: list[str]) -> None:
        policy = {"version": 1, "root": "Assets", "licenses": {}, "rules": [{"id": rule} for rule in rule_ids]}
        (self.root / "tools/asset-integrity/provenance.json").write_text(json.dumps(policy), encoding="utf-8")

    def add_asset(self, relative: str, payload: bytes, *, rule: str = "fixture-rule", record: bool = True) -> str:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(payload)
        digest = hashlib.sha256(payload).hexdigest()
        self.integrity.append({"path": relative.removeprefix("Assets/"), "sha256": digest,
                               "provenance": f"fixture [{rule}]"})
        if record:
            self.references.append({"path": relative, "sha256": digest, "kind": "fixture", "provenanceRule": rule})
        return digest

    def write_source(self, module: str, text: str) -> None:
        source = self.root / "GameModules" / module / "Source" / "Fixture.cpp"
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_text(text, encoding="utf-8")

    def write_manifests(self, module: str = ENFORCED) -> None:
        integrity = {"version": 2, "algorithm": "sha256", "root": "Assets", "fileCount": len(self.integrity),
                     "entries": self.integrity}
        (self.root / "Assets/assets.integrity.json").write_text(json.dumps(integrity), encoding="utf-8")
        manifest = self.root / "GameModules" / module / "asset-references.json"
        manifest.write_text(json.dumps({"manifestVersion": 1, "module": module, "description": "fixture",
                                        "references": self.references}), encoding="utf-8")

    def run(self, *modules: str) -> tuple[int, str]:
        argv = ["--repo-root", str(self.root)]
        for module in modules:
            argv += ["--module", module]
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = CHECKER.main(argv)
        return code, out.getvalue() + err.getvalue()


class CheckModuleAssetRefsTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.repo = FixtureRepo(Path(self._temp.name))
        self.repo.add_asset("Assets/Audio/Music/theme.wav", b"RIFF-theme")
        self.repo.add_asset("Assets/Models/Ground/tile.obj", b"v 0 0 0\n")
        self.valid_source = (
            'const char* kTheme = "Assets/Audio/Music/theme.wav";\n'
            '// "Assets/Audio/Music/commented_out.wav" is not compiled\n'
            'const char* kTile = "Assets/Models/Ground/tile.obj";\n'
        )

    def tearDown(self) -> None:
        self._temp.cleanup()

    def test_valid_module_passes(self) -> None:
        self.repo.write_source(ENFORCED, self.valid_source)
        self.repo.write_manifests()
        code, output = self.repo.run(ENFORCED)
        self.assertEqual(code, 0, output)
        self.assertIn("OK: SparkGameOpenWorld [enforced] 2 asset reference(s), 0 problem(s)", output)

    def test_missing_file_fails(self) -> None:
        self.repo.write_source(ENFORCED, self.valid_source + 'const char* kGone = "Assets/Audio/Music/gone.ogg";\n')
        self.repo.write_manifests()
        code, output = self.repo.run(ENFORCED)
        self.assertEqual(code, 1)
        self.assertIn("Assets/Audio/Music/gone.ogg does not exist", output)

    def test_case_mismatch_fails(self) -> None:
        self.repo.write_source(ENFORCED, self.valid_source.replace("Music/theme.wav", "music/theme.wav"))
        self.repo.write_manifests()
        code, output = self.repo.run(ENFORCED)
        self.assertEqual(code, 1)
        self.assertIn("Assets/Audio/music/theme.wav does not exist (exact case)", output)

    def test_composed_path_prefix_fails(self) -> None:
        self.repo.write_source(
            ENFORCED, self.valid_source + 'std::string p = std::string("Assets/Scenes/") + name + ".scene";\n')
        self.repo.write_manifests()
        code, output = self.repo.run(ENFORCED)
        self.assertEqual(code, 1)
        self.assertIn("'Assets/Scenes/' is not a complete asset path", output)

    def test_format_string_path_fails(self) -> None:
        self.repo.write_source(ENFORCED, self.valid_source + 'const char* f = "Assets/Textures/icon_%s.png";\n')
        self.repo.write_manifests()
        code, output = self.repo.run(ENFORCED)
        self.assertEqual(code, 1)
        self.assertIn("icon_%s.png' is not a complete asset path", output)

    def test_missing_module_manifest_fails(self) -> None:
        self.repo.write_source(ENFORCED, self.valid_source)
        self.repo.write_manifests()
        (self.repo.root / "GameModules" / ENFORCED / "asset-references.json").unlink()
        code, output = self.repo.run(ENFORCED)
        self.assertEqual(code, 1)
        self.assertIn("asset-references.json: missing", output)

    def test_unrecorded_reference_fails(self) -> None:
        self.repo.add_asset("Assets/Audio/Music/extra.wav", b"RIFF-extra", record=False)
        self.repo.write_source(ENFORCED, self.valid_source + 'const char* e = "Assets/Audio/Music/extra.wav";\n')
        self.repo.write_manifests()
        code, output = self.repo.run(ENFORCED)
        self.assertEqual(code, 1)
        self.assertIn("does not record referenced asset Assets/Audio/Music/extra.wav", output)

    def test_stale_record_fails(self) -> None:
        self.repo.add_asset("Assets/Audio/Music/old.wav", b"RIFF-old")
        self.repo.write_source(ENFORCED, self.valid_source)
        self.repo.write_manifests()
        code, output = self.repo.run(ENFORCED)
        self.assertEqual(code, 1)
        self.assertIn("records Assets/Audio/Music/old.wav, which no module source references", output)

    def test_digest_drift_fails(self) -> None:
        self.repo.write_source(ENFORCED, self.valid_source)
        self.repo.write_manifests()
        (self.repo.root / "Assets/Audio/Music/theme.wav").write_bytes(b"RIFF-replaced")
        code, output = self.repo.run(ENFORCED)
        self.assertEqual(code, 1)
        self.assertIn("Assets/Audio/Music/theme.wav sha256 is", output)
        self.assertIn("digest differs from Assets/assets.integrity.json", output)

    def test_undeclared_in_integrity_manifest_fails(self) -> None:
        self.repo.write_source(ENFORCED, self.valid_source)
        self.repo.integrity = [entry for entry in self.repo.integrity if entry["path"] != "Models/Ground/tile.obj"]
        self.repo.write_manifests()
        code, output = self.repo.run(ENFORCED)
        self.assertEqual(code, 1)
        self.assertIn("Assets/Models/Ground/tile.obj is not declared in Assets/assets.integrity.json", output)

    def test_unknown_or_mismatched_provenance_rule_fails(self) -> None:
        self.repo.write_source(ENFORCED, self.valid_source)
        self.repo.references[0]["provenanceRule"] = "invented-rule"
        self.repo.write_manifests()
        code, output = self.repo.run(ENFORCED)
        self.assertEqual(code, 1)
        self.assertIn("provenance rule 'invented-rule' is not defined", output)
        self.assertIn("attributed to a different provenance rule", output)

    def test_report_only_module_warns_without_failing(self) -> None:
        self.repo.write_source(REPORT_ONLY, 'const char* t = "Assets/Audio/Music/race.ogg";\n')
        code, output = self.repo.run(REPORT_ONLY)
        self.assertEqual(code, 0, output)
        self.assertIn(f"WARN: {REPORT_ONLY} [report-only]", output)
        self.assertIn("Assets/Audio/Music/race.ogg does not exist", output)

    def test_unknown_module_is_a_usage_error(self) -> None:
        self.repo.write_source(ENFORCED, self.valid_source)
        self.repo.write_manifests()
        code, output = self.repo.run("SparkGameNoSuchModule")
        self.assertEqual(code, 2)
        self.assertIn("unknown module: SparkGameNoSuchModule", output)

    def test_repository_openworld_references_resolve(self) -> None:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = CHECKER.main(["--module", ENFORCED])
        self.assertEqual(code, 0, out.getvalue() + err.getvalue())
        self.assertIn("OK: SparkGameOpenWorld [enforced]", out.getvalue())


if __name__ == "__main__":
    unittest.main()
