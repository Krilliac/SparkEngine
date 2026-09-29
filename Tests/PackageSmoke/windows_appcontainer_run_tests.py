#!/usr/bin/env python3
"""Host-independent contract for the Windows AppContainer package-isolation verdict (RDY-020).

The AppContainer launcher itself needs Windows; FPSPackage_RepositoryUnreachable
runs it. These cases pin the containment verdict, the layout guard and the
command lines on every host, so a verdict that stopped checking would fail here.
"""
import unittest
from unittest.mock import patch

import windows_appcontainer_run as isolation

SOURCE = r"D:\Work\SparkEngine"
BUILD = r"D:\Work\SparkEngine\build\windows-release"
RUN = r"C:\Temp\m310-1234\u\Release\run-1"
OUTPUT = RUN + r"\runs"
FORBIDDEN = [SOURCE, BUILD]
PASS = isolation.RunResult(0, "", "")
DENIED = isolation.RunResult(1, "", "Access is denied.\n")


def good_results():
    return {
        "canary-source": DENIED,
        "canary-build": DENIED,
        "canary-package": isolation.RunResult(0, "[Scene]\nname=Arena\n", ""),
        "nullrhi-positive": isolation.RunResult(
            0, "SPARK_MODULE_READY count=1\nSPARK_FPS_HEADLESS_ARENA objects=72 spawns=4\n", ""),
        "nullrhi-negative": isolation.RunResult(2, "SPARK_HEADLESS_LIFECYCLE initialized=0\n", ""),
        "d3d11-positive": isolation.RunResult(0, "SPARK_D3D11_DEVICE driver=warp\n", ""),
        "d3d11-negative": isolation.RunResult(0, "Warning: Failed to load model\n", ""),
    }


def good_frames():
    screenshots = {"d3d11-positive": OUTPUT + r"\d3d11-positive\fps-visible.png",
                   "d3d11-negative": OUTPUT + r"\d3d11-negative\fps-visible.png"}
    return screenshots, {"d3d11-positive": True, "d3d11-negative": False}


def verdict(results=None, screenshots=None, authored=None):
    default_screenshots, default_authored = good_frames()
    return isolation.containment_verdict(
        good_results() if results is None else results, FORBIDDEN,
        default_screenshots if screenshots is None else screenshots, OUTPUT,
        default_authored if authored is None else authored)


class ContainmentVerdictTests(unittest.TestCase):
    def assertRejected(self, errors, needle):
        self.assertTrue(any(needle in error for error in errors), errors)

    def test_contained_session_passes(self):
        self.assertEqual(verdict(), [])

    def test_readable_repository_canary_fails(self):
        results = good_results()
        results["canary-source"] = isolation.RunResult(0, "cmake_minimum_required(VERSION 3.25)\n", "")
        self.assertRejected(verdict(results), "checkout is reachable")

    def test_readable_build_canary_fails(self):
        results = good_results()
        results["canary-build"] = PASS
        self.assertRejected(verdict(results), "canary-build")

    def test_unreadable_package_canary_fails(self):
        results = good_results()
        results["canary-package"] = DENIED
        self.assertRejected(verdict(results), "could not read the package copy")

    def test_package_canary_without_scene_content_fails(self):
        results = good_results()
        results["canary-package"] = PASS
        self.assertRejected(verdict(results), "could not read the package copy")

    def test_missing_run_fails(self):
        results = good_results()
        del results["d3d11-negative"]
        self.assertRejected(verdict(results), "d3d11-negative did not run")

    def test_output_naming_source_root_fails_in_any_spelling(self):
        for text in (SOURCE + r"\Assets\Scenes\level1.scene", "d:/work/sparkengine/Assets/x.obj",
                     "loading " + BUILD.upper() + r"\bin"):
            with self.subTest(text=text):
                results = good_results()
                results["nullrhi-positive"] = isolation.RunResult(0, "", text)
                self.assertRejected(verdict(results), "names the forbidden root")

    def test_positive_failure_fails(self):
        results = good_results()
        results["nullrhi-positive"] = isolation.RunResult(2, "", "")
        self.assertRejected(verdict(results), "nullrhi-positive: exited 2")

    def test_timeout_fails(self):
        results = good_results()
        results["d3d11-positive"] = isolation.RunResult(None, "", "")
        self.assertRejected(verdict(results), "d3d11-positive: timed out")

    def test_nullrhi_negative_control_that_passed_fails(self):
        results = good_results()
        results["nullrhi-negative"] = PASS
        self.assertRejected(verdict(results), "scene-less package still passed")

    def test_nullrhi_negative_control_with_arena_record_fails(self):
        results = good_results()
        results["nullrhi-negative"] = isolation.RunResult(3, "SPARK_FPS_HEADLESS_ARENA objects=4\n", "")
        self.assertRejected(verdict(results), "emitted an arena record")

    def test_missing_positive_screenshot_fails(self):
        screenshots, authored = good_frames()
        screenshots["d3d11-positive"] = None
        self.assertRejected(verdict(screenshots=screenshots, authored=authored), "saved no screenshot")

    def test_screenshot_outside_output_fails(self):
        screenshots, authored = good_frames()
        screenshots["d3d11-positive"] = SOURCE + r"\fps-visible.png"
        self.assertRejected(verdict(screenshots=screenshots, authored=authored), "outside the run output")

    def test_sibling_of_output_is_outside_it(self):
        screenshots, authored = good_frames()
        screenshots["d3d11-positive"] = OUTPUT + r"-copy\fps-visible.png"
        self.assertRejected(verdict(screenshots=screenshots, authored=authored), "outside the run output")

    def test_positive_frame_without_arena_fails(self):
        screenshots, authored = good_frames()
        authored["d3d11-positive"] = False
        self.assertRejected(verdict(screenshots=screenshots, authored=authored), "not a visible authored")

    def test_d3d11_negative_control_with_visible_frame_fails(self):
        screenshots, authored = good_frames()
        authored["d3d11-negative"] = True
        self.assertRejected(verdict(screenshots=screenshots, authored=authored), "asset-less package still")

    def test_unchecked_negative_frame_fails_closed(self):
        screenshots, authored = good_frames()
        del authored["d3d11-negative"]
        self.assertRejected(verdict(screenshots=screenshots, authored=authored), "asset-less package still")

    def test_d3d11_negative_control_that_crashed_or_saved_nothing_passes(self):
        results = good_results()
        results["d3d11-negative"] = isolation.RunResult(3, "", "")
        self.assertEqual(verdict(results), [])
        screenshots, authored = good_frames()
        screenshots["d3d11-negative"] = None
        del authored["d3d11-negative"]
        self.assertEqual(verdict(screenshots=screenshots, authored=authored), [])


class LayoutTests(unittest.TestCase):
    def test_disjoint_directories_pass(self):
        self.assertEqual(isolation.layout_errors([RUN + r"\package", OUTPUT], FORBIDDEN), [])

    def test_package_inside_source_root_fails(self):
        self.assertTrue(isolation.layout_errors([SOURCE + r"\out\package"], FORBIDDEN))

    def test_output_inside_build_root_fails_case_insensitively(self):
        self.assertTrue(isolation.layout_errors([BUILD.lower() + r"\runs"], FORBIDDEN))

    def test_granted_directory_containing_the_checkout_fails(self):
        self.assertTrue(isolation.layout_errors([r"D:\Work"], FORBIDDEN))

    def test_prefix_named_sibling_is_not_inside(self):
        self.assertEqual(isolation.layout_errors([SOURCE + "-package"], FORBIDDEN), [])

    def test_no_forbidden_roots_fails(self):
        self.assertTrue(isolation.layout_errors([RUN], []))


class CommandLineTests(unittest.TestCase):
    package = RUN + r"\package"

    def test_nullrhi_runs_the_packaged_module_headless(self):
        argv = isolation.engine_argv(self.package, "nullrhi")
        self.assertEqual(argv[0], self.package + r"\bin\SparkEngine.exe")
        self.assertEqual(argv[argv.index("-game") + 1], self.package + r"\bin\SparkGameFPS.dll")
        for flag in ("-headless", "-require-game", "-no-subprocess"):
            self.assertIn(flag, argv)
        self.assertEqual(isolation.phase_environment("nullrhi"), {"SPARK_RHI_BACKEND": "null"})

    def test_d3d11_matches_the_installed_warp_smoke(self):
        argv = isolation.engine_argv(self.package, "d3d11", OUTPUT + r"\visual.exec")
        self.assertNotIn("-headless", argv)
        self.assertEqual(argv[argv.index("-window-size") + 1], "640x360")
        self.assertEqual(argv[argv.index("-exec") + 1], OUTPUT + r"\visual.exec")
        self.assertEqual(isolation.phase_environment("d3d11"),
                         {"SPARK_RHI_BACKEND": "d3d11", "SPARK_D3D11_DRIVER": "warp"})

    def test_d3d11_without_screenshot_script_or_unknown_phase_is_refused(self):
        with self.assertRaises(ValueError):
            isolation.engine_argv(self.package, "d3d11")
        with self.assertRaises(ValueError):
            isolation.engine_argv(self.package, "vulkan")

    def test_non_windows_host_is_refused_before_any_launch(self):
        with patch.object(isolation.sys, "platform", "linux"), \
                patch.object(isolation.sys, "argv", [
                    "windows_appcontainer_run.py", "--source-root", SOURCE, "--build-root", BUILD,
                    "--package", "p", "--scene-less-package", "s", "--asset-less-package", "a",
                    "--output", "o", "--frame-check", "f.ps1"]):
            with self.assertRaises(RuntimeError):
                isolation.main()


if __name__ == "__main__":
    unittest.main()
