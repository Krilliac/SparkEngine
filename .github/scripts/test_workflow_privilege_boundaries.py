#!/usr/bin/env python3
"""Regression tests for least-privilege boundaries in small workflows."""

from __future__ import annotations

from pathlib import Path
import re
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from buildmatrix.workflow import parse_workflow_yaml  # noqa: E402


WORKFLOW_ROOT = ROOT / ".github" / "workflows"
DEPENDABOT = ROOT / ".github" / "dependabot.yml"
CODACY = ROOT / ".github" / "workflows" / "codacy.yml"
CODACY_REPORT = ROOT / ".github" / "workflows" / "codacy-report.yml"
CODEQL = ROOT / ".github" / "workflows" / "codeql.yml"
CODEQL_REPORT = ROOT / ".github" / "workflows" / "codeql-report.yml"
SUMMARY = ROOT / ".github" / "workflows" / "summary.yml"
RELEASE = ROOT / ".github" / "workflows" / "release.yml"
MSVC = ROOT / ".github" / "workflows" / "msvc.yml"
CODEQL_ACTION_SHA = "cdf488f595d80d6e07e03d4674febd5ab45fa938"
FULL_SHA = re.compile(r"^[0-9a-f]{40}$")


def _block(text: str, key: str, indent: int) -> str:
    lines = text.splitlines()
    marker = f"{' ' * indent}{key}:"
    try:
        start = lines.index(marker)
    except ValueError as error:
        raise AssertionError(f"missing YAML block {key!r} at indent {indent}") from error

    end = len(lines)
    for index in range(start + 1, len(lines)):
        line = lines[index]
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        current_indent = len(line) - len(line.lstrip())
        if current_indent <= indent:
            end = index
            break
    return "\n".join(lines[start:end])


def _permissions(job: str) -> dict[str, str]:
    block = _block(job, "permissions", 4)
    permissions: dict[str, str] = {}
    for line in block.splitlines()[1:]:
        match = re.match(r"^ {6}([a-z-]+):\s*([^#\s]+)", line)
        if match:
            permissions[match.group(1)] = match.group(2)
    return permissions


def _assert_full_sha_pins(test: unittest.TestCase, text: str) -> None:
    uses = re.findall(r"^\s*-?\s*uses:\s*([^\s#]+)", text, flags=re.MULTILINE)
    test.assertTrue(uses, "workflow must use at least one action")
    for action in uses:
        test.assertIn("@", action)
        test.assertRegex(action.rsplit("@", 1)[1], FULL_SHA)


class WorkflowPrivilegeBoundaryTests(unittest.TestCase):
    def test_dependabot_version_updates_cannot_open_pull_requests(self) -> None:
        document = parse_workflow_yaml(DEPENDABOT.read_text(encoding="utf-8"))
        updates = document.get("updates")

        self.assertIsInstance(updates, list)
        self.assertEqual(len(updates), 2, "expected both Dependabot update blocks")

        pairs: list[tuple[str, str]] = []
        for update in updates:
            self.assertIsInstance(update, dict)
            ecosystem = update.get("package-ecosystem")
            directory = update.get("directory")
            pair = (ecosystem, directory)
            pairs.append(pair)

            with self.subTest(ecosystem=ecosystem, directory=directory):
                self.assertIsInstance(ecosystem, str)
                self.assertTrue(ecosystem)
                self.assertIsInstance(directory, str)
                self.assertTrue(directory)
                limit = update.get("open-pull-requests-limit")
                self.assertIs(
                    type(limit),
                    int,
                    "open-pull-requests-limit must be an integer",
                )
                self.assertEqual(
                    limit,
                    0,
                    "Dependabot version updates must not open pull requests",
                )

        self.assertEqual(
            len(pairs),
            len(set(pairs)),
            "Dependabot ecosystem/directory pairs must be unique",
        )

    def test_every_external_action_uses_an_immutable_revision(self) -> None:
        for workflow in sorted(WORKFLOW_ROOT.glob("*.y*ml")):
            text = workflow.read_text(encoding="utf-8")
            uses = re.findall(
                r"^\s*-?\s*uses:\s*([^\s#]+)",
                text,
                flags=re.MULTILINE,
            )
            for action in uses:
                if action.startswith("./"):
                    continue
                with self.subTest(workflow=workflow.name, action=action):
                    self.assertRegex(
                        action,
                        r"@(?:[0-9a-f]{40}|sha256:[0-9a-f]{64})$",
                    )

    def test_codeql_action_family_uses_one_reviewed_revision(self) -> None:
        revisions: set[str] = set()
        for workflow in (CODEQL, CODACY_REPORT, MSVC):
            text = workflow.read_text(encoding="utf-8")
            revisions.update(
                re.findall(
                    r"github/codeql-action/[a-z-]+@([0-9a-f]{40})",
                    text,
                )
            )

        self.assertEqual(revisions, {CODEQL_ACTION_SHA})

    def test_release_publisher_has_read_only_actions_access(self) -> None:
        text = RELEASE.read_text(encoding="utf-8")
        release = _block(text, "release", 2)

        self.assertEqual(
            _permissions(release),
            {
                "actions": "read",
                "attestations": "write",
                "contents": "write",
                "id-token": "write",
                "statuses": "read",
            },
        )
        self.assertNotIn("actions: write", release)

    def test_codeql_source_workflow_executes_no_repository_code(self) -> None:
        text = CODEQL.read_text(encoding="utf-8")
        analyze = _block(text, "analyze", 2)

        self.assertEqual(
            _permissions(analyze),
            {"security-events": "write", "actions": "read", "contents": "read"},
        )
        self.assertNotIn("pull-requests: write", analyze)
        self.assertNotIn("issues: write", analyze)
        self.assertNotRegex(analyze, r"(?m)^\s+run:")
        self.assertNotIn("actions/cache", analyze)
        self.assertIn("ref: ${{ github.event.pull_request.head.sha || github.sha }}", analyze)
        self.assertIn("persist-credentials: false", analyze)
        self.assertNotIn("config-file:", analyze)
        self.assertIn("paths-ignore:", analyze)
        self.assertIn("archive: false", analyze)
        self.assertIn(
            "CODEQL_EXTRACTOR_CPP_OPTION_FRONTEND_OPTIONS: --c++23", analyze
        )
        _assert_full_sha_pins(self, text)

    def test_codeql_privileged_reporter_bootstraps_exact_trusted_checkout(self) -> None:
        text = CODEQL_REPORT.read_text(encoding="utf-8")
        invalidate = _block(text, "invalidate-running", 2)
        report = _block(text, "report", 2)

        self.assertEqual(
            _permissions(invalidate),
            {"actions": "read", "contents": "read", "statuses": "write"},
        )
        self.assertNotIn("pull-requests: write", invalidate)
        self.assertEqual(
            _permissions(report),
            {
                "actions": "read",
                "contents": "read",
                "pull-requests": "write",
                "statuses": "write",
            },
        )
        self.assertEqual(report.count("actions/checkout@"), 1)
        checkout = _block(report, "with", 6)
        self.assertIn("repository: ${{ github.repository }}", checkout)
        self.assertIn("ref: ${{ github.event.repository.default_branch }}", checkout)
        self.assertIn("path: trusted-reporter", checkout)
        self.assertIn("persist-credentials: false", checkout)
        self.assertNotIn("github.event.workflow_run.head_sha", checkout)
        self.assertIn("Attest exact default-branch reporter before execution", report)
        self.assertLess(
            report.index("Attest exact default-branch reporter before execution"),
            report.index("Test trusted CodeQL reporter"),
        )
        self.assertIn("checkoutSha.toLowerCase() !== workflowSha.toLowerCase()", report)
        self.assertIn("TRUSTED_WORKFLOW_REF: ${{ github.workflow_ref }}", report)
        self.assertIn("TRUSTED_WORKFLOW_SHA: ${{ github.workflow_sha }}", report)
        self.assertIn("artifact-ids: ${{ steps.preflight.outputs.artifact-ids }}", report)
        self.assertIn("skip-decompress: true", report)
        self.assertIn("digest-mismatch: error", report)
        _assert_full_sha_pins(self, text)

    def test_codacy_source_workflow_is_unprivileged_data_only(self) -> None:
        text = CODACY.read_text(encoding="utf-8")
        scan = _block(text, "codacy-security-scan", 2)

        self.assertNotIn("pull_request:", text)
        self.assertEqual(_permissions(scan), {"contents": "read"})
        self.assertNotIn("security-events: write", text)
        self.assertNotIn("github/codeql-action/upload-sarif@", text)
        self.assertNotIn("actions/download-artifact@", text)
        self.assertIn("actions/checkout@", scan)
        self.assertIn("ref: ${{ github.sha }}", scan)
        self.assertIn("CODACY_COMMIT_SHA: ${{ github.sha }}", scan)
        self.assertIn("persist-credentials: false", scan)
        self.assertIn('test "$(git rev-parse HEAD)" != "$CODACY_COMMIT_SHA"', scan)
        self.assertIn("normalize-codacy-sarif.py", scan)
        self.assertIn("actions/upload-artifact@", scan)
        self.assertIn("archive: false", scan)
        self.assertNotIn("secrets.CODACY_PROJECT_TOKEN", scan)
        self.assertIn("codacy/codacy-analysis-cli@sha256:", scan)
        self.assertIn("/var/run/docker.sock", scan)
        _assert_full_sha_pins(self, text)

    def test_msvc_pr_analysis_does_not_persist_checkout_credentials(self) -> None:
        text = MSVC.read_text(encoding="utf-8")
        analyze = _block(text, "analyze", 2)
        self.assertIn("pull_request:", text)
        self.assertIn("security-events: write", analyze)
        self.assertIn("persist-credentials: false", analyze)

    def test_codacy_privileged_reporter_uses_only_trusted_code_and_exact_data(self) -> None:
        text = CODACY_REPORT.read_text(encoding="utf-8")
        report = _block(text, "report", 2)

        self.assertEqual(
            _permissions(report),
            {"actions": "read", "contents": "read", "security-events": "write"},
        )
        self.assertIn('workflows: ["Codacy Security Scan"]', text)
        self.assertNotIn("pull_request_target:", text)
        self.assertEqual(report.count("actions/checkout@"), 1)
        checkout = _block(report, "with", 8)
        self.assertIn("repository: ${{ github.repository }}", checkout)
        self.assertIn("ref: ${{ github.event.repository.default_branch }}", checkout)
        self.assertIn("path: trusted-reporter", checkout)
        self.assertIn("persist-credentials: false", checkout)
        self.assertNotIn("github.event.workflow_run.head_sha", checkout)
        self.assertIn("test-authorize-codacy-sarif.js", report)
        self.assertIn("test_validate_codacy_sarif.py", report)
        self.assertIn("test_workflow_privilege_boundaries.py", report)
        self.assertIn("Attest exact default-branch reporter before execution", report)
        self.assertLess(
            report.index("Attest exact default-branch reporter before execution"),
            report.index("Test trusted Codacy reporter"),
        )
        self.assertIn("checkoutSha.toLowerCase() !== workflowSha.toLowerCase()", report)
        self.assertIn("actions/download-artifact@", report)
        self.assertIn("artifact-ids: ${{ steps.preflight.outputs.artifact-id }}", report)
        self.assertIn("run-id: ${{ github.event.workflow_run.id }}", report)
        self.assertIn("skip-decompress: true", report)
        self.assertIn("digest-mismatch: error", report)
        self.assertIn("validate-codacy-sarif.py", report)
        self.assertIn("PREFLIGHT_ARTIFACT_MANIFEST", report)
        self.assertIn("TRUSTED_WORKFLOW_REF: ${{ github.workflow_ref }}", report)
        self.assertIn("TRUSTED_WORKFLOW_SHA: ${{ github.workflow_sha }}", report)
        self.assertIn("github/codeql-action/upload-sarif@", report)
        self.assertIn('mkdir -- "$RUNNER_TEMP/codacy-no-checkout"', report)
        self.assertIn("checkout_path: ${{ runner.temp }}/codacy-no-checkout", report)
        self.assertIn("ref: ${{ steps.final-auth.outputs.upload-ref }}", report)
        self.assertIn("sha: ${{ steps.final-auth.outputs.upload-sha }}", report)
        _assert_full_sha_pins(self, text)

    def test_issue_summary_needs_no_repository_contents(self) -> None:
        text = SUMMARY.read_text(encoding="utf-8")
        job = _block(text, "summary", 2)

        self.assertEqual(_permissions(job), {"issues": "write", "models": "read"})
        self.assertNotIn("actions/checkout@", job)
        _assert_full_sha_pins(self, text)


if __name__ == "__main__":
    unittest.main()
