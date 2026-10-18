"""REL-100 contract checks for SparkBuild's configured product version."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPARKBUILD_CMAKE = ROOT / "SparkBuild" / "CMakeLists.txt"
SPARKBUILD_MAIN = ROOT / "SparkBuild" / "src" / "main.cpp"
SPARKBUILD_APP = ROOT / "SparkBuild" / "src" / "SparkBuild.cpp"


class SparkBuildVersionContractTests(unittest.TestCase):
    def test_version_output_and_banner_are_bound_to_configured_project_version(self) -> None:
        cmake = SPARKBUILD_CMAKE.read_text(encoding="utf-8")
        main = SPARKBUILD_MAIN.read_text(encoding="utf-8")
        app = SPARKBUILD_APP.read_text(encoding="utf-8")

        self.assertRegex(
            cmake,
            r"target_compile_definitions\(SparkBuild PRIVATE(?s:.*?)"
            r"SPARK_BUILD_VERSION=\\\"\$\{PROJECT_VERSION\}\\\"",
        )
        self.assertIn("#ifndef SPARK_BUILD_VERSION", main)
        self.assertIn('std::cout << "SparkBuild v" SPARK_BUILD_VERSION', main)
        self.assertIn('"SparkBuild - SparkEngine Build Tool v" SPARK_BUILD_VERSION', app)
        self.assertNotIn("v2.1.0", main)
        self.assertNotIn("v2.1", app)

        self.assertRegex(cmake, r"add_test\(NAME SparkBuildVersion\b")
        self.assertRegex(
            cmake,
            r"add_dependencies\(SparkBuildProcessRunnerTests\s+SparkBuild\)",
        )
        self.assertIn(
            "-DSPARK_EXPECTED_VERSION_OUTPUT=SparkBuild v${PROJECT_VERSION}",
            cmake,
        )


if __name__ == "__main__":
    unittest.main()
