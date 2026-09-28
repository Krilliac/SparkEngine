#!/usr/bin/env python3
"""EDT-210: every World/registry mutation in SparkEditor/Source is classified.

"All world mutations round-trip through undo/redo without divergence" is only
checkable if "all" is a closed list. This scanner finds every structural World
or registry mutation call in SparkEditor/Source (CreateEntity, DestroyEntity,
SetParent, AddComponent, RemoveComponent, registry create/destroy/emplace/
emplace_or_replace/get_or_emplace/replace/patch/remove/erase, and whole-registry
assignment), attributes each call to its enclosing function with a brace-depth
scan, and requires SparkEditor/world-mutation-inventory.json to list that
function with the exact number of sites and one class:

- "command": the call runs inside a CommandHistory command body or a
  SceneEditTools/EditorDocument commit. "case" names the Tests/ TEST that drives
  the entry point through undo and redo (EditorUndo_WorldMatrix_*).
- "fresh-world": the call builds a World that is not yet the document (new-scene
  and project-template seeding). "reason" says why.
- "non-document": the call writes something other than the document World.
  "reason" says what.

An unlisted site, a changed site count, a stale entry, a command entry whose case
has no TEST( in Tests/, or a non-command entry without a reason fails by name.
Mutation cases copy the real files into a temporary tree and inject each drift.

Registered as the CTest EditorUndo_WorldMutationInventory.
"""

from __future__ import annotations

import json
import re
import shutil
import sys
import tempfile
import unittest
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE_RELATIVE = Path("SparkEditor") / "Source"
INVENTORY_RELATIVE = Path("SparkEditor") / "world-mutation-inventory.json"
TESTS_RELATIVE = Path("Tests")
CLASSES = ("command", "fresh-world", "non-document")

MUTATION_CALL = re.compile(
    r"(?:\.|->)\s*(?:CreateEntity|DestroyEntity|SetParent)\s*\("
    r"|(?:\.|->)\s*(?:AddComponent|RemoveComponent)\s*[<(]"
    r"|(?:\.|->)\s*(?:emplace|emplace_or_replace|get_or_emplace|replace|patch|remove|erase)\s*<"
    r"|\b(?:reg|registry|m_registry)\s*(?:\.|->)\s*(?:create|destroy)\s*\("
    r"|GetRegistry\(\)\s*(?:\.\s*(?:create|destroy)\s*\(|=(?!=))"
)
NOT_A_FUNCTION = {"if", "for", "while", "switch", "catch", "do", "else", "return", "sizeof", "decltype"}
SCOPE_KEYWORD = re.compile(r"^(?:template\s*<[^{]*>\s*)?(?:namespace|class|struct|union|enum)\b\s*(?:class\s+)?(\w*)")
FUNCTION_NAME = re.compile(r"([A-Za-z_~][\w~]*(?:\s*::\s*~?[A-Za-z_]\w*)*|operator\s*\S+)\s*\($")


def strip_code(text: str) -> str:
    """Blank comments and string/char literals, keeping every newline and offset."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("".join("\n" if ch == "\n" else " " for ch in text[i:end]))
            i = end
            continue
        raw = re.match(r'R"([^(\s]*)\(', text[i:i + 20]) if c == "R" and (i == 0 or not text[i - 1].isalnum()) else None
        if raw:
            terminator = ")" + raw.group(1) + '"'
            end = text.find(terminator, i + raw.end())
            end = n if end < 0 else end + len(terminator)
            out.append("".join("\n" if ch == "\n" else " " for ch in text[i:end]))
            i = end
            continue
        if c in "\"'":
            if c == "'" and i > 0 and text[i - 1].isalnum():
                out.append(c)  # digit separator such as 1'000
                i += 1
                continue
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            end = min(j + 1, n)
            out.append(c + " " * (end - i - 2) + c if end - i >= 2 else " " * (end - i))
            i = end
            continue
        out.append(c)
        i += 1
    return "".join(out)


def function_scopes(code: str) -> list[tuple[int, int, str]]:
    """(start, end, qualified name) of every outermost function body in stripped code."""
    scopes: list[tuple[str, str]] = []  # (kind, name) per open brace
    results: list[tuple[int, int, str]] = []
    opened: list[int] = []
    header_start = 0
    paren = 0
    for index, ch in enumerate(code):
        if ch == "(":
            paren += 1
        elif ch == ")":
            paren = max(0, paren - 1)
        elif ch == ";" and paren == 0:
            header_start = index + 1
        elif ch == "{":
            inside_function = any(kind == "function" for kind, _ in scopes)
            header = " ".join(code[header_start:index].split())
            kind, name = "block", ""
            if not inside_function and paren == 0:
                keyword = SCOPE_KEYWORD.match(header)
                if keyword and "(" not in header:
                    kind, name = ("namespace" if header.startswith("namespace") else "class"), keyword.group(1)
                else:
                    kind, name = _function_header(header)
            scopes.append((kind, name))
            opened.append(index)
            header_start = index + 1
        elif ch == "}":
            if scopes:
                kind, name = scopes.pop()
                start = opened.pop()
                if kind == "function" and not any(k == "function" for k, _ in scopes):
                    classes = [n for k, n in scopes if k == "class" and n]
                    qualified = "::".join(classes + [name]) if classes and "::" not in name else name
                    results.append((start, index, qualified))
            header_start = index + 1
            paren = 0
    return results


def _function_header(header: str) -> tuple[str, str]:
    if not header or header.endswith(("=", ",")) or ")" not in header:
        return "block", ""
    # The name is the identifier before the first top-level '(' of the header.
    depth = 0
    for position, ch in enumerate(header):
        if ch == "<":
            depth += 1
        elif ch == ">" and depth:
            depth -= 1
        elif ch == "(" and depth == 0:
            match = FUNCTION_NAME.search(header[: position + 1])
            if not match:
                return "block", ""
            name = re.sub(r"\s+", "", match.group(1))
            if name.split("::")[-1] in NOT_A_FUNCTION:
                return "block", ""
            return "function", name
    return "block", ""


def scan_file(path: Path) -> Counter:
    code = strip_code(path.read_text(encoding="utf-8", errors="replace"))
    scopes = function_scopes(code)
    counts: Counter = Counter()
    for match in MUTATION_CALL.finditer(code):
        owner = next((name for start, end, name in scopes if start < match.start() < end), "<file scope>")
        counts[owner] += 1
    return counts


def scan_tree(root: Path) -> dict[tuple[str, str], int]:
    """Live mutation sites keyed by (repo-relative file, enclosing function)."""
    sites: dict[tuple[str, str], int] = {}
    source = root / SOURCE_RELATIVE
    for path in sorted(source.rglob("*")):
        if path.suffix not in (".cpp", ".h", ".hpp", ".inl") or not path.is_file():
            continue
        relative = path.relative_to(root).as_posix()
        for function, count in scan_file(path).items():
            sites[(relative, function)] = count
    return sites


def test_names(root: Path) -> set[str]:
    names: set[str] = set()
    for path in (root / TESTS_RELATIVE).rglob("*.cpp"):
        names.update(re.findall(r"\bTEST\(\s*(\w+)\s*\)", path.read_text(encoding="utf-8", errors="replace")))
    return names


def check(root: Path) -> list[str]:
    """Every problem with the inventory at root, as named messages (empty when clean)."""
    inventory = json.loads((root / INVENTORY_RELATIVE).read_text(encoding="utf-8"))
    problems: list[str] = []
    listed: dict[tuple[str, str], dict] = {}
    for entry in inventory["sites"]:
        key = (entry["file"], entry["function"])
        if key in listed:
            problems.append(f"duplicate entry: {key[0]} {key[1]}")
        listed[key] = entry
        klass = entry.get("class")
        if klass not in CLASSES:
            problems.append(f"bad class {klass!r}: {key[0]} {key[1]}")
        elif klass == "command" and not entry.get("case"):
            problems.append(f"command entry has no case: {key[0]} {key[1]}")
        elif klass != "command" and not str(entry.get("reason", "")).strip():
            problems.append(f"{klass} entry has no reason: {key[0]} {key[1]}")

    live = scan_tree(root)
    for key, count in sorted(live.items()):
        entry = listed.get(key)
        if entry is None:
            problems.append(
                f"unlisted mutation site: {key[0]} {key[1]} ({count} call(s)); add "
                + json.dumps({"file": key[0], "function": key[1], "sites": count, "class": "?"})
            )
        elif entry.get("sites") != count:
            problems.append(f"site count changed: {key[0]} {key[1]} lists {entry.get('sites')}, found {count}")
    for key in sorted(set(listed) - set(live)):
        problems.append(f"stale entry: {key[0]} {key[1]} has no mutation call")

    known_tests = test_names(root)
    for key, entry in sorted(listed.items()):
        case = entry.get("case")
        if entry.get("class") == "command" and case and case not in known_tests:
            problems.append(f"missing case: {key[0]} {key[1]} names {case}, which has no TEST( in Tests/")
    return problems


class LiveTreeTests(unittest.TestCase):
    def test_inventory_matches_live_tree(self) -> None:
        problems = check(ROOT)
        self.assertEqual(problems, [], "\n".join(problems))

    def test_every_class_is_used_and_command_cases_are_matrix_tests(self) -> None:
        inventory = json.loads((ROOT / INVENTORY_RELATIVE).read_text(encoding="utf-8"))
        classes = {entry["class"] for entry in inventory["sites"]}
        self.assertEqual(classes, set(CLASSES))
        for entry in inventory["sites"]:
            if entry["class"] == "command":
                self.assertTrue(entry["case"].startswith("EditorUndo_WorldMatrix_"), entry)


class ScannerTests(unittest.TestCase):
    def test_attributes_calls_to_the_outermost_function(self) -> None:
        code = strip_code(
            "namespace A {\n"
            "  // world.CreateEntity(\"comment\");\n"
            "  class Panel {\n"
            "    void Inline() { m_world->DestroyEntity(e); }\n"
            "  };\n"
            "  bool Doc::Create(int x = 1) {\n"
            "    auto undo = [this]() { reg.remove<T>(e); };\n"
            "    if (x) { world.AddComponent<T>(e); }\n"
            "    const char* s = \"reg.emplace<T>(e)\";\n"
            "    return true;\n"
            "  }\n"
            "}\n"
        )
        scopes = function_scopes(code)
        owners = Counter(
            next((name for start, end, name in scopes if start < m.start() < end), "<file scope>")
            for m in MUTATION_CALL.finditer(code)
        )
        self.assertEqual(owners, Counter({"Panel::Inline": 1, "Doc::Create": 2}))

    def test_declarations_and_value_calls_are_not_sites(self) -> None:
        code = strip_code(
            "bool CreateEntity(const std::string& name);\n"
            "void F() { std::remove(v.begin(), v.end(), x); v.erase(v.begin()); s.replace(0, 1, t); }\n"
        )
        self.assertEqual(list(MUTATION_CALL.finditer(code)), [])


class MutationTests(unittest.TestCase):
    """Copies of the real files, each with one injected drift, fail by name."""

    def setUp(self) -> None:
        temp = tempfile.TemporaryDirectory(prefix="world-mutation-inventory-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        inventory = json.loads((ROOT / INVENTORY_RELATIVE).read_text(encoding="utf-8"))
        files = {entry["file"] for entry in inventory["sites"]}
        files.add(INVENTORY_RELATIVE.as_posix())
        files.add("Tests/TestEditorUndoWorldMatrixReal.cpp")
        for relative in files:
            target = self.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / relative, target)
        (self.root / SOURCE_RELATIVE).mkdir(parents=True, exist_ok=True)

    def _edit(self, relative: str, old: str, new: str) -> None:
        path = self.root / relative
        text = path.read_text(encoding="utf-8")
        self.assertIn(old, text, f"mutation anchor missing from {relative}")
        path.write_text(text.replace(old, new, 1), encoding="utf-8")

    def _inventory(self) -> dict:
        return json.loads((self.root / INVENTORY_RELATIVE).read_text(encoding="utf-8"))

    def _write_inventory(self, inventory: dict) -> None:
        (self.root / INVENTORY_RELATIVE).write_text(json.dumps(inventory, indent=2), encoding="utf-8")

    def test_copied_tree_is_clean(self) -> None:
        self.assertEqual(check(self.root), [])

    def test_unlisted_raw_emplace_in_a_new_function_fails(self) -> None:
        path = self.root / SOURCE_RELATIVE / "Panels" / "RawEdit.cpp"
        path.write_text("void RawEdit(entt::registry& reg, entt::entity e) { reg.emplace<Tag>(e); }\n", encoding="utf-8")
        problems = check(self.root)
        self.assertTrue(any(p.startswith("unlisted mutation site: SparkEditor/Source/Panels/RawEdit.cpp RawEdit") for p in problems), problems)

    def test_extra_raw_add_component_in_a_listed_function_fails(self) -> None:
        self._edit(
            "SparkEditor/Source/Core/EditorDocument.cpp",
            "        m_world->DestroyEntity(m_selectedEntity);\n",
            "        m_world->DestroyEntity(m_selectedEntity);\n        m_world->AddComponent<::Transform>(m_selectedEntity);\n",
        )
        problems = check(self.root)
        self.assertTrue(any(p.startswith("site count changed: SparkEditor/Source/Core/EditorDocument.cpp EditorDocument::DeleteSelected") for p in problems), problems)

    def test_stale_entry_fails(self) -> None:
        (self.root / "SparkEditor/Source/Terrain/TerrainEditor.cpp").unlink()
        problems = check(self.root)
        self.assertTrue(any(p.startswith("stale entry: SparkEditor/Source/Terrain/TerrainEditor.cpp") for p in problems), problems)

    def test_missing_case_name_fails(self) -> None:
        self._edit(
            "Tests/TestEditorUndoWorldMatrixReal.cpp",
            "TEST(EditorUndo_WorldMatrix_EveryCommandEntryPointRoundTrips)",
            "TEST(EditorUndo_WorldMatrix_Renamed)",
        )
        problems = check(self.root)
        self.assertTrue(any(p.startswith("missing case:") and "EditorUndo_WorldMatrix_EveryCommandEntryPointRoundTrips" in p for p in problems), problems)

    def test_non_command_entry_without_reason_fails(self) -> None:
        inventory = self._inventory()
        entry = next(e for e in inventory["sites"] if e["class"] == "non-document")
        entry["reason"] = " "
        self._write_inventory(inventory)
        problems = check(self.root)
        self.assertIn(f"non-document entry has no reason: {entry['file']} {entry['function']}", problems)

    def test_command_entry_without_case_fails(self) -> None:
        inventory = self._inventory()
        entry = next(e for e in inventory["sites"] if e["class"] == "command")
        del entry["case"]
        self._write_inventory(inventory)
        problems = check(self.root)
        self.assertIn(f"command entry has no case: {entry['file']} {entry['function']}", problems)


def main(argv: list[str]) -> int:
    if "--print-live-sites" in argv:
        for (file, function), count in sorted(scan_tree(ROOT).items()):
            print(json.dumps({"file": file, "function": function, "sites": count}))
        return 0
    return 0 if unittest.main(argv=[argv[0]] + [a for a in argv[1:]], exit=False).result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
