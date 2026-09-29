#!/usr/bin/env python3
"""Mutation checks for CI-100/CI-110's required execution and public claims."""

import copy
import importlib.util
import json
from pathlib import Path
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tools"))
from buildmatrix.workflow import parse_workflow_yaml


def load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


GATE = load("verify-required-jobs")
POLICY = load("test-workflow-failure-propagation")


def coverage_errors(job):
    """Pin the threshold producer and its terminal outcome enforcement."""
    errors = []
    steps = job.get("steps", [])
    threshold = [s for s in steps if s.get("id") == "subsystem-coverage"]
    expected_threshold = '''set +e
chmod +x scripts/coverage-report.sh
scripts/coverage-report.sh coverage.info --json coverage.json 2>&1 | tee subsystem-coverage.txt
threshold_exit=${PIPESTATUS[0]}
if [ "$threshold_exit" -ne 0 ]; then
  echo "coverage: error: Per-subsystem coverage analysis or threshold enforcement failed" | tee -a subsystem-coverage.txt
fi
exit "$threshold_exit"'''
    if (len(threshold) != 1 or threshold[0].get("run", "").strip() != expected_threshold
            or threshold[0].get("if") != "always()" or threshold[0].get("continue-on-error")):
        errors.append("per-subsystem coverage thresholds must execute and propagate their exit")
    final = [s for s in steps if s.get("name") == "Enforce coverage test and threshold results"]
    if len(final) != 1:
        return errors + ["missing terminal coverage enforcement"]
    final = final[0]
    if final.get("if") != "always()" or final.get("continue-on-error"):
        errors.append("terminal coverage enforcement must always run fail closed")
    environment = {
        "TEST_OUTCOME": "${{ steps.coverage-tests.outcome }}",
        "GENERATE_OUTCOME": "${{ steps.generate-coverage.outcome }}",
        "THRESHOLD_OUTCOME": "${{ steps.subsystem-coverage.outcome }}",
    }
    script = final.get("run", "")
    if final.get("env") != environment or not script.strip().endswith('exit "$failed"'):
        errors.append("coverage must consume all three real step outcomes and return failure")
    for variable in environment:
        if not re.search(r'if \[ "\$' + variable + r'" != "success" \]; then\n[^\n]+\n  failed=1\nfi', script):
            errors.append(f"coverage must reject every non-success {variable}")
    return errors


def workflow_errors(doc):
    errors = []
    triggers = doc.get("on", {})
    push = triggers.get("push", {})
    if "Working" not in push.get("branches", []) or any(k in push for k in ("paths", "paths-ignore")):
        errors.append("every Working push, including docs-only, must run")
    jobs = doc["jobs"]
    gate = jobs["required-ci-gate"]
    if gate.get("if") != "always()":
        errors.append("gate summary must survive dependency failure")
    required = gate.get("needs", [])
    verifier = next(s for s in gate["steps"] if s.get("name") == "Verify every required job succeeded")
    if json.loads(verifier["env"]["EXPECTED_REQUIRED_JOBS_JSON"]) != required:
        errors.append("gate inventory and needs differ")
    for job_id in ("coverage", "clang-tidy", "analysis-regressions"):
        job = jobs.get(job_id, {})
        if job_id not in required or not job or "if" in job or job.get("continue-on-error", False):
            errors.append(f"{job_id} must be required and unconditional")
    errors.extend(coverage_errors(jobs.get("coverage", {})))
    analysis = jobs.get("analysis-regressions", {})
    if analysis.get("permissions") != {"contents": "read"}:
        errors.append("analysis must be read-only")
    rows = analysis.get("strategy", {}).get("matrix", {}).get("include", [])
    if [(row.get("language"), row.get("report")) for row in rows] != [
            ("actions", "actions.sarif"), ("c-cpp", "cpp.sarif"), ("python", "python.sarif")]:
        errors.append("analysis must cover each shipped source language")
    # Hosted buildless c-cpp analysis took about 80 minutes (CodeQL Advanced run
    # 36455093304); a bound below that cancels the job and turns the gate red.
    timeouts = {row.get("language"): row.get("timeout") for row in rows}
    if analysis.get("timeout-minutes") != "${{ matrix.timeout }}" or not all(
            isinstance(value, int) and 10 <= value <= 180 for value in timeouts.values()) \
            or timeouts.get("c-cpp", 0) < 120:
        errors.append("analysis must carry a bounded per-language timeout that fits c-cpp analysis")
    steps = analysis.get("steps", [])
    required_steps = {
        "Initialize blocking analysis": ("uses", "github/codeql-action/init@cdf488f595d80d6e07e03d4674febd5ab45fa938"),
        "Produce fresh analysis results": ("uses", "github/codeql-action/analyze@cdf488f595d80d6e07e03d4674febd5ab45fa938"),
        "Fail on analysis findings": ("run", 'python3 .github/scripts/check-analysis-results.py '
                                      '"${{ runner.temp }}/blocking-codeql/${{ matrix.report }}" '
                                      '--language "${{ matrix.language }}" --baseline .github/codeql-baseline.json'),
    }
    for name, (key, expected) in required_steps.items():
        matches = [s for s in steps if s.get("name") == name]
        if len(matches) != 1 or matches[0].get(key) != expected:
            errors.append(f"missing or changed {name}")
        elif "if" in matches[0] or matches[0].get("continue-on-error", False):
            errors.append(f"bypassed {name}")
    if len(steps) == 4:
        if steps[2].get("with") != {"output": "${{ runner.temp }}/blocking-codeql",
                                    "upload": "never", "upload-database": False}:
            errors.append("analysis must consume fresh local SARIF without publishing")
        if steps[1].get("with", {}).get("build-mode") != "none":
            errors.append("analysis must remain buildless")
    else:
        errors.append("analysis step inventory changed")
    for job_id, tree, python, suffix in (
            ("build-linux-gcc", "linux-gcc-release", "python3", " --label-exclude experimental-modules"),
            ("build-windows-vs2022", "windows-release", "python", "")):
        job = jobs[job_id]
        steps = job["steps"]
        selected = [s for s in steps if s.get("name") == "Execute documented subsystem commands"]
        expected = (f"{python} tools/site-data/check_documented_selectors.py --build-dir build "
                    f"--documented-build-dir build/{tree} --config Release --execute --test-limit 50{suffix}")
        if len(selected) != 1:
            errors.append(f"{job_id} must execute documented commands")
        else:
            step = selected[0]
            command = " ".join(step.get("run", "").replace("\\\n", " ").split())
            if command != expected or step.get("if") != "matrix.config == 'Release'" or step.get("continue-on-error"):
                errors.append(f"{job_id} must execute exact bounded documented selectors")
            if not isinstance(step.get("timeout-minutes"), int) or not 10 <= step["timeout-minutes"] <= 60:
                errors.append(f"{job_id} documented execution needs a step timeout of 10-60 minutes")
            names = [s.get("name") for s in steps]
            if names.index("Execute documented subsystem commands") >= names.index("Run Tests"):
                errors.append(f"{job_id} must check documentation before broader test failures can skip it")
        if "Release" not in job["strategy"]["matrix"]["config"]:
            errors.append(f"{job_id} must contain the executing Release row")
    perf = jobs["performance-budget-governance"]
    golden = [s for s in perf["steps"] if s.get("name") == "Check golden baseline review records"]
    if ("performance-budget-governance" not in required or "if" in perf or perf.get("continue-on-error")
            or len(golden) != 1 or "if" in golden[0] or golden[0].get("continue-on-error")
            or golden[0].get("run") != 'python3 tools/perf-budget/check_golden_review.py --base "${GOLDEN_BASE_SHA}" --fallback-base origin/Working'):
        errors.append("golden baseline review must block the required gate")
    return errors


CLAIM_PATHS = (
    "README.md", "wiki/development/MinGW-Wine-Cross-Compilation.md",
    "wiki/platform/Cross-Compilation-Wine-Testing.md", "wiki/development/CI-Reproducible-Builds.md",
    "wiki/advanced/Testing.md", "wiki/advanced/Wine-Role-and-Fallback-Tiers.md",
)


def claim_errors(documents):
    errors = []
    for path, text in documents.items():
        if not re.search(r"experimental[\s\S]{0,220}workflow_dispatch|workflow_dispatch[\s\S]{0,220}experimental",
                         text, re.IGNORECASE):
            errors.append(f"{path}: MinGW/Wine must be experimental and manual workflow_dispatch")
        if "works fully" in text or "runs this automatically" in text:
            errors.append(f"{path}: unsupported MinGW/Wine success/automatic claim")
    return errors


class CIPerfContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
        cls.doc = parse_workflow_yaml(cls.source)

    def test_required_analysis_coverage_and_docs_push_contract(self):
        self.assertEqual(workflow_errors(self.doc), [])
        self.assertEqual(POLICY.required_workflow_errors(self.source), [])
        self.assertEqual(POLICY.required_job_bypass_errors(self.doc), [])

    def test_docs_only_push_and_red_summary_mutations(self):
        for key, value in (("paths-ignore", ["**/*.md", "docs/**", "wiki/**"]),
                           ("paths", ["SparkEngine/**"]), ("branches", ["main"])):
            mutant = copy.deepcopy(self.doc)
            mutant["on"]["push"][key] = value
            self.assertTrue(workflow_errors(mutant), key)
        mutant = copy.deepcopy(self.doc)
        mutant["jobs"]["required-ci-gate"]["if"] = "success()"
        self.assertTrue(workflow_errors(mutant))

    def test_analysis_cannot_be_removed_bypassed_or_detached_from_gate(self):
        for mutation in ("removed", "advisory", "conditional", "detached", "results skipped", "language dropped",
                         "baseline dropped", "c-cpp timeout too short"):
            mutant = copy.deepcopy(self.doc)
            jobs = mutant["jobs"]
            if mutation == "removed":
                del jobs["analysis-regressions"]
            elif mutation == "advisory":
                jobs["analysis-regressions"]["continue-on-error"] = True
            elif mutation == "conditional":
                jobs["analysis-regressions"]["if"] = "false"
            elif mutation == "detached":
                jobs["required-ci-gate"]["needs"].remove("analysis-regressions")
            elif mutation == "results skipped":
                jobs["analysis-regressions"]["steps"][-1]["run"] = "true"
            elif mutation == "baseline dropped":
                step = jobs["analysis-regressions"]["steps"][-1]
                step["run"] = step["run"].split(" --baseline")[0]
            elif mutation == "c-cpp timeout too short":
                jobs["analysis-regressions"]["strategy"]["matrix"]["include"][1]["timeout"] = 30
            else:
                jobs["analysis-regressions"]["strategy"]["matrix"]["include"].pop()
            with self.subTest(mutation=mutation):
                self.assertTrue(workflow_errors(mutant))

    def test_coverage_threshold_and_outcome_mutations(self):
        for mutation in ("threshold removed", "threshold ignored", "terminal skipped", "skipped outcome accepted"):
            mutant = copy.deepcopy(self.doc)
            steps = mutant["jobs"]["coverage"]["steps"]
            threshold = next(s for s in steps if s.get("id") == "subsystem-coverage")
            final = next(s for s in steps if s.get("name") == "Enforce coverage test and threshold results")
            if mutation == "threshold removed":
                steps.remove(threshold)
            elif mutation == "threshold ignored":
                threshold["run"] = threshold["run"].replace('exit "$threshold_exit"', "exit 0")
            elif mutation == "terminal skipped":
                final["if"] = "success()"
            else:
                final["run"] = final["run"].replace('!= "success"', '== "failure"')
            with self.subTest(mutation=mutation):
                self.assertTrue(workflow_errors(mutant))

    def test_each_regression_makes_real_required_gate_red(self):
        required = self.doc["jobs"]["required-ci-gate"]["needs"]
        for job in ("coverage", "clang-tidy", "analysis-regressions"):
            for outcome in ("failure", "skipped", "cancelled"):
                needs = {name: {"result": "success"} for name in required}
                needs[job]["result"] = outcome
                passed, deferred, failed = GATE.verify_with_policy(needs, expected_jobs=required)
                self.assertEqual(failed, [(job, outcome)])
                self.assertIn(f"| {job} | **{outcome}** |", GATE.markdown(passed, failed, deferred))

    def test_documented_commands_cannot_regress_to_discovery_only(self):
        for job in ("build-linux-gcc", "build-windows-vs2022"):
            for mutation in ("resolve only", "advisory", "unbounded", "missing release"):
                mutant = copy.deepcopy(self.doc)
                lane = mutant["jobs"][job]
                step = next(s for s in lane["steps"] if s.get("name") == "Execute documented subsystem commands")
                if mutation == "resolve only":
                    step["run"] = step["run"].replace("--execute", "")
                elif mutation == "advisory":
                    step["continue-on-error"] = True
                elif mutation == "unbounded":
                    step.pop("timeout-minutes")
                else:
                    lane["strategy"]["matrix"]["config"].remove("Release")
                with self.subTest(job=job, mutation=mutation):
                    self.assertTrue(workflow_errors(mutant))

    def test_golden_review_cannot_be_skipped_or_advisory(self):
        for mutation in ("advisory", "skipped", "removed"):
            mutant = copy.deepcopy(self.doc)
            steps = mutant["jobs"]["performance-budget-governance"]["steps"]
            step = next(s for s in steps if s.get("name") == "Check golden baseline review records")
            if mutation == "advisory":
                step["continue-on-error"] = True
            elif mutation == "skipped":
                step["if"] = "false"
            else:
                steps.remove(step)
            self.assertTrue(workflow_errors(mutant), mutation)

    def test_mingw_claims_and_mutations(self):
        docs = {p: (ROOT / p).read_text(encoding="utf-8") for p in CLAIM_PATHS}
        self.assertEqual(claim_errors(docs), [])
        for path in docs:
            for old, new in (("experimental", "supported"), ("workflow_dispatch", "push")):
                mutant = {path: re.sub(old, new, docs[path], flags=re.IGNORECASE)}
                self.assertTrue(claim_errors(mutant), (path, old))
        self.assertTrue(claim_errors({"wiki": "experimental workflow_dispatch; D3D11 works fully"}))
        self.assertEqual(POLICY.experimental_mingw_lane_errors(self.doc), [])


if __name__ == "__main__":
    unittest.main()
