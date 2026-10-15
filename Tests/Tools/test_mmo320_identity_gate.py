#!/usr/bin/env python3
"""MOD-320 c2: SparkGameMMO must not regress to demo identity, toy hashing or no-op loads.

Three source-level rules over GameModules/SparkGameMMO/Source, each with a RED
fixture proving the rule fails on the pattern it names:

* no ``std::hash`` or other non-password hash (djb2, FNV, CRC, MD5, SHA-1, a
  "simple hash") anywhere near a password or credential;
* no identifier assigned from, or returned by an ID allocator out of, a clock
  or timestamp (two creates in one second collide, and a restart reissues IDs);
* ``LoadWorld`` is never an empty or constant-return body.

The positive anchors (the account system hashes with ``Spark::PasswordHash``,
the persistence system really parses and returns world state) keep a refactor
that moves the code from silently emptying a rule.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MODULE_SOURCE = REPO_ROOT / "GameModules" / "SparkGameMMO" / "Source"

WEAK_HASH = re.compile(
    r"std::hash\b|\bdjb2\b|\bfnv\w*|\bcrc32\w*|\badler32\w*|\bmd5\w*|\bsha1\w*|\bsimple[_ ]?hash\w*",
    re.IGNORECASE,
)
PASSWORD_WORD = re.compile(r"passw(or)?d|credential", re.IGNORECASE)
TIME_SOURCE = re.compile(
    r"\bGetTimestamp\b|time_since_epoch|\bsystem_clock\b|\bsteady_clock\b|\bhigh_resolution_clock\b|"
    r"\bstd::time\s*\(|(?<![\w:])time\s*\(|\bGetTickCount\w*|\bQueryPerformanceCounter\b"
)
ID_ASSIGNMENT = re.compile(r"(?<!\w)(?:[A-Za-z_]\w*[A-Za-z_])?(?:Id|ID|id)\s*(?:=(?!=)|\()")
ID_BRACE_INIT = re.compile(r"(?<!\w)(?:[A-Za-z_]\w*[A-Za-z_])?(?:Id|ID|id)\s*\{")
ID_ALLOCATOR = re.compile(
    r"\b(?:Generate|Allocate|Next|New|Make|Create)\w*Id\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{"
)
LOAD_WORLD = re.compile(r"\bLoadWorld\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{")
PASSWORD_WINDOW = 5


def strip_comments(text: str) -> str:
    """Remove // and /* */ comments, keeping line numbers and string literals."""
    out: list[str] = []
    index = 0
    state = "code"
    while index < len(text):
        char = text[index]
        pair = text[index : index + 2]
        if state == "code":
            if pair == "//":
                state = "line"
                index += 2
            elif pair == "/*":
                state = "block"
                index += 2
            elif char == '"':
                state = "string"
                out.append(char)
                index += 1
            else:
                out.append(char)
                index += 1
        elif state == "line":
            if char == "\n":
                out.append(char)
                state = "code"
            index += 1
        elif state == "block":
            if pair == "*/":
                state = "code"
                index += 2
            else:
                out.append("\n" if char == "\n" else " ")
                index += 1
        else:
            out.append(char)
            if char == "\\" and index + 1 < len(text):
                out.append(text[index + 1])
                index += 2
                continue
            if char == '"':
                state = "code"
            index += 1
    return "".join(out)


def function_body(text: str, open_brace: int) -> str:
    """Text between the brace at ``open_brace`` and its match."""
    depth = 0
    for index in range(open_brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace + 1 : index]
    return text[open_brace + 1 :]


def line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def find_weak_password_hashes(name: str, code: str) -> list[str]:
    lines = code.splitlines()
    password_lines = [i for i, line in enumerate(lines) if PASSWORD_WORD.search(line)]
    findings = []
    for i, line in enumerate(lines):
        match = WEAK_HASH.search(line)
        if match and any(abs(i - p) <= PASSWORD_WINDOW for p in password_lines):
            findings.append(f"{name}:{i + 1}: '{match.group(0)}' used near a password/credential")
    return findings


def find_timestamp_ids(name: str, code: str) -> list[str]:
    findings = []
    for statement_match in re.finditer(r"[^;{}]+;", code):
        statement = statement_match.group(0)
        assignment = ID_ASSIGNMENT.search(statement)
        if assignment and TIME_SOURCE.search(statement[assignment.end() :]):
            findings.append(f"{name}:{line_of(code, statement_match.start())}: id assigned from a clock")
    for brace in ID_BRACE_INIT.finditer(code):
        if TIME_SOURCE.search(function_body(code, brace.end() - 1)):
            findings.append(f"{name}:{line_of(code, brace.start())}: id brace-initialised from a clock")
    for allocator in ID_ALLOCATOR.finditer(code):
        body = function_body(code, allocator.end() - 1)
        if TIME_SOURCE.search(body):
            findings.append(f"{name}:{line_of(code, allocator.start())}: id allocator reads a clock")
    return findings


def find_empty_load_world(name: str, code: str) -> list[str]:
    findings = []
    for definition in LOAD_WORLD.finditer(code):
        body = function_body(code, definition.end() - 1)
        statements = [s.strip() for s in re.split(r"[;{}]", body) if s.strip()]
        trivial = all(re.fullmatch(r"return\s*(true|false|0|1)?|\(void\)\s*\w+", s) for s in statements)
        if trivial:
            findings.append(f"{name}:{line_of(code, definition.start())}: LoadWorld is empty or returns a constant")
    return findings


def module_sources() -> list[tuple[str, str]]:
    sources = []
    for path in sorted(MODULE_SOURCE.rglob("*")):
        if path.suffix in (".h", ".hpp", ".cpp"):
            relative = path.relative_to(REPO_ROOT).as_posix()
            sources.append((relative, strip_comments(path.read_text(encoding="utf-8", errors="replace"))))
    return sources


class ModuleIdentityGate(unittest.TestCase):
    """Rules applied to the real module sources."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.sources = module_sources()

    def test_sources_were_found(self) -> None:
        self.assertGreater(len(self.sources), 20, "module source tree not scanned; every rule would pass vacuously")

    def test_no_simple_hash_for_passwords(self) -> None:
        findings = [f for name, code in self.sources for f in find_weak_password_hashes(name, code)]
        self.assertEqual(findings, [])

    def test_no_timestamp_ids(self) -> None:
        findings = [f for name, code in self.sources for f in find_timestamp_ids(name, code)]
        self.assertEqual(findings, [])

    def test_no_empty_load_world(self) -> None:
        findings = [f for name, code in self.sources for f in find_empty_load_world(name, code)]
        self.assertEqual(findings, [])

    def test_accounts_use_the_password_hash_primitive(self) -> None:
        account = dict(self.sources)["GameModules/SparkGameMMO/Source/Account/MMOAccountSystem.cpp"]
        self.assertIn("PasswordHash", account)

    def test_load_world_is_defined_and_reads_the_store(self) -> None:
        persistence = dict(self.sources)["GameModules/SparkGameMMO/Source/Persistence/MMOPersistenceSystem.cpp"]
        definitions = list(LOAD_WORLD.finditer(persistence))
        self.assertEqual(len(definitions), 1)
        body = function_body(persistence, definitions[0].end() - 1)
        self.assertIn("LoadGuilds", body)
        self.assertIn("outData", body)


class RuleSelfTests(unittest.TestCase):
    """Each rule must fail on the pattern it names and pass on the legitimate shape."""

    def test_std_hash_password_is_rejected(self) -> None:
        code = strip_comments("std::string Store(const std::string& password)\n{\n  return std::to_string(std::hash<std::string>{}(password));\n}\n")
        self.assertTrue(find_weak_password_hashes("x.cpp", code))

    def test_simple_hash_credential_is_rejected(self) -> None:
        code = strip_comments("uint32_t SimpleHash(const char* s);\nauto h = SimpleHash(credential.c_str());\n")
        self.assertTrue(find_weak_password_hashes("x.cpp", code))

    def test_hash_far_from_passwords_is_allowed(self) -> None:
        filler = "int a = 0;\n" * 12
        code = strip_comments("auto h = std::hash<int>{}(1);\n" + filler + "bool CheckPassword();\n")
        self.assertEqual(find_weak_password_hashes("x.cpp", code), [])

    def test_commented_weak_hash_is_ignored(self) -> None:
        code = strip_comments("// never std::hash the password\nSpark::PasswordHash::Hash(password);\n")
        self.assertEqual(find_weak_password_hashes("x.cpp", code), [])

    def test_timestamp_assignment_is_rejected(self) -> None:
        code = strip_comments("void F()\n{\n  const uint32_t characterId = static_cast<uint32_t>(GetTimestamp());\n}\n")
        self.assertTrue(find_timestamp_ids("x.cpp", code))

    def test_timestamp_member_field_assignment_is_rejected(self) -> None:
        for statement in (
            "account.id = static_cast<uint32_t>(std::time(nullptr));",
            "x.characterId = GetTimestamp();",
            "p->id = GetTimestamp();",
        ):
            code = strip_comments(f"void F()\n{{\n  {statement}\n}}\n")
            self.assertTrue(find_timestamp_ids("x.cpp", code), statement)

    def test_timestamp_brace_or_direct_init_is_rejected(self) -> None:
        for statement in (
            "const uint32_t id{static_cast<uint32_t>(std::time(nullptr))};",
            "const uint32_t characterId(GetTimestamp());",
        ):
            code = strip_comments(f"void F()\n{{\n  {statement}\n}}\n")
            self.assertTrue(find_timestamp_ids("x.cpp", code), statement)

    def test_timestamp_in_id_allocator_is_rejected(self) -> None:
        code = strip_comments("uint32_t AllocateCharacterId()\n{\n  return std::time(nullptr);\n}\n")
        self.assertTrue(find_timestamp_ids("x.cpp", code))

    def test_timestamp_field_is_allowed(self) -> None:
        code = strip_comments("void F()\n{\n  data.createdAt = GetTimestamp();\n  data.characterId = m_next++;\n  bool ok = sessionId == 3;\n}\n")
        self.assertEqual(find_timestamp_ids("x.cpp", code), [])

    def test_counter_allocator_is_allowed(self) -> None:
        code = strip_comments("uint32_t AllocateCharacterId()\n{\n  const uint32_t id = m_next++;\n  return id;\n}\n")
        self.assertEqual(find_timestamp_ids("x.cpp", code), [])

    def test_empty_load_world_is_rejected(self) -> None:
        for body in ("", "return true;", "(void)outData; return false;", "// nothing\nreturn true;"):
            code = strip_comments(f"bool P::LoadWorld(WorldSaveData& outData)\n{{\n{body}\n}}\n")
            self.assertTrue(find_empty_load_world("x.cpp", code), body)

    def test_real_load_world_is_allowed(self) -> None:
        code = strip_comments(
            "bool P::LoadWorld(WorldSaveData& outData)\n{\n  outData = WorldSaveData{};\n  if (!LoadGuilds(outData)) { return false; }\n  return true;\n}\n"
        )
        self.assertEqual(find_empty_load_world("x.cpp", code), [])


if __name__ == "__main__":
    unittest.main()
