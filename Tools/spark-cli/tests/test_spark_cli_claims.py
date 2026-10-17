"""ASSET-220: every advertised CLI and packager claim is tied to a test that exercises it.

Tools/spark-cli/claims.json lists one entry per claim made by the CLI README or the
Game Packaging wiki page: the subcommands and options it concerns and the tests that
prove it (unittest ids under Tools/spark-cli/tests, or SparkTests TEST names). This
contract fails when

- a build_parser() subcommand, alias or option is missing from the README;
- a README command line or `--flag` token, or a parser (command, option) pair, has no claim;
- a claim has no tests, names a test that does not exist, or names a command or option the
  parser does not have;
- the CPack "tools" component description names a tool that no tools-component install rule
  ships.

Mutation cases (an undocumented option, a dropped test, an unshipped tool) fail by name.
Registered as the CTest CLI_ClaimsMatchBehavior; SparkCliContract's discover run includes it.
"""

import argparse
import copy
import importlib.util
import json
import re
import sys
import unittest
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
CLI_DIR = TESTS_DIR.parent
REPO_ROOT = CLI_DIR.parents[1]
README = CLI_DIR / "README.md"
CLAIMS = CLI_DIR / "claims.json"
CLAIM_SOURCES = ("Tools/spark-cli/README.md", "wiki/gameplay-tools/Game-Packaging.md")

SPEC = importlib.util.spec_from_file_location("spark_cli_for_claims", CLI_DIR / "spark_cli.py")
spark_cli = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(spark_cli)

FLAG = re.compile(r"(?<![\w-])--[a-z][a-z0-9-]*")


def parser_surface(parser: argparse.ArgumentParser) -> dict[str, set[str]]:
    """{command: long options} for every subcommand and alias ('pak inspect' for nested ones)."""
    surface: dict[str, set[str]] = {}

    def walk(current: argparse.ArgumentParser, prefix: str) -> None:
        for action in current._actions:
            if isinstance(action, argparse._SubParsersAction):
                for name, sub in action.choices.items():
                    command = f"{prefix} {name}".strip()
                    surface[command] = {
                        option
                        for sub_action in sub._actions
                        for option in sub_action.option_strings
                        if option.startswith("--") and option != "--help"
                    }
                    walk(sub, command)

    walk(parser, "")
    # A command that only groups nested subcommands ('pak') has no surface of its own.
    return {command: options for command, options in surface.items()
            if not any(other.startswith(command + " ") for other in surface)}


def readme_command_lines(text: str) -> list[tuple[str, list[str]]]:
    """(command, options) of every fenced `spark_cli.py <command> ...` line."""
    lines = []
    in_fence = False
    for line in text.splitlines():
        if line.strip().startswith("```"):
            in_fence = not in_fence
            continue
        if not in_fence or "spark_cli.py " not in line:
            continue
        tokens = line.split("spark_cli.py ", 1)[1].split()
        if not tokens:
            continue
        command = tokens[0]
        rest = tokens[1:]
        if command == "pak" and rest:
            command, rest = f"pak {rest[0]}", rest[1:]
        options = []
        for token in rest:
            if token == "--":
                break  # everything after -- is forwarded to another program
            if token.startswith("--"):
                options.append(token)
        lines.append((command, options))
    return lines


def readme_prose_flags(text: str) -> set[str]:
    """Every `--flag` mentioned outside fenced command blocks."""
    prose = []
    in_fence = False
    for line in text.splitlines():
        if line.strip().startswith("```"):
            in_fence = not in_fence
            continue
        if not in_fence:
            prose.append(line)
    return set(FLAG.findall("\n".join(prose)))


def python_test_ids() -> set[str]:
    sys.path.insert(0, str(TESTS_DIR))
    suite = unittest.defaultTestLoader.discover(str(TESTS_DIR), pattern="test_*.py", top_level_dir=str(TESTS_DIR))
    ids: set[str] = set()

    def walk(item):
        if isinstance(item, unittest.TestSuite):
            for child in item:
                walk(child)
        else:
            ids.add(item.id())

    walk(suite)
    return ids


def cpp_test_names(root: Path) -> set[str]:
    names: set[str] = set()
    for path in (root / "Tests").rglob("*.cpp"):
        names.update(re.findall(r"\bTEST\(\s*(\w+)\s*\)", path.read_text(encoding="utf-8", errors="replace")))
    return names


def cpack_tool_problems(cmake_text: str, root: Path) -> list[str]:
    """Tools the CPack tools-component description names without a tools-component install rule."""
    match = re.search(r'set\(CPACK_COMPONENT_TOOLS_DESCRIPTION\s+"([^"]*)"\)', cmake_text)
    if not match:
        return ["CPACK_COMPONENT_TOOLS_DESCRIPTION is not set in CMakeLists.txt"]
    description = match.group(1)
    named = description.split("such as", 1)[1] if "such as" in description else ""
    tools = [name for name in re.split(r",\s*|\s+and\s+", named.strip()) if name]
    if not tools:
        return [f"CPack tools description names no tool: {description!r}"]

    rules = []
    for opener in re.finditer(r"\b(install|spark_install_tracked_directory)\s*\(", cmake_text):
        depth, index = 0, opener.end() - 1
        while index < len(cmake_text):
            depth += {"(": 1, ")": -1}.get(cmake_text[index], 0)
            if depth == 0:
                break
            index += 1
        block = cmake_text[opener.start():index + 1]
        if "${SPARK_INSTALL_COMPONENT_TOOLS}" in block:
            rules.append(block)

    problems = []
    for tool in tools:
        as_target = any(re.search(rf"\bTARGETS\s+{re.escape(tool)}\b", rule) for rule in rules)
        as_tree = (root / "Tools" / tool).is_dir() and any(re.search(r"\bSOURCE\s+Tools\b", rule) for rule in rules)
        if not (as_target or as_tree):
            problems.append(f"CPack tools description names {tool}, which no tools-component install rule ships")
    return problems


def check(parser, readme_text: str, claims: dict, python_ids: set[str], cpp_names: set[str]) -> list[str]:
    problems: list[str] = []
    surface = parser_surface(parser)

    # 1. The parser surface is documented.
    for command, options in sorted(surface.items()):
        spelled = (f"spark_cli.py {command}" in readme_text or f"`{command}`" in readme_text
                   or f"`{command.split()[-1]}`" in readme_text)
        if not spelled:
            problems.append(f"undocumented command: {command}")
        for option in sorted(options):
            if option not in readme_text:
                problems.append(f"undocumented option: {command} {option}")

    # 2. Claims are well formed and name real surfaces and tests.
    covered: set[tuple[str, str]] = set()
    claimed_commands: set[str] = set()
    claimed_flags: set[str] = set()
    for claim in claims["claims"]:
        label = claim.get("id", "<no id>")
        if claim.get("source") not in CLAIM_SOURCES:
            problems.append(f"claim {label} has unknown source {claim.get('source')!r}")
        if not claim.get("tests"):
            problems.append(f"claim {label} has no tests")
        for test in claim.get("tests", []):
            known = test in python_ids if "." in test else test in cpp_names
            if not known:
                problems.append(f"claim {label} names missing test {test}")
        if not claim.get("tests"):
            continue  # an untested claim covers nothing
        for command in claim.get("commands", []):
            if command not in surface:
                problems.append(f"claim {label} names unknown command {command}")
                continue
            claimed_commands.add(command)
            for flag in claim.get("flags", []):
                if flag in surface[command]:
                    covered.add((command, flag))
        for flag in claim.get("flags", []):
            claimed_flags.add(flag)
            if not any(flag in surface.get(command, set()) for command in claim.get("commands", [])):
                problems.append(f"claim {label} names {flag}, which none of its commands accepts")

    # 3. Everything advertised is claimed.
    for command, options in sorted(surface.items()):
        if command not in claimed_commands:
            problems.append(f"unclaimed command: {command}")
        for option in sorted(options):
            if (command, option) not in covered:
                problems.append(f"unclaimed option: {command} {option}")
    for command, options in readme_command_lines(readme_text):
        if command not in surface:
            problems.append(f"README runs unknown command: {command}")
            continue
        for option in options:
            if (command, option) not in covered:
                problems.append(f"README example has unclaimed option: {command} {option}")
    for flag in sorted(readme_prose_flags(readme_text)):
        if flag not in claimed_flags:
            problems.append(f"README mentions unclaimed flag: {flag}")
    return problems


class ClaimsContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.readme = README.read_text(encoding="utf-8")
        cls.claims = json.loads(CLAIMS.read_text(encoding="utf-8"))
        cls.python_ids = python_test_ids()
        cls.cpp_names = cpp_test_names(REPO_ROOT)
        cls.cmake = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

    def problems(self, parser=None, readme=None, claims=None):
        return check(parser or spark_cli.build_parser(), self.readme if readme is None else readme,
                     self.claims if claims is None else claims, self.python_ids, self.cpp_names)

    def test_every_claim_matches_behavior(self):
        problems = self.problems()
        self.assertEqual(problems, [], "\n".join(problems))

    def test_every_claim_source_is_a_real_document(self):
        for source in CLAIM_SOURCES:
            self.assertTrue((REPO_ROOT / source).is_file(), source)
        sources = {claim["source"] for claim in self.claims["claims"]}
        self.assertEqual(sources, set(CLAIM_SOURCES))

    def test_cpack_tools_description_matches_install_rules(self):
        problems = cpack_tool_problems(self.cmake, REPO_ROOT)
        self.assertEqual(problems, [], "\n".join(problems))

    def test_undocumented_option_fails(self):
        parser = spark_cli.build_parser()
        subparsers = next(a for a in parser._actions if isinstance(a, argparse._SubParsersAction))
        subparsers.choices["new"].add_argument("--licence-header")
        problems = self.problems(parser=parser)
        self.assertIn("undocumented option: new --licence-header", problems)
        self.assertIn("unclaimed option: new --licence-header", problems)

    def test_dropped_mapped_test_fails(self):
        claims = copy.deepcopy(self.claims)
        claim = next(c for c in claims["claims"] if c["id"] == "package-platform-host-only")
        claim["tests"] = ["test_spark_cli.SparkPackageTests.test_package_cross_compiles_for_every_platform"]
        problems = self.problems(claims=claims)
        self.assertIn(
            "claim package-platform-host-only names missing test "
            "test_spark_cli.SparkPackageTests.test_package_cross_compiles_for_every_platform",
            problems,
        )

    def test_claim_without_tests_fails(self):
        claims = copy.deepcopy(self.claims)
        next(c for c in claims["claims"] if c["id"] == "validate-strict-noop")["tests"] = []
        problems = self.problems(claims=claims)
        self.assertIn("claim validate-strict-noop has no tests", problems)
        self.assertIn("unclaimed option: validate --strict", problems)

    def test_readme_flag_without_claim_fails(self):
        readme = self.readme + "\nPass `--frobnicate` to make packages smaller.\n"
        self.assertIn("README mentions unclaimed flag: --frobnicate", self.problems(readme=readme))

    def test_missing_cpp_test_fails(self):
        claims = copy.deepcopy(self.claims)
        claim = next(c for c in claims["claims"] if c["source"] == "wiki/gameplay-tools/Game-Packaging.md")
        claim["tests"] = ["GamePackager_ASSET220_ThisTestDoesNotExist"]
        self.assertIn(f"claim {claim['id']} names missing test GamePackager_ASSET220_ThisTestDoesNotExist",
                      self.problems(claims=claims))

    def test_unshipped_cpack_tool_fails(self):
        cmake = self.cmake.replace("such as spark-cli and SparkShaderCompiler",
                                   "such as spark-cli, SparkLevelBaker and SparkShaderCompiler")
        self.assertNotEqual(cmake, self.cmake)
        self.assertEqual(
            cpack_tool_problems(cmake, REPO_ROOT),
            ["CPack tools description names SparkLevelBaker, which no tools-component install rule ships"],
        )


if __name__ == "__main__":
    unittest.main()
