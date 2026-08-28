#!/usr/bin/env python3
"""Generate the exact-commit SparkEngine website bundle and documentation corpus."""

from __future__ import annotations

import argparse
import json
import os
import posixpath
import re
import shutil
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path
from typing import Any, Iterable
from urllib.parse import unquote

from common import (
    REPOSITORY,
    REPOSITORY_URL,
    REPO_ROOT,
    SCHEMA_VERSION,
    SiteDataError,
    canonical_json_bytes,
    extract_excerpt,
    extract_headings,
    extract_title,
    git_dirty_paths,
    heading_slug,
    load_contract,
    plain_text,
    repository_source,
    sha256_bytes,
    slug_part,
    title_from_filename,
    to_posix,
    walk_files,
    write_bytes_atomic,
    write_json,
    write_text,
)
from render_handoff import render_handoff
from validate import validate_contract


SOURCE_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".m", ".mm"}
LARGE_DOCUMENT_BYTES = 240_000


def regenerate_api_docs(committed_at: str) -> None:
    """Rebuild ignored API reference pages from the checked-out source tree.

    ``docs/api`` is intentionally not tracked, so a clean CI checkout cannot
    rely on a developer's previously generated copy.  Treat the generator as
    part of publication and fail closed if it cannot produce the corpus.
    """

    script = REPO_ROOT / "docs" / "generate-api-docs.sh"
    if not script.is_file():
        raise SiteDataError("missing API documentation generator: docs/generate-api-docs.sh")

    environment = os.environ.copy()
    environment["SPARKENGINE_DOC_SOURCE_COMMITTED_AT"] = committed_at
    try:
        result = subprocess.run(
            ["bash", str(script), "generate"],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            timeout=240,
            check=False,
            env=environment,
        )
    except subprocess.TimeoutExpired as error:
        raise SiteDataError("API documentation generation exceeded 240 seconds") from error
    if result.returncode:
        detail = (result.stderr.strip() or result.stdout.strip())[-4000:]
        raise SiteDataError(f"API documentation generation failed: {detail}")

    api_root = REPO_ROOT / "docs" / "api"
    pages = list(api_root.rglob("*.md")) if api_root.is_dir() else []
    if not (api_root / "README.md").is_file() or len(pages) < 3:
        raise SiteDataError(
            "API documentation generator did not produce its index and reference pages"
        )
    manifest_path = api_root / ".generation.json"
    symbols_path = api_root / ".symbols.tsv"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SiteDataError("API documentation generation manifest is missing or invalid") from error

    source_roots = [
        REPO_ROOT / "SparkEngine" / "Source",
        REPO_ROOT / "SparkEditor" / "Source",
        REPO_ROOT / "SparkConsole" / "src",
        REPO_ROOT / "SparkShaderCompiler" / "src",
        REPO_ROOT / "SparkSDK",
        REPO_ROOT / "Tests",
        *sorted((REPO_ROOT / "GameModules").glob("*/Source")),
    ]
    expected_headers = sum(
        1
        for root in source_roots
        if root.is_dir()
        for path in root.rglob("*")
        if path.is_file() and path.suffix.lower() in {".h", ".hpp"}
    )
    expected_cpp = sum(
        1
        for root in source_roots
        if root.is_dir()
        for path in root.rglob("*.cpp")
        if path.is_file()
    )
    try:
        symbol_records = sum(1 for _ in symbols_path.open(encoding="utf-8", errors="replace"))
    except OSError as error:
        raise SiteDataError("API documentation symbol index is missing") from error
    expected_manifest = {
        "sourceCommittedAt": committed_at,
        "headersScanned": expected_headers,
        "cppFilesScanned": expected_cpp,
        "markdownPages": len(pages),
        "symbolRecords": symbol_records,
    }
    if manifest != expected_manifest:
        raise SiteDataError(
            "API documentation generation manifest does not match the complete source corpus: "
            f"expected {expected_manifest}, got {manifest}"
        )


def file_lines(paths: Iterable[Path]) -> tuple[int, int]:
    count = 0
    lines = 0
    for path in paths:
        if not path.is_file() or path.suffix.lower() not in SOURCE_EXTENSIONS:
            continue
        try:
            payload = path.read_bytes()
        except OSError:
            continue
        count += 1
        # Match the repository LOC workflow's wc -l semantics exactly.
        lines += payload.count(b"\n")
    return count, lines


def module_statistics() -> list[dict[str, Any]]:
    modules: list[dict[str, Any]] = []
    for module in sorted((REPO_ROOT / "GameModules").glob("SparkGame*")):
        if not module.is_dir() or not (module / "CMakeLists.txt").is_file():
            continue
        source_files, source_lines = file_lines((module / "Source").rglob("*") if (module / "Source").exists() else [])
        modules.append(
            {
                "name": module.name,
                "sourcePath": module.relative_to(REPO_ROOT).as_posix(),
                "files": source_files,
                "lines": source_lines,
                "hasReadme": (module / "README.md").is_file(),
            }
        )
    return modules


def metric(identifier: str, label: str, value: int | float | str, evidence: list[dict[str, Any]], unit: str | None = None) -> dict[str, Any]:
    result: dict[str, Any] = {
        "id": identifier,
        "label": label,
        "value": value,
        "display": f"{value:,}" if isinstance(value, int) else str(value),
        "evidence": evidence,
    }
    if unit:
        result["unit"] = unit
    return result


def collect_metrics(authored_documents: int, modules: list[dict[str, Any]]) -> list[dict[str, Any]]:
    canonical_code_roots = [
        "SparkEngine/Source",
        "SparkEditor/Source",
        "GameModules",
        "SparkConsole/src",
        "SparkShaderCompiler/src",
        "Tests",
    ]
    canonical_code_files: list[Path] = []
    for root_value in canonical_code_roots:
        root = REPO_ROOT / root_value
        if root.is_dir():
            canonical_code_files.extend(
                path for path in root.rglob("*")
                if path.is_file() and path.suffix.lower() in {".cpp", ".h", ".hpp"}
            )
    code_files, code_lines = file_lines(canonical_code_files)

    test_pattern = re.compile(r"^[ \t]*TEST(?:_F)?[ \t]*\(", flags=re.MULTILINE)
    test_definitions = 0
    test_sources = list(walk_files(REPO_ROOT / "Tests", {".cpp"}))
    for path in test_sources:
        content = path.read_text(encoding="utf-8", errors="ignore")
        test_definitions += len(test_pattern.findall(content))

    panel_path = REPO_ROOT / "SparkEditor" / "Source" / "Core" / "EditorPanelFactory.cpp"
    panel_content = panel_path.read_text(encoding="utf-8", errors="ignore")
    editor_panels = len(re.findall(r"\btryRegister\s*\(\s*\"", panel_content))

    palette_path = REPO_ROOT / "SparkEngine" / "Source" / "Engine" / "Scripting" / "VisualScriptCompiler.cpp"
    palette_content = palette_path.read_text(encoding="utf-8", errors="ignore")
    palette_match = re.search(r"\bkPalette\s*=\s*\{(?P<body>.*?)\};", palette_content, flags=re.DOTALL)
    visual_nodes = len(re.findall(r"\{\s*ScriptNodeType::", palette_match.group("body") if palette_match else ""))

    shader_root = REPO_ROOT / "Shaders"
    hlsl = sum(1 for path in shader_root.rglob("*") if path.is_file() and path.suffix.lower() == ".hlsl")
    glsl = sum(1 for path in shader_root.rglob("*") if path.is_file() and path.suffix.lower() == ".glsl")

    networking_files: list[Path] = []
    for root in (
        REPO_ROOT / "SparkEngine" / "Source" / "Engine" / "Networking",
        REPO_ROOT / "SparkEngine" / "Source" / "Engine" / "OnlineServices",
    ):
        if root.exists():
            networking_files.extend(root.rglob("*"))
    _, networking_lines = file_lines(networking_files)

    by_name = {module["name"]: module for module in modules}
    fps = by_name["SparkGameFPS"]
    mmofps = by_name["SparkGameMMOFPS"]
    tf_types_path = REPO_ROOT / "GameModules" / "SparkGameMMOFPS" / "Source" / "Core" / "TFTypes.h"
    tf_types = tf_types_path.read_text(encoding="utf-8", errors="strict")

    def exact_rate(name: str) -> int | float:
        values = re.findall(
            rf"\b{name}\s*=\s*([0-9]+(?:\.[0-9]+)?)f?\s*;",
            tf_types,
        )
        if len(values) != 1:
            raise SiteDataError(f"expected exactly one numeric {name} declaration in {tf_types_path.relative_to(REPO_ROOT)}")
        value = float(values[0])
        return int(value) if value.is_integer() else value

    server_hz = exact_rate("kServerTickHz")
    replication_hz = exact_rate("kReplicationHz")
    source_evidence = lambda path, label: [{"type": "source", "path": path, "label": label}]
    code_evidence = [
        {"type": "metric", "path": "tools/site-data/generate.py", "selector": "collect_metrics", "label": "Exact-commit LOC derivation"},
        *(
            {"type": "source", "path": path, "label": "Canonical LOC root"}
            for path in canonical_code_roots
        ),
    ]
    return [
        metric("code.totalLines", "C/C++ physical lines", code_lines, code_evidence, "lines"),
        metric("code.files", "C/C++ source files", code_files, code_evidence, "files"),
        metric("tests.definitions", "GoogleTest definitions", test_definitions, source_evidence("Tests", "TEST and TEST_F declarations"), "tests"),
        metric("tests.files", "Test C++ translation units", len(test_sources), source_evidence("Tests", "Recursive .cpp files in the test tree"), "files"),
        metric("editor.panels", "Registered editor panels", editor_panels, source_evidence("SparkEditor/Source/Core/EditorPanelFactory.cpp", "Editor panel registrations"), "panels"),
        metric("shaders.hlsl", "HLSL shader files", hlsl, source_evidence("Shaders/HLSL", "HLSL source tree"), "files"),
        metric("shaders.glsl", "GLSL shader files", glsl, source_evidence("Shaders/GLSL", "GLSL source tree"), "files"),
        metric("visualScript.nodes", "Visual-script palette nodes", visual_nodes, source_evidence("SparkEngine/Source/Engine/Scripting/VisualScriptCompiler.cpp", "Node palette"), "nodes"),
        metric("networking.lines", "Networking and online-services lines", networking_lines, source_evidence("SparkEngine/Source/Engine/Networking", "Networking source tree"), "lines"),
        metric("modules.discovered", "CMake-discovered game modules", len(modules), source_evidence("GameModules", "Module directories with CMakeLists.txt"), "modules"),
        metric("module.fps.files", "FPS module source files", fps["files"], source_evidence(fps["sourcePath"], "FPS module source"), "files"),
        metric("module.fps.lines", "FPS module physical lines", fps["lines"], source_evidence(fps["sourcePath"], "FPS module source"), "lines"),
        metric("module.mmofps.files", "MMOFPS module source files", mmofps["files"], source_evidence(mmofps["sourcePath"], "MMOFPS module source"), "files"),
        metric("module.mmofps.lines", "MMOFPS module physical lines", mmofps["lines"], source_evidence(mmofps["sourcePath"], "MMOFPS module source"), "lines"),
        metric("module.mmofps.serverHz", "MMOFPS authoritative simulation rate", server_hz, source_evidence("GameModules/SparkGameMMOFPS/Source/Core/TFTypes.h", "kServerTickHz declaration"), "Hz"),
        metric("module.mmofps.replicationHz", "MMOFPS state replication ceiling", replication_hz, source_evidence("GameModules/SparkGameMMOFPS/Source/Core/TFTypes.h", "kReplicationHz declaration"), "Hz"),
        metric("docs.authored", "Authored documentation pages", authored_documents, source_evidence("docs/site/docs-catalog.json", "Documentation inclusion contract"), "documents"),
    ]


def slug_for_source(source_path: str, catalog: dict[str, Any]) -> str:
    override = catalog.get("routeOverrides", {}).get(source_path)
    if override:
        return override
    parts = source_path.split("/")
    filename = parts[-1]
    if source_path.startswith("wiki/"):
        return "/".join(slug_part(part) for part in parts[1:])
    if source_path.startswith("docs/api/"):
        return "/".join(["api-reference", "headers", *(slug_part(part) for part in parts[2:])])
    if source_path.startswith("docs/"):
        docs_parts = parts[1:]
        if docs_parts[0] == "specs":
            return "/".join(["specifications", "contracts", *(slug_part(part) for part in docs_parts[1:])])
        prefix = {
            "architecture": "architecture",
            "guides": "tools-workflows",
            "plans": "roadmap-plans",
            "status": "project-status",
            "tooling": "toolchain",
            "wine-upstream": "platforms",
            "readiness": "project-status",
        }.get(docs_parts[0], "advanced-engineering")
        return "/".join([prefix, *(slug_part(part) for part in docs_parts[1:])])
    if source_path.startswith(("GameModules/", "Templates/")):
        catalog_name = "catalog" if source_path.startswith("GameModules/") else "templates"
        entry = slug_part(parts[1]) if len(parts) > 2 else catalog_name
        route = ["game-modules", entry]
        # A nested README describes its directory, not the module/template
        # root. Without the directory segments, e.g. Assets/README.md and the
        # package README collide on the same published slug.
        route.extend(slug_part(part) for part in parts[2:-1])
        document = slug_part(filename)
        if document != "readme":
            route.append(document)
        return "/".join(route)
    if len(parts) == 1:
        return f"project/{slug_part(filename)}"
    nested = [slug_part(part) for part in parts[1:-1]]
    document = slug_part(filename)
    route = ["toolchain", slug_part(parts[0]), *nested]
    if document != "readme":
        route.append(document)
    return "/".join(route)


def classify(source_path: str, catalog: dict[str, Any]) -> tuple[str, str, str]:
    for rule in catalog.get("classificationRules", []):
        if source_path.startswith(rule["prefix"]):
            provenance = rule.get("provenance", "derived" if rule.get("kind") == "generated" else "authored")
            return rule["section"], rule["kind"], provenance
    if source_path == "wiki/Home.md":
        return "getting-started", "guide", "authored"
    if source_path.startswith("wiki/advanced/"):
        if re.search(r"Status|Audit|Stub|Health|Observations", source_path, flags=re.IGNORECASE):
            return "project-status", "status", "authored"
        if re.search(r"Plan", source_path, flags=re.IGNORECASE):
            return "roadmap-plans", "proposal", "authored"
        return "advanced-engineering", "guide", "authored"
    if source_path.startswith(("GameModules/", "Templates/")):
        return "game-modules", "reference", "authored"
    if source_path.startswith("Assets/"):
        return "tools-workflows", "reference", "authored"
    if "/README.md" in source_path or source_path.startswith(("Spark", "Tools/", "tools/", "cmake/", "Tests/")):
        return "toolchain", "reference", "authored"
    kind = "changelog" if "CHANGELOG" in source_path.upper() else "policy"
    return "project-governance", kind, "authored"


def collect_document_sources(catalog: dict[str, Any]) -> list[Path]:
    include = catalog["include"]
    candidates: set[Path] = set()
    for value in include.get("rootDocuments", []):
        path = REPO_ROOT / value
        if path.is_file():
            candidates.add(path)
    for value in include.get("recursiveMarkdownRoots", []):
        root = REPO_ROOT / value
        if root.is_file() and root.suffix.lower() == ".md":
            candidates.add(root)
        elif root.is_dir():
            candidates.update(path for path in root.rglob("*.md") if path.is_file())

    excluded_paths = set(catalog.get("excludePaths", []))
    excluded_prefixes = tuple(catalog.get("excludePrefixes", []))
    result = []
    for path in candidates:
        source_path = path.relative_to(REPO_ROOT).as_posix()
        if source_path in excluded_paths or source_path.startswith(excluded_prefixes):
            continue
        result.append(path)
    return sorted(result)


def normalize_html(markdown: str) -> str:
    value = re.sub(r"<details[^>]*>|</details>", "", markdown, flags=re.IGNORECASE)
    value = re.sub(r"<summary[^>]*>(.*?)</summary>", r"**\1**", value, flags=re.IGNORECASE | re.DOTALL)
    value = re.sub(r"\s*<sup>(.*?)</sup>", r" · \1", value, flags=re.IGNORECASE | re.DOTALL)
    value = re.sub(r"<br\s*/?>", "\n", value, flags=re.IGNORECASE)
    return re.sub(r"<kbd>(.*?)</kbd>", r"`\1`", value, flags=re.IGNORECASE | re.DOTALL)


def split_large_document(document: dict[str, Any]) -> list[dict[str, Any]]:
    if len(document["content"].encode("utf-8")) < LARGE_DOCUMENT_BYTES:
        return [document]
    pieces = re.split(r"(?=^##\s+)", document["content"], flags=re.MULTILINE)
    if len(pieces) < 3:
        lines = document["content"].splitlines()
        table_index = next(
            (
                index
                for index, line in enumerate(lines[:-1])
                if re.match(r"^\|\s*(?:Symbol|Class|Enum|Macro|Function|File)\s*\|", line, flags=re.IGNORECASE)
                and re.match(r"^\|[-:|\s]+\|?$", lines[index + 1])
            ),
            -1,
        )
        if table_index < 0:
            return [document]
        table_header = "\n".join(lines[table_index : table_index + 2])
        rows = [line for line in lines[table_index + 2 :] if line.startswith("|")]
        if len(rows) < 500:
            return [document]
        grouped: dict[str, list[str]] = {}
        for row in rows:
            first_cell = row.split("|", 2)[1] if "|" in row else ""
            match = re.search(r"[a-z0-9]", re.sub(r"[`*_~]", "", first_cell), flags=re.IGNORECASE)
            group = match.group(0).upper() if match else "#"
            grouped.setdefault(group, []).append(row)
        children: list[dict[str, Any]] = []
        maximum_rows_bytes = 1_500_000
        for group, group_rows in sorted(grouped.items()):
            chunks: list[list[str]] = []
            current: list[str] = []
            current_bytes = 0
            for row in group_rows:
                row_bytes = len(row.encode("utf-8")) + 1
                if current and current_bytes + row_bytes > maximum_rows_bytes:
                    chunks.append(current)
                    current = []
                    current_bytes = 0
                current.append(row)
                current_bytes += row_bytes
            if current:
                chunks.append(current)
            for chunk_index, chunk in enumerate(chunks, start=1):
                label = group if len(chunks) == 1 else f"{group} {chunk_index}"
                suffix = "symbols" if group == "#" else group.lower()
                if len(chunks) > 1:
                    suffix = f"{suffix}-{chunk_index}"
                child = dict(document)
                child.update(
                    {
                        "slug": f"{document['slug']}/{suffix}",
                        "title": f"{document['title']} — {label}",
                        "content": f"# {document['title']}: {label}\n\n{table_header}\n" + "\n".join(chunk) + "\n",
                        "parentSlug": document["slug"],
                    }
                )
                children.append(child)
        intro = "\n".join(lines[:table_index]).rstrip()
        links = "\n".join(
            f"- [{child['title'].removeprefix(document['title'] + ' — ')}](/docs/{child['slug']})"
            for child in children
        )
        parent = dict(document)
        parent["content"] = (
            f"{intro}\n\n## Browse this large reference\n\n"
            "This source index is divided alphabetically into exact-commit pages so browsers do not render thousands of rows at once.\n\n"
            f"{links}\n"
        )
        parent["splitChildren"] = [child["slug"] for child in children]
        return [parent, *children]
    intro = pieces[0]
    children: list[dict[str, Any]] = []
    used: Counter[str] = Counter()
    for index, piece in enumerate(pieces[1:], start=1):
        match = re.search(r"^##\s+(.+?)\s*$", piece, flags=re.MULTILINE)
        heading = re.sub(r"[*_`]", "", match.group(1)).strip() if match else f"Section {index}"
        base = heading_slug(heading)
        occurrence = used[base]
        used[base] += 1
        suffix = base if occurrence == 0 else f"{base}-{occurrence}"
        child = dict(document)
        child.update(
            {
                "slug": f"{document['slug']}/{suffix}",
                "title": f"{document['title']} — {heading}",
                "content": f"# {document['title']}: {heading}\n\n" + re.sub(r"^##\s+.+?$", "", piece, count=1, flags=re.MULTILINE),
                "parentSlug": document["slug"],
            }
        )
        children.append(child)
    links = "\n".join(f"- [{child['title'].removeprefix(document['title'] + ' — ')}](/docs/{child['slug']})" for child in children)
    parent = dict(document)
    parent["content"] = (
        f"{intro.rstrip()}\n\n## Browse this large reference\n\n"
        "This source is divided into smaller exact-commit pages so browsers do not render the whole reference at once.\n\n"
        f"{links}\n"
    )
    parent["splitChildren"] = [child["slug"] for child in children]
    return [parent, *children]


def documentation_health() -> dict[str, Any]:
    script_relative = Path("docs") / "update-all-docs.sh"
    if not (REPO_ROOT / script_relative).is_file():
        return {"status": "unknown", "checks": []}
    with tempfile.TemporaryDirectory(prefix="sparkengine-doc-health-") as temporary:
        checkout = Path(temporary) / "checkout"
        add = subprocess.run(
            ["git", "worktree", "add", "--detach", str(checkout), "HEAD"],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        if add.returncode:
            detail = add.stderr.strip() or add.stdout.strip()
            return {"status": "unknown", "checks": [{"name": "Documentation generators", "status": "refresh-pending", "detail": f"Isolated check unavailable: {detail}"}]}
        try:
            try:
                result = subprocess.run(
                    ["bash", str(checkout / script_relative), "check"],
                    cwd=checkout,
                    capture_output=True,
                    text=True,
                    timeout=90,
                    check=False,
                )
            except subprocess.TimeoutExpired:
                return {"status": "unknown", "checks": [{"name": "Documentation generators", "status": "refresh-pending", "detail": "Health check exceeded 90 seconds"}]}
        finally:
            subprocess.run(
                ["git", "worktree", "remove", "--force", str(checkout)],
                cwd=REPO_ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
    output = f"{result.stdout}\n{result.stderr}"
    output = re.sub(r"\x1b\[[0-9;]*m", "", output)
    checks = []
    for match in re.finditer(r"\[CHECK\]\s+(.+?)\.\.\.\s+([✓✗])\s+([^\n]+)", output):
        checks.append(
            {
                "name": match.group(1).strip(),
                "status": "current" if match.group(2) == "✓" else "refresh-pending",
                "detail": match.group(3).strip(),
            }
        )
    status = "current" if result.returncode == 0 else ("refresh-pending" if checks else "unknown")
    return {"status": status, "checks": checks}


def build_documents(contract: dict[str, Any], source: dict[str, str], *, check_health: bool) -> tuple[list[dict[str, Any]], list[dict[str, Any]], dict[str, Any], int]:
    catalog = contract["docsCatalog"]
    authored: list[dict[str, Any]] = []
    for path in collect_document_sources(catalog):
        source_path = path.relative_to(REPO_ROOT).as_posix()
        content = path.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")
        section, kind, provenance = classify(source_path, catalog)
        authored.append(
            {
                "sourcePath": source_path,
                "slug": slug_for_source(source_path, catalog),
                "title": extract_title(content, source_path),
                "section": section,
                "kind": kind,
                "provenance": provenance,
                "sourceUrl": f"{REPOSITORY_URL}/blob/{source['commit']}/{source_path}",
                "content": content,
            }
        )

    source_to_slug = {document["sourcePath"]: document["slug"] for document in authored}
    commit = source["commit"]

    def rewrite_target(target: str, source_path: str, is_image: bool) -> str:
        if not target or target.startswith("#") or re.match(r"^(?:https?:|mailto:|tel:|data:)", target, flags=re.IGNORECASE):
            return target
        cleaned = target.strip("<>")
        pathname, marker, anchor = cleaned.partition("#")
        try:
            pathname = unquote(pathname)
        except ValueError:
            return target
        if not pathname or "\\" in pathname or pathname.startswith("/"):
            return target
        resolved = posixpath.normpath(posixpath.join(posixpath.dirname(source_path), pathname))
        if resolved == ".." or resolved.startswith("../"):
            return target
        suffix = f"#{anchor}" if marker else ""
        if not is_image and resolved in source_to_slug:
            return f"/docs/{source_to_slug[resolved]}{suffix}"
        candidate = REPO_ROOT / resolved
        if is_image and candidate.is_file():
            return f"https://raw.githubusercontent.com/{REPOSITORY}/{commit}/{resolved}{suffix}"
        if candidate.is_dir():
            return f"{REPOSITORY_URL}/tree/{commit}/{resolved}{suffix}"
        if candidate.exists():
            return f"{REPOSITORY_URL}/blob/{commit}/{resolved}{suffix}"
        return target

    rewritten: list[dict[str, Any]] = []
    pattern = re.compile(r"(!?\[[^\]]*\]\()([^\s)]+)([^)]*\))")
    for document in authored:
        content = normalize_html(document["content"])
        for branch in ("master", "main", "Working"):
            content = content.replace(f"{REPOSITORY_URL}/blob/{branch}/", f"{REPOSITORY_URL}/blob/{commit}/")
            content = content.replace(f"{REPOSITORY_URL}/tree/{branch}/", f"{REPOSITORY_URL}/tree/{commit}/")
            content = content.replace(f"https://raw.githubusercontent.com/{REPOSITORY}/{branch}/", f"https://raw.githubusercontent.com/{REPOSITORY}/{commit}/")

        def replace_link(match: re.Match[str]) -> str:
            prefix, target, suffix = match.groups()
            replacement = rewrite_target(target, document["sourcePath"], prefix.startswith("!"))
            return f"{prefix}{replacement}{suffix}"

        copy = dict(document)
        copy["content"] = pattern.sub(replace_link, content)
        rewritten.extend(split_large_document(copy))

    seen: dict[str, str] = {}
    for document in rewritten:
        if document["slug"] in seen:
            raise SiteDataError(
                f"duplicate documentation slug {document['slug']!r}: {seen[document['slug']]} and {document['sourcePath']}"
            )
        seen[document["slug"]] = document["sourcePath"]

    section_order = {section["id"]: index for index, section in enumerate(catalog["sections"])}
    rewritten.sort(key=lambda document: (section_order.get(document["section"], 999), document["title"].casefold(), document["slug"]))
    sections = []
    for section in catalog["sections"]:
        count = sum(1 for document in rewritten if document["section"] == section["id"])
        if count:
            sections.append({**section, "count": count})

    modules = module_statistics()
    sdk_headers = sum(1 for path in (REPO_ROOT / "SparkSDK").rglob("*") if path.is_file() and path.suffix.lower() in {".h", ".hpp"})
    authored_count = sum(1 for document in rewritten if document["provenance"] == "authored" and not document.get("parentSlug"))
    snapshot = {
        **source,
        "generatedAt": source["committedAt"],
        "health": documentation_health() if check_health else {"status": "unknown", "checks": []},
        "counts": {
            "documents": len(rewritten),
            "authoredDocuments": authored_count,
            "derivedDocuments": sum(1 for document in rewritten if document["provenance"] == "derived"),
            "splitReferenceSections": sum(1 for document in rewritten if document.get("parentSlug")),
            "wikiPages": sum(1 for document in authored if document["sourcePath"].startswith("wiki/")),
            "modules": len(modules),
            "sdkHeaders": sdk_headers,
            "localAssets": 0,
        },
    }
    return rewritten, sections, snapshot, authored_count


def metadata_for(document: dict[str, Any]) -> dict[str, Any]:
    content = document["content"]
    words = len(plain_text(content).split())
    return {
        "slug": document["slug"],
        "title": document["title"],
        "excerpt": extract_excerpt(content),
        "section": document["section"],
        "kind": document["kind"],
        "provenance": document["provenance"],
        "sourcePath": document["sourcePath"],
        "sourceUrl": document["sourceUrl"],
        "parentSlug": document.get("parentSlug"),
        "headings": extract_headings(content),
        "wordCount": words,
        "readingMinutes": max(1, (words + 219) // 220),
    }


def ensure_safe_output(output: Path, *, preserve_existing: bool) -> Path:
    resolved = output.resolve()
    forbidden = {Path("/").resolve(), Path.home().resolve(), REPO_ROOT.resolve(), REPO_ROOT.parent.resolve()}
    if resolved in forbidden:
        raise SiteDataError(f"refusing unsafe output directory: {resolved}")
    if not preserve_existing and resolved.exists():
        sentinel = resolved / ".sparkengine-site-data-output"
        narrow_name = re.fullmatch(r"\.site-data(?:[-._][a-zA-Z0-9_-]+)?", resolved.name)
        if not sentinel.is_file() and not narrow_name:
            raise SiteDataError(
                f"refusing to replace non-site-data directory without generator sentinel: {resolved}"
            )
        shutil.rmtree(resolved)
    resolved.mkdir(parents=True, exist_ok=True)
    write_text(resolved / ".sparkengine-site-data-output", "SparkEngine repository site-data output\n")
    return resolved


def pointer(output: Path, path: Path, info: dict[str, Any]) -> dict[str, Any]:
    return {**info, "path": path.relative_to(output).as_posix()}


def prune_snapshots(output: Path, current_commit: str, retain: int) -> None:
    snapshots = output / "snapshots"
    if not snapshots.is_dir():
        return
    records: list[tuple[str, str, Path]] = []
    for directory in snapshots.iterdir():
        bundle = directory / "bundle.json"
        if not directory.is_dir() or not bundle.is_file():
            continue
        try:
            data = json.loads(bundle.read_text(encoding="utf-8"))
            records.append((data.get("generatedAt", ""), directory.name, directory))
        except (OSError, json.JSONDecodeError):
            records.append(("", directory.name, directory))
    records.sort(reverse=True)
    keep = {current_commit}
    previous = (name for _, name, _ in records if name != current_commit)
    for _ in range(max(0, retain - 1)):
        try:
            keep.add(next(previous))
        except StopIteration:
            break
    for _, name, directory in records:
        if name not in keep:
            shutil.rmtree(directory)


def enforce_publication_budgets(output: Path, bundle_path: Path, page_root: Path) -> None:
    budgets = {
        "latest.json": (output / "latest.json", 32 * 1024),
        "bundle metadata": (bundle_path, 5 * 1024 * 1024),
    }
    for label, (path, maximum) in budgets.items():
        size = path.stat().st_size
        if size > maximum:
            raise SiteDataError(f"{label} is {size} bytes; publication budget is {maximum} bytes")
    oversize = sorted(
        (path.stat().st_size, path)
        for path in page_root.glob("*.json")
        if path.stat().st_size > 2 * 1024 * 1024
    )
    if oversize:
        size, path = oversize[-1]
        raise SiteDataError(
            f"documentation page {path.name} is {size} bytes; publication budget is {2 * 1024 * 1024} bytes"
        )


def generate(args: argparse.Namespace) -> dict[str, Any]:
    contract = validate_contract()
    source = repository_source(
        branch_override=args.source_branch,
        commit_override=args.source_commit,
        committed_at_override=args.committed_at,
    )
    checked_out_commit = repository_source()["commit"]
    if source["commit"] != checked_out_commit:
        raise SiteDataError(
            f"source commit {source['commit']} does not match checked-out files at {checked_out_commit}"
        )
    dirty = git_dirty_paths()
    if dirty and not args.allow_dirty:
        preview = ", ".join(dirty[:8]) + ("…" if len(dirty) > 8 else "")
        raise SiteDataError(f"repository is dirty; exact-commit publication refused ({preview})")
    if args.evidence_commit and args.evidence_commit != source["commit"]:
        raise SiteDataError(
            f"evidence commit {args.evidence_commit} does not match checked-out commit {source['commit']}"
        )

    regenerate_api_docs(source["committedAt"])

    output = ensure_safe_output(args.output, preserve_existing=args.preserve_existing)
    snapshot_root = output / "snapshots" / source["commit"]
    if snapshot_root.exists():
        shutil.rmtree(snapshot_root)
    docs_root = snapshot_root / "docs"
    page_root = docs_root / "pages"
    page_root.mkdir(parents=True, exist_ok=True)

    documents, sections, docs_snapshot, authored_count = build_documents(
        contract, source, check_health=not args.skip_doc_health
    )
    metadata: list[dict[str, Any]] = []
    search_records: list[dict[str, Any]] = []
    files_by_slug: dict[str, dict[str, Any]] = {}
    for document in documents:
        base_meta = metadata_for(document)
        page = {
            "schemaVersion": SCHEMA_VERSION,
            "bundleVersion": source["commit"],
            "sourceCommit": source["commit"],
            "document": base_meta,
            "content": document["content"],
        }
        page_bytes = canonical_json_bytes(page)
        page_hash = sha256_bytes(page_bytes)
        page_path = page_root / f"{page_hash}.json"
        write_bytes_atomic(page_path, page_bytes)
        published = {
            "path": page_path.relative_to(output).as_posix(),
            "sha256": page_hash,
            "bytes": len(page_bytes),
        }
        files_by_slug[document["slug"]] = published
        metadata.append(
            {
                **base_meta,
                "contentPath": published["path"],
                "contentSha256": published["sha256"],
                "contentBytes": published["bytes"],
                "published": published,
            }
        )
        search_records.append(
            {
                "slug": base_meta["slug"],
                "title": base_meta["title"],
                "excerpt": base_meta["excerpt"],
                "section": base_meta["section"],
                "kind": base_meta["kind"],
                "sourcePath": base_meta["sourcePath"],
                "headings": [heading["label"] for heading in base_meta["headings"]],
                "searchText": plain_text(document["content"])[:4000],
            }
        )

    search_path = docs_root / "search.json"
    search_info = pointer(
        output,
        search_path,
        write_json(
            search_path,
            {
                "schemaVersion": SCHEMA_VERSION,
                "bundleVersion": source["commit"],
                "sourceCommit": source["commit"],
                "records": search_records,
            },
        ),
    )
    docs_payload = {
        "snapshot": docs_snapshot,
        "sections": sections,
        "documents": metadata,
        "searchPath": search_info["path"],
        "searchSha256": search_info["sha256"],
        "searchBytes": search_info["bytes"],
        "filesBySlug": files_by_slug,
    }
    docs_index_path = docs_root / "index.json"
    docs_index_info = pointer(output, docs_index_path, write_json(docs_index_path, {"schemaVersion": SCHEMA_VERSION, **docs_payload}))

    modules = module_statistics()
    metrics = collect_metrics(authored_count, modules)
    readiness = contract["readiness"]
    evidence_commit = args.evidence_commit or source["commit"]
    conclusion = args.ci_conclusion or ("dirty-working-tree" if dirty else "success")
    publication_state = "current" if conclusion == "success" and not dirty else "blocked"
    publication = {
        "state": publication_state,
        "evidenceCommit": evidence_commit,
        "workflowUrl": args.workflow_url,
        "conclusion": conclusion,
    }
    bundle = {
        "schemaVersion": SCHEMA_VERSION,
        "bundleVersion": source["commit"],
        "generatedAt": source["committedAt"],
        "source": source,
        "publication": publication,
        "globalRelease": readiness["globalRelease"],
        "statusPromotionRules": readiness["statusPromotionRules"],
        "metrics": metrics,
        "capabilities": readiness["capabilities"],
        "gates": readiness["gates"],
        "releaseProfiles": readiness["releaseProfiles"],
        "workItems": contract["workItems"],
        "execution": readiness["execution"],
        "parityDimensions": contract.get("parityDimensions"),
        "site": {key: value for key, value in contract["content"].items() if key != "schemaVersion"},
        "docs": docs_payload,
    }

    split_files: dict[str, dict[str, Any]] = {"docsIndex": docs_index_info, "docsSearch": search_info}
    split_values = {
        "site": bundle["site"],
        "readiness": {
            "schemaVersion": SCHEMA_VERSION,
            "globalRelease": bundle["globalRelease"],
            "statusPromotionRules": bundle["statusPromotionRules"],
            "capabilities": bundle["capabilities"],
            "gates": bundle["gates"],
            "releaseProfiles": bundle["releaseProfiles"],
            "execution": bundle["execution"],
        },
        "metrics": {"schemaVersion": SCHEMA_VERSION, "metrics": metrics},
        "handoff": {
            "schemaVersion": SCHEMA_VERSION,
            "workItems": bundle["workItems"],
            "execution": bundle["execution"],
            "parityDimensions": bundle["parityDimensions"],
        },
    }
    for name, value in split_values.items():
        path = snapshot_root / f"{name}.json"
        split_files[name] = pointer(output, path, write_json(path, value))

    handoff_path = snapshot_root / "handoff.md"
    handoff_text = render_handoff(contract)
    published_stamp = (
        f"\n> Published snapshot: branch `{source['branch']}` at "
        f"[commit `{source['shortCommit']}`]({REPOSITORY_URL}/commit/{source['commit']}) "
        f"(committed {source['committedAt']}).\n"
    )
    first_heading = handoff_text.find("\n# ")
    handoff_text = (
        handoff_text[:first_heading]
        + published_stamp
        + handoff_text[first_heading:]
        if first_heading >= 0
        else published_stamp.lstrip() + handoff_text
    )
    write_text(handoff_path, handoff_text)
    handoff_bytes = handoff_text.encode("utf-8")
    split_files["handoffMarkdown"] = {
        "path": handoff_path.relative_to(output).as_posix(),
        "sha256": sha256_bytes(handoff_bytes),
        "bytes": len(handoff_bytes),
    }

    bundle_path = snapshot_root / "bundle.json"
    bundle_info = pointer(output, bundle_path, write_json(bundle_path, bundle))
    latest = {
        "schemaVersion": SCHEMA_VERSION,
        "bundleVersion": source["commit"],
        "source": source,
        "generatedAt": source["committedAt"],
        "publication": publication,
        "files": {"bundle": bundle_info, **split_files},
    }
    write_json(output / "latest.json", latest, pretty=True)
    write_text(output / ".nojekyll", "")
    write_text(
        output / "README.md",
        "# SparkEngine site data\n\nGenerated exact-commit website data. Do not edit this branch by hand; update the repository contracts and let the publication workflow rebuild it.\n",
    )
    enforce_publication_budgets(output, bundle_path, page_root)
    prune_snapshots(output, source["commit"], max(1, args.retain))
    return {"latest": latest, "documents": len(metadata), "output": output}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=REPO_ROOT / ".site-data")
    parser.add_argument("--allow-dirty", action="store_true", help="generate a blocked local bundle from a dirty tree")
    parser.add_argument("--preserve-existing", action="store_true", help="keep existing snapshots before applying retention")
    parser.add_argument("--retain", type=int, default=3, help="number of commit snapshots to retain")
    parser.add_argument("--source-branch", "--branch", dest="source_branch", help="branch label for detached CI checkouts")
    parser.add_argument("--source-commit", help="commit identity; must resolve to the checked-out HEAD")
    parser.add_argument("--committed-at", help="ISO-8601 commit timestamp supplied by the publishing workflow")
    parser.add_argument("--evidence-commit")
    parser.add_argument("--workflow-url")
    parser.add_argument("--ci-conclusion")
    parser.add_argument("--skip-doc-health", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        result = generate(args)
    except (SiteDataError, OSError, ValueError, KeyError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    latest = result["latest"]
    print(
        f"Generated {result['documents']} documentation routes and site bundle "
        f"for {latest['source']['shortCommit']} at {result['output']}."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
