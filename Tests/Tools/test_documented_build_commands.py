#!/usr/bin/env python3
"""Tests for tools/site-data/documented_commands.py (CI-120).

The fixture cases drive the checker against a small, self-contained preset set
so each rule is exercised in isolation; the repository case runs the checker
over the real documentation surface against the real CMakePresets.json, which
is the gate that keeps README/wiki/CLAUDE.md quick starts aligned.
"""

import sys
import textwrap
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "site-data"))

import documented_commands  # noqa: E402
from contract_selectors import CMakePresetIndex  # noqa: E402

FIXTURE_PRESETS = {
    "version": 6,
    "configurePresets": [
        {"name": "default", "hidden": True, "binaryDir": "${sourceDir}/build/${presetName}"},
        {
            "name": "windows-release",
            "inherits": "default",
            "generator": "Visual Studio 17 2022",
            "architecture": "x64",
            "toolset": "v143",
            "cacheVariables": {"CMAKE_BUILD_TYPE": "Release"},
        },
        {
            "name": "windows-shipping",
            "inherits": "default",
            "generator": "Visual Studio 17 2022",
            "architecture": "x64",
            "toolset": "v143",
            "cacheVariables": {"CMAKE_BUILD_TYPE": "MinSizeRel"},
        },
        {"name": "linux-gcc-release", "inherits": "default", "cacheVariables": {"CMAKE_BUILD_TYPE": "Release"}},
    ],
    "buildPresets": [
        {"name": "windows-release", "configurePreset": "windows-release", "configuration": "Release"},
        {"name": "windows-shipping", "configurePreset": "windows-shipping", "configuration": "MinSizeRel"},
        {"name": "linux-gcc-release", "configurePreset": "linux-gcc-release"},
    ],
    "testPresets": [{"name": "default", "configurePreset": "linux-gcc-release"}],
}


def fenced(body: str, language: str = "bash") -> str:
    return f"# Doc\n\n```{language}\n{textwrap.dedent(body).strip()}\n```\n"


class DocumentedCommandRules(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.index = CMakePresetIndex(FIXTURE_PRESETS)

    def findings(self, text: str) -> list[str]:
        return [finding.message for finding in documented_commands.check_text("doc.md", text, self.index)]

    def test_preset_tree_with_matching_configuration_passes(self):
        text = fenced(
            """
            cmake --preset windows-release
            cmake --build build/windows-release --config Release
            ctest --test-dir build/windows-release -C Release --output-on-failure
            """
        )
        self.assertEqual(self.findings(text), [])

    def test_bare_build_tree_after_preset_configure_fails(self):
        # The historical CLAUDE.md quick start: the preset writes build/windows-release.
        messages = self.findings(
            fenced(
                """
                cmake --preset windows-release
                cmake --build build --config Release
                cd build && ctest --output-on-failure
                """
            )
        )
        self.assertEqual(len(messages), 2, messages)
        self.assertTrue(all("build/windows-release" in message for message in messages), messages)

    def test_bare_tree_after_preset_fails_even_when_document_declared_it_elsewhere(self):
        text = fenced("cmake -B build -DBUILD_TESTS=ON") + fenced(
            """
            cmake --preset linux-gcc-release
            cmake --build build
            """
        )
        messages = self.findings(text)
        self.assertEqual(len(messages), 1, messages)
        self.assertIn("preset configured above", messages[0])

    def test_adhoc_tree_configured_earlier_in_document_passes(self):
        configure = fenced("cmake -B build -DBUILD_TESTS=ON")
        text = configure + "\nThen:\n" + fenced("cmake --build build\nctest --test-dir build")
        self.assertEqual(self.findings(text), [])

    def test_undeclared_tree_fails(self):
        messages = self.findings(fenced("ctest --test-dir build -C Release --output-on-failure"))
        self.assertEqual(len(messages), 1, messages)
        self.assertIn("neither a preset binaryDir", messages[0])

    def test_unknown_preset_fails_per_family(self):
        messages = self.findings(
            fenced(
                """
                cmake --preset windows-relase
                cmake --build --preset linux-gcc-debug
                ctest --preset windows-release
                """
            )
        )
        self.assertEqual(len(messages), 3, messages)
        self.assertIn("configure preset", messages[0])
        self.assertIn("build preset", messages[1])
        self.assertIn("test preset", messages[2])

    def test_placeholder_names_are_not_resolved(self):
        text = "Re-run `cmake --preset <your-preset>` then `cmake --build build/<preset> --config Release`.\n"
        self.assertEqual(self.findings(text), [])

    def test_multi_config_tree_without_configuration_fails(self):
        messages = self.findings(fenced("cmake --build build/windows-release\nctest --test-dir build/windows-release"))
        self.assertEqual(len(messages), 2, messages)
        self.assertTrue(all("--config Release" in m or "-C Release" in m for m in messages), messages)

    def test_configuration_mismatch_fails(self):
        messages = self.findings(
            fenced(
                """
                cmake --build build/windows-shipping --config Release
                cmake --build build/linux-gcc-release --config Debug
                """
            )
        )
        self.assertEqual(len(messages), 2, messages)
        self.assertIn("MinSizeRel", messages[0])
        self.assertIn("Release", messages[1])

    def test_single_config_tree_needs_no_configuration(self):
        self.assertEqual(self.findings(fenced("cmake --build build/linux-gcc-release -j4")), [])

    def test_adhoc_configure_into_preset_tree_fails(self):
        messages = self.findings(fenced("cmake -B build/linux-gcc-release -DSPARK_ENABLE_SOAK_TESTS=ON"))
        self.assertEqual(len(messages), 1, messages)
        self.assertIn("cmake --preset linux-gcc-release", messages[0])

    def test_visual_studio_configure_must_pin_preset_toolset(self):
        unpinned = self.findings(fenced('cmake -B build -G "Visual Studio 17 2022" -A x64'))
        self.assertEqual(len(unpinned), 1, unpinned)
        self.assertIn("-A x64 -T v143", unpinned[0])
        pinned = fenced(
            """
            cmake -B build -G "Visual Studio 17 2022" -A x64 -T v143
            cmake --build build --config Release
            """
        )
        self.assertEqual(self.findings(pinned), [])
        # A generator no preset uses carries no preset pin to match.
        self.assertEqual(self.findings(fenced('cmake -G "Visual Studio 18 2026" -A x64 ...')), [])

    def test_windows_shell_backslash_paths_resolve(self):
        text = fenced(
            "cmake --preset windows-release\ncmake --build build\\windows-release --config Release", "powershell"
        )
        self.assertEqual(self.findings(text), [])
        broken = fenced("cmake --preset windows-release\ncmake --build build\\windows-relase --config Release", "cmd")
        self.assertEqual(len(self.findings(broken)), 1)

    def test_continuations_comments_and_non_shell_blocks(self):
        text = fenced(
            """
            cmake --preset linux-gcc-release \\
                -DENABLE_LTO=OFF   # cmake --build build would be wrong here
            cmake --build build/linux-gcc-release
            """
        ) + fenced("cmake --build build  # CMake source, not a shell command", "cmake")
        self.assertEqual(self.findings(text), [])

    def test_other_presets_tree_after_preset_configure_fails(self):
        messages = self.findings(
            fenced(
                """
                cmake --preset linux-gcc-release
                cmake --build build/windows-shipping --config MinSizeRel
                ctest --test-dir build/windows-release -C Release
                cmake --build build/linux-gcc-release
                """
            )
        )
        self.assertEqual(len(messages), 2, messages)
        self.assertTrue(all("build/linux-gcc-release" in message for message in messages), messages)
        self.assertIn("preset 'windows-shipping'", messages[0])

    def test_install_without_configuration_on_non_release_multi_config_preset_fails(self):
        messages = self.findings(
            fenced(
                """
                cmake --preset windows-shipping
                cmake --install build/windows-shipping --prefix out
                cmake --install build/windows-shipping --config Release --prefix out
                """
            )
        )
        self.assertEqual(len(messages), 2, messages)
        self.assertIn("--config MinSizeRel", messages[0])
        self.assertIn("installs or packages Release", messages[0])
        self.assertIn("whose configuration is MinSizeRel", messages[1])
        # A multi-config install falls back to Release, so a Release preset may omit it.
        self.assertEqual(self.findings(fenced("cmake --install build/windows-release --prefix out")), [])
        self.assertEqual(
            self.findings(fenced("cmake --install build/windows-shipping --config MinSizeRel --prefix out")), []
        )

    def test_cpack_trees_and_configurations_are_checked(self):
        stale = self.findings(
            fenced(
                """
                cmake --preset windows-shipping
                cpack --config build/CPackConfig.cmake -C MinSizeRel
                """
            )
        )
        self.assertEqual(len(stale), 1, stale)
        self.assertIn("build/windows-shipping", stale[0])
        missing = self.findings(fenced("cpack --config build/windows-shipping/CPackConfig.cmake"))
        self.assertEqual(len(missing), 1, missing)
        self.assertIn("-C MinSizeRel", missing[0])
        mismatched = self.findings(fenced("cd build/windows-shipping\ncpack -C Release"))
        self.assertEqual(len(mismatched), 1, mismatched)
        self.assertIn("MinSizeRel", mismatched[0])
        passing = fenced(
            """
            cmake --preset windows-shipping
            cpack --config build/windows-shipping/CPackConfig.cmake -C MinSizeRel
            cd build/windows-shipping && cpack -C MinSizeRel -G ZIP
            """
        )
        self.assertEqual(self.findings(passing), [])

    def test_repository_configure_scripts_declare_the_build_tree(self):
        text = fenced("./generate.sh release -g Ninja\n./build.sh release") + fenced(
            "cmake --install build --prefix out\nctest --test-dir build"
        )
        self.assertEqual(self.findings(text), [])
        windows = fenced("generate.bat\n.\\build.ps1 -config Release", "powershell") + fenced(
            "cmake --build build --config Release"
        )
        self.assertEqual(self.findings(windows), [])
        # Mentioning the script is not running it.
        self.assertEqual(len(self.findings(fenced("cat build.sh\nctest --test-dir build"))), 1)

    def test_toolset_host_suffix_matches_the_pinned_version(self):
        self.assertEqual(self.findings(fenced('cmake -B out -G "Visual Studio 17 2022" -A x64 -T v143,host=x64')), [])
        messages = self.findings(fenced('cmake -B out -G "Visual Studio 17 2022" -A x64 -T v145'))
        self.assertEqual(len(messages), 1, messages)

    def test_line_numbers_point_at_the_command(self):
        text = "Intro\n\n```bash\ncmake --preset linux-gcc-release\n\ncmake --build build\n```\n"
        findings = documented_commands.check_text("doc.md", text, self.index)
        self.assertEqual([finding.line for finding in findings], [6])


class DocumentationSurface(unittest.TestCase):
    def test_surface_covers_quick_starts_and_excludes_generated_pages(self):
        documents = {path.relative_to(REPO_ROOT).as_posix() for path in documented_commands.documented_markdown()}
        for required in ("README.md", "CLAUDE.md", "wiki/getting-started/Getting-Started.md",
                         "wiki/advanced/Build-System-and-CMake-Modules.md", ".github/prompts/build-test.prompt.md"):
            self.assertIn(required, documents)
        self.assertFalse(any(path.startswith(("docs/api/", "wiki/reference/")) for path in documents))

    def test_repository_documentation_agrees_with_presets(self):
        findings = documented_commands.check_documents()
        self.assertEqual([str(finding) for finding in findings], [])


if __name__ == "__main__":
    unittest.main()
