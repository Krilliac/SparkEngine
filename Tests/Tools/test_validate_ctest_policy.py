#!/usr/bin/env python3
"""Adversarial tests for the CI-110 CTest timeout/label policy validator."""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
VALIDATOR = REPO_ROOT / "Tools" / "validate_ctest_policy.py"
TESTS_CMAKE = REPO_ROOT / "Tests" / "CMakeLists.txt"
BUILD_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build.yml"

# Registrations outside Tests/CMakeLists.txt that the configured Linux tree
# evaluates. Each once lacked TIMEOUT or LABELS; the default static scan must
# keep reaching every one of these files.
PRODUCT_TEST_SOURCES = (
    "CMakeLists.txt",
    "SparkAssetPipelineCore/CMakeLists.txt",
    "SparkAutomation/CMakeLists.txt",
    "SparkBuild/CMakeLists.txt",
    "SparkConsole/CMakeLists.txt",
    "SparkCrashReporter/CMakeLists.txt",
    "SparkDaemon/CMakeLists.txt",
    "SparkGateway/CMakeLists.txt",
    "SparkInstaller/CMakeLists.txt",
    "SparkLauncher/CMakeLists.txt",
    "SparkServer/CMakeLists.txt",
    "Tests/FPSGameplayEvents/CMakeLists.txt",
    "Tests/PackageSmoke/CMakeLists.txt",
    "Tests/PackageSmoke/FPSProgression/CMakeLists.txt",
    "cmake/SparkFuzzPolicy.cmake",
)

GOOD_CMAKE = """\
add_test(NAME Alpha COMMAND alpha)
set_tests_properties(Alpha PROPERTIES
    LABELS "unit"
    TIMEOUT 30)
foreach(_selector IN ITEMS A B)
    add_test(NAME ${_selector} COMMAND runner --filter "${_selector}")
    set_tests_properties(${_selector} PROPERTIES LABELS "security;unit" TIMEOUT 30)
endforeach()
add_test(
    NAME Beta
    COMMAND ${CMAKE_COMMAND} "-DVALUE=$<IF:$<CONFIG:Debug>,a,b>" -P script.cmake
)
set_tests_properties(Beta PROPERTIES
    ENVIRONMENT "X=1"
    LABELS "integration"
    TIMEOUT ${SPARK_TEST_CTEST_TIMEOUT})
"""


def ctest_json(*tests: dict) -> str:
    return json.dumps({"kind": "ctestInfo", "version": {"major": 1, "minor": 0}, "tests": list(tests)})


def ctest_entry(name: str, **props: object) -> dict:
    return {
        "name": name,
        "command": ["x"],
        "properties": [{"name": key, "value": value} for key, value in props.items()],
    }


def write_build_tree(
    root: Path,
    installed: tuple[str, ...],
    system_name: str = "Windows",
    uninstalled: tuple[str, ...] = (),
) -> None:
    """Minimal configured tree: a CMake file-API codemodel reply plus CMakeSystem.cmake.

    ``installed`` executables carry an install rule; ``uninstalled`` ones and a
    static library do not, so neither may count as shipped.
    """
    reply = root / ".cmake" / "api" / "v1" / "reply"
    reply.mkdir(parents=True)
    targets = []
    for name, target_type, install in (
        [(name, "EXECUTABLE", True) for name in installed]
        + [(name, "EXECUTABLE", False) for name in uninstalled]
        + [("SparkEngineLib", "STATIC_LIBRARY", True)]
    ):
        document: dict = {"name": name, "type": target_type}
        if install:
            document["install"] = {"prefix": {"path": "/usr"}, "destinations": [{"path": "bin"}]}
        json_file = f"target-{name}-Release.json"
        (reply / json_file).write_text(json.dumps(document), encoding="utf-8")
        targets.append({"name": name, "jsonFile": json_file})
    codemodel = {"kind": "codemodel", "configurations": [{"name": "Release", "targets": targets}]}
    (reply / "codemodel-v2-0.json").write_text(json.dumps(codemodel), encoding="utf-8")
    index = {
        "cmake": {"version": {"string": "4.4.1"}},
        "objects": [{"kind": "codemodel", "version": {"major": 2, "minor": 8}, "jsonFile": "codemodel-v2-0.json"}],
        "reply": {},
    }
    (reply / "index-2026-09-27T00-00-00-0000.json").write_text(json.dumps(index), encoding="utf-8")
    system = root / "CMakeFiles" / "4.4.1" / "CMakeSystem.cmake"
    system.parent.mkdir(parents=True)
    system.write_text(f'set(CMAKE_SYSTEM_NAME "{system_name}")\n', encoding="utf-8")


class CTestPolicyValidator(unittest.TestCase):
    def run_validator(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(VALIDATOR), *args],
            capture_output=True,
            text=True,
            check=False,
        )

    def check_cmake(self, text: str) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "CMakeLists.txt"
            path.write_text(text, encoding="utf-8")
            return self.run_validator("--cmake-lists", str(path))

    def check_json(
        self,
        text: str,
        installed: tuple[str, ...] = ("SparkShaderCompiler",),
        system_name: str = "Linux",
        uninstalled: tuple[str, ...] = (),
    ) -> subprocess.CompletedProcess[str]:
        # Generic timeout/label fixtures use the remaining documented Linux
        # compiler gap. A configured tree must contain an installed executable;
        # binary-enforcement cases override installed/platform explicitly.
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "ctest.json"
            path.write_text(text, encoding="utf-8")
            build = Path(temporary) / "build"
            write_build_tree(build, installed, system_name, uninstalled)
            return self.run_validator("--ctest-json", str(path), "--build-dir", str(build))

    # -- static CMake view ---------------------------------------------------

    def test_well_formed_registrations_pass(self) -> None:
        result = self.check_cmake(GOOD_CMAKE)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("OK (3 test registration(s)", result.stdout)

    def test_test_without_timeout_fails_by_name(self) -> None:
        result = self.check_cmake(GOOD_CMAKE + "add_test(NAME Hangs COMMAND hang)\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("Hangs: no TIMEOUT", result.stderr)
        self.assertIn("Hangs: no LABELS", result.stderr)

    def test_timeout_zero_is_rejected_because_ctest_treats_it_as_unbounded(self) -> None:
        result = self.check_cmake(GOOD_CMAKE.replace("TIMEOUT 30)\nforeach", "TIMEOUT 0)\nforeach"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("Alpha: TIMEOUT 0 disables", result.stderr)

    def test_timeout_above_ceiling_and_non_numeric_are_rejected(self) -> None:
        over = self.check_cmake(GOOD_CMAKE.replace("TIMEOUT 30)\nforeach", "TIMEOUT 99999)\nforeach"))
        self.assertEqual(over.returncode, 1)
        self.assertIn("exceeds", over.stderr)
        junk = self.check_cmake(GOOD_CMAKE.replace("TIMEOUT 30)\nforeach", "TIMEOUT soon)\nforeach"))
        self.assertEqual(junk.returncode, 1)
        self.assertIn("not a positive integer", junk.stderr)

    def test_commented_out_timeout_does_not_count(self) -> None:
        text = (
            "add_test(NAME Gamma COMMAND gamma)\n"
            "set_tests_properties(Gamma PROPERTIES LABELS \"unit\")\n"
            "# set_tests_properties(Gamma PROPERTIES TIMEOUT 30)\n"
        )
        result = self.check_cmake(text)
        self.assertEqual(result.returncode, 1)
        self.assertIn("Gamma: no TIMEOUT", result.stderr)

    def test_commented_out_add_test_is_not_counted_and_hash_in_quotes_survives(self) -> None:
        text = (
            "# add_test(NAME Ghost COMMAND ghost)\n"
            "add_test(NAME Delta COMMAND delta \"--tag=#1\")\n"
            "set_tests_properties(Delta PROPERTIES LABELS \"unit\" TIMEOUT 5)\n"
        )
        result = self.check_cmake(text)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("OK (1 test registration(s)", result.stdout)

    def test_timeout_on_a_different_test_does_not_cover_this_one(self) -> None:
        text = (
            "add_test(NAME One COMMAND one)\n"
            "add_test(NAME Two COMMAND two)\n"
            "set_tests_properties(One PROPERTIES LABELS \"unit\" TIMEOUT 5)\n"
        )
        result = self.check_cmake(text)
        self.assertEqual(result.returncode, 1)
        self.assertIn("Two: no TIMEOUT", result.stderr)
        self.assertNotIn("One:", result.stderr)

    def test_file_that_registers_nothing_fails(self) -> None:
        result = self.check_cmake("enable_testing()\n")
        self.assertEqual(result.returncode, 1)
        self.assertIn("registers no tests", result.stderr)

    # -- configured-tree view (ctest --show-only=json-v1) ----------------------

    def test_json_inventory_passes_when_every_test_is_bounded_and_labelled(self) -> None:
        result = self.check_json(ctest_json(ctest_entry("A", TIMEOUT=30.0, LABELS=["unit"])))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_json_inventory_rejects_missing_or_zero_timeout_and_missing_labels(self) -> None:
        result = self.check_json(
            ctest_json(
                ctest_entry("NoTimeout", LABELS=["unit"]),
                ctest_entry("ZeroTimeout", TIMEOUT=0.0, LABELS=["unit"]),
                ctest_entry("NoLabels", TIMEOUT=30.0),
            )
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("NoTimeout: no TIMEOUT", result.stderr)
        self.assertIn("ZeroTimeout: TIMEOUT 0.0", result.stderr)
        self.assertIn("NoLabels: no LABELS", result.stderr)

    def test_json_inventory_with_zero_tests_fails(self) -> None:
        result = self.check_json(ctest_json())
        self.assertEqual(result.returncode, 1)
        self.assertIn("inventory is empty", result.stderr)

    def test_json_that_is_not_ctest_output_fails(self) -> None:
        result = self.check_json(json.dumps({"kind": "somethingElse"}))
        self.assertEqual(result.returncode, 1)
        self.assertIn("missing 'tests' list", result.stderr)

    # -- shipped-binary lanes (configured-tree view) -------------------------------

    @staticmethod
    def binary_entry(name: str, command: list[str], labels: list[str], environment: list[str] | None = None) -> dict:
        entry = ctest_entry(name, TIMEOUT=30.0, LABELS=labels)
        entry["command"] = command
        if environment is not None:
            entry["properties"].append({"name": "ENVIRONMENT", "value": environment})
        return entry

    def test_shipped_binary_with_only_a_version_probe_is_rejected(self) -> None:
        installer = "/build/bin/SparkInstaller"
        cases = {
            "direct --version": [installer, "--version"],
            "direct --help": [installer, "--help"],
            "version runner": ["/usr/bin/cmake", f"-DSPARK_VERSION_EXECUTABLE={installer}", "-P", "v.cmake"],
        }
        for case, command in cases.items():
            with self.subTest(case=case):
                result = self.check_json(
                    ctest_json(self.binary_entry("SparkInstallerVersion", command, ["installer", "integration"])),
                    installed=("SparkInstaller",),
                )
                self.assertEqual(result.returncode, 1)
                self.assertIn("shipped binary SparkInstaller is only exercised through --help/--version", result.stderr)

    def test_behavioural_lane_needs_an_integration_smoke_or_process_label(self) -> None:
        command = ["C:/cmake.exe", "-DSPARK_INSTALLER=C:\\build\\bin\\Release\\SparkInstaller.exe", "-P", "s.cmake"]
        unlabelled = self.check_json(
            ctest_json(self.binary_entry("Smoke", command, ["installer", "unit"])), installed=("SparkInstaller",)
        )
        self.assertEqual(unlabelled.returncode, 1)
        self.assertIn("shipped binary SparkInstaller", unlabelled.stderr)

        labelled = self.check_json(
            ctest_json(
                self.binary_entry("Version", ["/b/SparkInstaller", "--version"], ["installer"]),
                self.binary_entry("Smoke", command, ["installer", "integration", "process"]),
            ),
            installed=("SparkInstaller",),
        )
        self.assertEqual(labelled.returncode, 0, labelled.stderr)

    def test_binary_driven_through_an_argument_or_environment_counts(self) -> None:
        by_argument = self.binary_entry(
            "GatewaySmoke", ["/b/SparkAutomation", "--", "t.py", "--gateway", "/b/SparkGateway"], ["process"]
        )
        by_environment = self.binary_entry(
            "DaemonSmoke", ["/b/SparkTests"], ["integration"], environment=["SPARK_TEST_DAEMON_PATH=/b/SparkDaemon"]
        )
        result = self.check_json(
            ctest_json(by_argument, by_environment), installed=("SparkAutomation", "SparkDaemon", "SparkGateway")
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_shipped_built_binary_with_no_tests_at_all_is_rejected(self) -> None:
        # The fail-open case: SparkServer is built and installed in this tree but
        # no registered test names it. Built-ness comes from the codemodel, so
        # the binary cannot drop out of enforcement by having no tests.
        installer_smoke = self.binary_entry(
            "InstallerSmoke", ["/b/cmake", "-DSPARK_INSTALLER=/b/SparkInstaller", "-P", "s.cmake"], ["integration"]
        )
        result = self.check_json(
            ctest_json(installer_smoke, ctest_entry("Unrelated", TIMEOUT=30.0, LABELS=["unit"])),
            installed=("SparkInstaller", "SparkServer"),
            system_name="Linux",
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "shipped binary SparkServer is built in this tree but no registered test runs it", result.stderr
        )
        self.assertNotIn("SparkInstaller", result.stderr)

    def test_unshipped_or_unbuilt_executables_need_no_lane(self) -> None:
        # DevTool is built but has no install rule; SparkServer is shipped by the
        # project but not built in this tree, so the codemodel does not list it.
        smoke = self.binary_entry(
            "InstallerSmoke", ["/b/cmake", "-DSPARK_INSTALLER=/b/SparkInstaller", "-P", "s.cmake"], ["integration"]
        )
        result = self.check_json(ctest_json(smoke), installed=("SparkInstaller",), uninstalled=("DevTool",))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_platform_scoped_gap_applies_only_on_its_platforms(self) -> None:
        compiler_smoke = self.binary_entry(
            "ShaderSmoke", ["/b/cmake", "-DSPARK_SHADER_COMPILER=/b/SparkShaderCompiler", "-P", "s.cmake"], ["process"]
        )
        unrelated = ctest_entry("Unrelated", TIMEOUT=30.0, LABELS=["unit"])
        shipped = ("SparkShaderCompiler",)

        linux_gap = self.check_json(ctest_json(unrelated), installed=shipped, system_name="Linux")
        self.assertEqual(linux_gap.returncode, 0, linux_gap.stderr)
        self.assertIn("known gap: shipped binary SparkShaderCompiler", linux_gap.stdout)

        windows_missing = self.check_json(ctest_json(unrelated), installed=shipped, system_name="Windows")
        self.assertEqual(windows_missing.returncode, 1)
        self.assertIn("shipped binary SparkShaderCompiler is built in this tree", windows_missing.stderr)

        windows_lane = self.check_json(ctest_json(compiler_smoke), installed=shipped, system_name="Windows")
        self.assertEqual(windows_lane.returncode, 0, windows_lane.stderr)

        linux_stale = self.check_json(ctest_json(compiler_smoke), installed=shipped, system_name="Linux")
        self.assertEqual(linux_stale.returncode, 1)
        self.assertIn("SparkShaderCompiler now has a behavioural lane on Linux", linux_stale.stderr)

    def test_ctest_json_needs_a_build_tree_with_a_codemodel(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "ctest.json"
            path.write_text(ctest_json(ctest_entry("A", TIMEOUT=30.0, LABELS=["unit"])), encoding="utf-8")
            alone = self.run_validator("--ctest-json", str(path))
            self.assertEqual(alone.returncode, 2)
            self.assertIn("--ctest-json and --build-dir must be given together", alone.stderr)

            unconfigured = self.run_validator("--ctest-json", str(path), "--build-dir", temporary)
            self.assertEqual(unconfigured.returncode, 2)
            self.assertIn("no CMake file-API reply", unconfigured.stderr)

            ships_nothing = Path(temporary) / "empty-build"
            write_build_tree(ships_nothing, installed=(), uninstalled=("DevTool",))
            empty = self.run_validator("--ctest-json", str(path), "--build-dir", str(ships_nothing))
            self.assertEqual(empty.returncode, 2)
            self.assertIn("builds no installed executable", empty.stderr)

    def test_launcher_behavioural_lane_is_required_after_headless_entry_point(self) -> None:
        help_only = self.binary_entry("LauncherHelp", ["/b/SparkLauncher", "--help"], ["launcher", "process"])
        missing = self.check_json(ctest_json(help_only), installed=("SparkLauncher",), system_name="Linux")
        self.assertEqual(missing.returncode, 1)
        self.assertIn("shipped binary SparkLauncher", missing.stderr)

        driven = self.binary_entry(
            "LauncherSmoke",
            ["/b/SparkLauncher", "--validate-launch-request", "fixture.sparkproject"],
            ["launcher", "integration", "process"],
        )
        valid = self.check_json(ctest_json(help_only, driven), installed=("SparkLauncher",))
        self.assertEqual(valid.returncode, 0, valid.stderr)

    def test_configured_shipped_executables_are_installed_executables_of_every_configuration(self) -> None:
        policy = self._policy_module()
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            write_build_tree(build, ("SparkServer", "SparkEngine"), "Linux", uninstalled=("DevTool",))
            self.assertEqual(policy.configured_shipped_executables(build), (["SparkEngine", "SparkServer"], "Linux"))

    def test_every_known_gap_is_an_installed_executable(self) -> None:
        policy = self._policy_module()
        sources = [
            (REPO_ROOT / relative).read_text(encoding="utf-8")
            for relative in policy._git_tracked_cmake_sources(REPO_ROOT) or []
            if (REPO_ROOT / relative).is_file() and not relative.startswith("ThirdParty/")
        ]
        for target in policy.KNOWN_BINARY_LANE_GAPS:
            with self.subTest(target=target):
                pattern = re.compile(rf"install\(\s*TARGETS\s+{re.escape(target)}\b[^)]*RUNTIME")
                self.assertTrue(any(pattern.search(text) for text in sources), f"{target} is not installed")

    def test_root_cmakelists_requests_the_codemodel_for_test_trees(self) -> None:
        text = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("cmake_file_api(QUERY API_VERSION 1 CODEMODEL 2)", text)

    # -- first-party discovery ----------------------------------------------------

    def test_discovery_skips_vendored_build_trees_and_commented_registrations(self) -> None:
        policy = self._policy_module()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            files = {
                "CMakeLists.txt": "add_subdirectory(Product)\n",
                "Product/CMakeLists.txt": "add_test(NAME Hangs COMMAND hang)\n",
                "cmake/Helper.cmake": "# add_test(NAME Ghost COMMAND ghost)\n",
                "cmake/Policy.cmake": "function(f)\n  add_test(NAME Fn COMMAND fn)\nendfunction()\n",
                "ThirdParty/lib/CMakeLists.txt": "add_test(NAME Vendored COMMAND vendored)\n",
                "build/tree/CMakeCache.txt": "",
                "build/tree/_deps/x/CMakeLists.txt": "add_test(NAME Fetched COMMAND fetched)\n",
                "Product/notes.txt": "add_test(NAME NotCMake COMMAND x)\n",
            }
            for relative, content in files.items():
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding="utf-8")
            found = [path.relative_to(root).as_posix() for path in policy.discover_cmake_sources(root)]
        self.assertEqual(found, ["Product/CMakeLists.txt", "cmake/Policy.cmake"])

    def test_discovery_walks_an_untracked_tree_nested_inside_another_repository(self) -> None:
        # git ls-files from a nested, untracked copy succeeds but lists nothing;
        # discovery must fall back to the walk instead of reporting no files.
        policy = self._policy_module()
        with tempfile.TemporaryDirectory() as temporary:
            outer = Path(temporary)
            # Strip inherited GIT_* variables: with GIT_DIR/GIT_WORK_TREE set by a caller,
            # `git init` re-initialises that repository (and rewrites its core.worktree)
            # instead of creating one in the temporary directory.
            clean_env = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
            init = subprocess.run(["git", "init", "-q", str(outer)], capture_output=True, check=False, env=clean_env)
            if init.returncode != 0:
                self.skipTest("git is unavailable to create the enclosing repository")
            root = outer / "copy"
            (root / "Product").mkdir(parents=True)
            (root / "CMakeLists.txt").write_text("add_subdirectory(Product)\n", encoding="utf-8")
            (root / "Product" / "CMakeLists.txt").write_text("add_test(NAME Hangs COMMAND hang)\n", encoding="utf-8")
            found = [path.relative_to(root).as_posix() for path in policy.discover_cmake_sources(root)]
        self.assertEqual(found, ["Product/CMakeLists.txt"])

    def test_default_static_view_scans_every_first_party_registration_file(self) -> None:
        policy = self._policy_module()
        found = {path.relative_to(REPO_ROOT).as_posix() for path in policy.discover_cmake_sources()}
        self.assertIn("Tests/CMakeLists.txt", found)
        for relative in PRODUCT_TEST_SOURCES:
            self.assertIn(relative, found, f"default static view no longer scans {relative}")
        self.assertFalse(any(path.startswith("ThirdParty/") for path in found), sorted(found))

    def test_default_static_view_passes_on_the_repository(self) -> None:
        result = self.run_validator()
        self.assertEqual(result.returncode, 0, result.stderr)
        count = int(result.stdout.split("OK (", 1)[1].split(" ", 1)[0])
        tests_only = len(self._policy_module().parse_cmake(TESTS_CMAKE.read_text(encoding="utf-8"))[0])
        # Product registrations must be counted on top of Tests/CMakeLists.txt.
        self.assertGreater(count, tests_only + len(PRODUCT_TEST_SOURCES), result.stdout)

    def test_every_product_registration_file_is_bounded_and_labelled(self) -> None:
        for relative in PRODUCT_TEST_SOURCES:
            with self.subTest(file=relative):
                result = self.run_validator("--cmake-lists", str(REPO_ROOT / relative))
                self.assertEqual(result.returncode, 0, result.stderr)

    # -- CI wiring ------------------------------------------------------------------

    def test_configured_tree_policy_is_enforced_in_linux_gcc_and_windows_lanes(self) -> None:
        workflow = BUILD_WORKFLOW.read_text(encoding="utf-8")
        for job in ("build-linux-gcc", "build-windows-vs2022"):
            with self.subTest(job=job):
                body = self._job_body(workflow, job)
                step = body.split("- name: Enforce configured-tree CTest policy", 1)
                self.assertEqual(len(step), 2, f"{job} has no configured-tree CTest policy step")
                script = step[1].split("\n    - name:", 1)[0]
                self.assertIn("set -euo pipefail", script)
                self.assertIn("--no-tests=error", script)
                self.assertIn("--show-only=json-v1 > build/ctest-policy-inventory.json", script)
                self.assertIn(
                    "Tools/validate_ctest_policy.py --ctest-json build/ctest-policy-inventory.json --build-dir build",
                    script,
                )
                self.assertNotIn("continue-on-error", script)
                self.assertNotIn("|| true", script)

    def test_static_policy_runs_in_ci_tool_validation(self) -> None:
        body = self._job_body(BUILD_WORKFLOW.read_text(encoding="utf-8"), "validate-ci-tools")
        self.assertIn("run: python3 Tools/validate_ctest_policy.py\n", body)
        self.assertIn("run: python3 Tests/Tools/test_validate_ctest_policy.py\n", body)

    @staticmethod
    def _job_body(workflow: str, job: str) -> str:
        marker = f"\n  {job}:\n"
        start = workflow.index(marker) + len(marker)
        next_job = re.search(r"\n  [A-Za-z0-9_-]+:\n", workflow[start:])
        return workflow[start : start + next_job.start()] if next_job else workflow[start:]

    # -- the real tree ----------------------------------------------------------

    def test_repository_tests_cmakelists_satisfies_policy(self) -> None:
        result = self.run_validator("--cmake-lists", str(TESTS_CMAKE))
        self.assertEqual(result.returncode, 0, result.stderr)
        # Guard against a parser regression that "passes" by seeing nothing.
        count = int(result.stdout.split("OK (", 1)[1].split(" ", 1)[0])
        self.assertGreaterEqual(count, 50, result.stdout)

    def test_main_suite_is_selectable_by_documented_unit_and_integration_labels(self) -> None:
        sys.path.insert(0, str(REPO_ROOT / "Tools"))
        try:
            import validate_ctest_policy as policy
        finally:
            sys.path.pop(0)
        _, policies = policy.parse_cmake(TESTS_CMAKE.read_text(encoding="utf-8"))
        labels = ";".join(policies["SparkEngineTests"].labels).split(";")
        self.assertIn("unit", labels)
        self.assertIn("integration", labels)

    def test_policy_validator_is_registered_as_a_bounded_ctest(self) -> None:
        text = TESTS_CMAKE.read_text(encoding="utf-8")
        self.assertTrue("NAME SparkCTestTimeoutPolicy" in text, "policy CTest is not registered")
        self.assertTrue(
            "Tests/Tools/test_validate_ctest_policy.py" in text, "policy CTest does not run this suite"
        )
        _, policies = self._policy_module().parse_cmake(text)
        self.assertTrue(policies["SparkCTestTimeoutPolicy"].timeouts, "policy CTest has no TIMEOUT")

    @staticmethod
    def _policy_module():
        sys.path.insert(0, str(REPO_ROOT / "Tools"))
        try:
            import validate_ctest_policy as policy
        finally:
            sys.path.pop(0)
        return policy


if __name__ == "__main__":
    unittest.main()
