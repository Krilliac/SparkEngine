#!/usr/bin/env python3
"""Generate the exact-commit SparkEngine website bundle and documentation corpus."""

from __future__ import annotations

import argparse
import importlib.util
import os
import posixpath
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ElementTree
from collections import Counter
from pathlib import Path, PurePosixPath
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
    load_json,
    plain_text,
    read_bytes_stable,
    repository_source,
    sha256_bytes,
    slug_part,
    title_from_filename,
    to_posix,
    tracked_files,
    write_bytes_atomic,
    write_json,
    write_text,
)
from render_handoff import render_handoff
from validate import validate_contract, warn_legacy_contract_flag_deprecated
from exact_evidence import (
    ExactEvidenceError,
    load_manifest as load_exact_evidence_manifest,
    validate_manifest as validate_exact_evidence_manifest,
)


TOOLS_ROOT = Path(__file__).resolve().parents[1]
if str(TOOLS_ROOT) not in sys.path:
    sys.path.insert(0, str(TOOLS_ROOT))
import docs_contract  # noqa: E402


SOURCE_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".m", ".mm"}
LARGE_DOCUMENT_BYTES = 240_000
API_GENERATION_TIMEOUT_SECONDS = 240
# Nine declared generators, each bounded to 300 s by docs/update-all-docs.sh.
DOC_HEALTH_TIMEOUT_SECONDS = 1800
MAX_HEALTH_JSON_BYTES = 4 * 1024 * 1024
MAX_CTEST_REPORT_BYTES = 32 * 1024 * 1024
CODEBASE_METRICS_PATH = REPO_ROOT / "docs" / "codebase-metrics.py"
MAX_OUTPUT_FILES = 5000
MAX_OUTPUT_BYTES = 256 * 1024 * 1024
DOC_OUTPUT_ENVIRONMENT = (
    "SPARK_DOC_API_OUTPUT_DIR",
    "SPARK_DOC_API_DIR",
    "SPARK_SYMBOL_INDEX_OUTPUT_DIR",
    "SPARK_FILE_TREE_OUTPUT",
    "SPARK_CLASS_HIERARCHY_OUTPUT",
    "SPARK_WIKI_DIR",
    "SPARK_DOC_HEALTH_OUTPUT",
    "SPARK_DOC_HEALTH_INNER",
)


def run_bounded_process(
    command: list[str], *, cwd: Path, environment: dict[str, str], timeout: int, label: str
) -> subprocess.CompletedProcess[str]:
    """Translate shared process-bound failures into site-data diagnostics."""

    try:
        return docs_contract.run_bounded_process(
            command,
            cwd=cwd,
            environment=environment,
            timeout=timeout,
            label=label,
        )
    except docs_contract.ContractError as error:
        raise SiteDataError(str(error)) from error


def api_generation_environment(source_commit: str, committed_at: str, api_root: Path) -> dict[str, str]:
    """Build a closed environment for the API producer's sole authorized output."""

    environment = os.environ.copy()
    for key in DOC_OUTPUT_ENVIRONMENT:
        environment.pop(key, None)
    environment.update(
        {
            "SPARK_DOC_API_OUTPUT_DIR": str(api_root),
            "SPARKENGINE_DOC_SOURCE_SHA": source_commit,
            "SPARKENGINE_DOC_SOURCE_COMMITTED_AT": committed_at,
        }
    )
    return environment


def trusted_bash() -> str:
    """Locate the host shell without inheriting an attacker-controlled override."""

    candidate = shutil.which("bash")
    if candidate and Path(candidate).is_file():
        return candidate
    bundled = Path(r"C:\\Program Files\\Git\\bin\\bash.exe")
    if bundled.is_file():
        return str(bundled)
    raise SiteDataError("bash is required for API documentation generation")


def regenerate_api_docs(source_commit: str, committed_at: str) -> None:
    """Rebuild ignored API reference pages from the checked-out source tree.

    ``docs/api`` is intentionally not tracked, so a clean CI checkout cannot
    rely on a developer's previously generated copy.  Treat the generator as
    part of publication and fail closed if it cannot produce the corpus.
    """

    script = REPO_ROOT / "docs" / "generate-api-docs.sh"
    try:
        docs_contract.assert_contained(script, REPO_ROOT, label="API documentation generator")
        docs_contract.regular_identity(script, label="API documentation generator")
    except docs_contract.ContractError as error:
        raise SiteDataError("missing or unsafe API documentation generator: docs/generate-api-docs.sh") from error
    if not script.is_file() or script.is_symlink():
        raise SiteDataError("missing API documentation generator: docs/generate-api-docs.sh")

    api_root = REPO_ROOT / "docs" / "api"
    environment = api_generation_environment(source_commit, committed_at, api_root)
    result = run_bounded_process(
        [trusted_bash(), str(script), "generate"],
        cwd=REPO_ROOT,
        environment=environment,
        timeout=API_GENERATION_TIMEOUT_SECONDS,
        label="API documentation generation",
    )
    if result.returncode:
        detail = (result.stderr.strip() or result.stdout.strip())[-4000:]
        raise SiteDataError(f"API documentation generation failed: {detail}")

    try:
        tree = docs_contract.generated_tree_snapshot(api_root, label="site-data API output")
        manifest = docs_contract.load_bounded_json(
            api_root / ".generation.json", label="API generation metadata"
        )
        manifest_errors = docs_contract.validate_api_manifest(api_root, source_commit)
    except docs_contract.ContractError as error:
        raise SiteDataError(f"API documentation output is unsafe or invalid: {error}") from error
    if manifest_errors:
        raise SiteDataError("API documentation manifest validation failed: " + "; ".join(manifest_errors[:3]))
    pages = [name for name in tree if PurePosixPath(name).suffix.lower() == ".md"]
    if "README.md" not in tree or len(pages) < 3:
        raise SiteDataError("API documentation generator did not produce its index and reference pages")
    if not isinstance(manifest, dict):
        raise SiteDataError("API documentation generation metadata must be a JSON object")
    try:
        paths, symbols, _ = docs_contract.scan_sources()
        header_extensions = {
            value.lower()
            for value in docs_contract.load_contract()["sourceContract"]["headerExtensions"]
        }
    except docs_contract.ContractError as error:
        raise SiteDataError(f"cannot bind API metadata to the active source contract: {error}") from error
    headers = [path for path in paths if path.suffix.lower() in header_extensions]
    expected_manifest = {
        "schemaVersion": 1,
        "sourceCommit": source_commit,
        "sourceCommittedAt": committed_at,
        "sourcesScanned": len(paths),
        "headersScanned": len(headers),
        "cppFilesScanned": sum(
            path.suffix.lower() in {".c", ".cc", ".cpp", ".cxx", ".mm"}
            for path in paths
        ),
        "markdownPages": len(pages),
        "symbolRecords": len(symbols),
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
        source_files, source_lines = file_lines(tracked_files(module / "Source"))
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


def codebase_metrics_module() -> Any:
    """Load the one tracked-tree inventory definition shared with README badges.

    docs/update-readme-badges.sh publishes these same counts, so a second
    definition here is a second answer to one public question.
    """
    specification = importlib.util.spec_from_file_location(
        "spark_codebase_metrics", CODEBASE_METRICS_PATH
    )
    if specification is None or specification.loader is None:
        raise SiteDataError(f"cannot load {to_posix(CODEBASE_METRICS_PATH.relative_to(REPO_ROOT))}")
    module = importlib.util.module_from_spec(specification)
    try:
        specification.loader.exec_module(module)
    except (OSError, ValueError) as error:
        raise SiteDataError(f"codebase metrics are unavailable: {error}") from error
    return module


def ctest_summary(path: Path) -> dict[str, int]:
    """Execution totals from the exact-commit CTest JUnit report."""
    payload = read_bytes_stable(path, MAX_CTEST_REPORT_BYTES, "CTest JUnit report")
    document = payload.decode("utf-8", errors="replace")
    # CTest emits no doctype. Refusing one keeps entity expansion out of the
    # parser instead of trusting a report that reached us through an artifact.
    if "<!DOCTYPE" in document or "<!ENTITY" in document:
        raise SiteDataError("CTest JUnit report declares a doctype or entity and is refused")
    try:
        root = ElementTree.fromstring(document)
    except ElementTree.ParseError as error:
        raise SiteDataError(f"CTest JUnit report is not well-formed XML: {error}") from error
    cases = root.findall(".//testcase")
    if not cases:
        raise SiteDataError("CTest JUnit report contains no test cases")
    failed = sum(
        1 for case in cases if case.find("failure") is not None or case.find("error") is not None
    )
    skipped = sum(1 for case in cases if case.find("skipped") is not None)
    return {"executed": len(cases), "failed": failed, "skipped": skipped}


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


def collect_metrics(
    authored_documents: int,
    modules: list[dict[str, Any]],
    execution: dict[str, int] | None = None,
) -> list[dict[str, Any]]:
    inventory_module = codebase_metrics_module()
    try:
        inventory = inventory_module.collect()
    except ValueError as error:
        raise SiteDataError(f"codebase metrics are unavailable: {error}") from error
    canonical_code_roots = sorted(
        {root for roots in inventory_module.CATEGORIES.values() for root in roots}
    )

    panel_path = REPO_ROOT / "SparkEditor" / "Source" / "Core" / "EditorPanelFactory.cpp"
    panel_content = panel_path.read_text(encoding="utf-8", errors="ignore")
    editor_panels = len(re.findall(r"\btryRegister\s*\(\s*\"", panel_content))

    palette_path = REPO_ROOT / "SparkEngine" / "Source" / "Engine" / "Scripting" / "VisualScriptCompiler.cpp"
    palette_content = palette_path.read_text(encoding="utf-8", errors="ignore")
    palette_match = re.search(r"\bkPalette\s*=\s*\{(?P<body>.*?)\};", palette_content, flags=re.DOTALL)
    visual_nodes = len(re.findall(r"\{\s*ScriptNodeType::", palette_match.group("body") if palette_match else ""))

    shader_root = REPO_ROOT / "Shaders"
    hlsl = len(tracked_files(shader_root, {".hlsl"}))
    glsl = len(tracked_files(shader_root, {".glsl"}))

    networking_files: list[Path] = []
    for root in (
        REPO_ROOT / "SparkEngine" / "Source" / "Engine" / "Networking",
        REPO_ROOT / "SparkEngine" / "Source" / "Engine" / "OnlineServices",
    ):
        networking_files.extend(tracked_files(root))
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
        {"type": "metric", "path": "docs/codebase-metrics.py", "selector": "collect", "label": "Tracked-tree source and test inventory"},
        *(
            {"type": "source", "path": path, "label": "Canonical LOC root"}
            for path in canonical_code_roots
        ),
    ]
    execution_metrics: list[dict[str, Any]] = []
    if execution is not None:
        execution_evidence = [
            {
                "type": "workflow",
                "path": ".github/workflows/build.yml",
                "selector": "build-windows-vs2022",
                "label": "CTest JUnit report for the exact commit",
            }
        ]
        execution_metrics = [
            metric("tests.executed", "CTest cases executed", execution["executed"], execution_evidence, "tests"),
            metric("tests.failed", "CTest cases failed", execution["failed"], execution_evidence, "tests"),
            metric("tests.skipped", "CTest cases skipped", execution["skipped"], execution_evidence, "tests"),
        ]
    return [
        metric("code.totalLines", "C/C++ physical lines", inventory["total_lines"], code_evidence, "lines"),
        metric("code.files", "C/C++ source files", inventory["file_count"], code_evidence, "files"),
        metric("tests.definitions", "SparkTests TEST and TEST_F definitions", inventory["test_definitions"], code_evidence, "tests"),
        metric("tests.files", "Source files defining SparkTests cases", inventory["test_files"], code_evidence, "files"),
        *execution_metrics,
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


def summarize_documentation_health(payload: Any, exit_code: int, commit: str) -> dict[str, Any]:
    """Translate schema-v1 health evidence into the published health block.

    Every declared generator must appear exactly once with its own status, and
    the aggregate counts must agree with the rows they summarize; anything else
    is evidence that did not measure what it claims to measure.
    """
    if not isinstance(payload, dict) or payload.get("schemaVersion") != 1:
        raise SiteDataError("documentation health evidence is not schema-v1")
    if payload.get("sourceCommit") != commit:
        raise SiteDataError(
            f"documentation health evidence is bound to {payload.get('sourceCommit')!r}, not {commit}"
        )
    results = payload.get("results")
    if not isinstance(results, list) or not results:
        raise SiteDataError("documentation health evidence declares no generator results")
    checks: list[dict[str, Any]] = []
    for row in results:
        if not isinstance(row, dict) or not isinstance(row.get("id"), str) or not row["id"]:
            raise SiteDataError("documentation health evidence has an unidentified generator result")
        if not isinstance(row.get("status"), str) or not isinstance(row.get("message"), str):
            raise SiteDataError(f"documentation health result for {row['id']!r} is malformed")
        checks.append(
            {
                "name": row["id"],
                "status": "current" if row["status"] == "current" else "refresh-pending",
                "detail": row["message"][:500],
            }
        )
    names = [check["name"] for check in checks]
    duplicates = sorted({name for name in names if names.count(name) > 1})
    if duplicates:
        raise SiteDataError(f"documentation health evidence repeats generators: {duplicates}")
    failures = sum(1 for check in checks if check["status"] != "current")
    successes = len(checks) - failures
    if payload.get("failures") != failures or payload.get("successes") != successes:
        raise SiteDataError("documentation health aggregate counts contradict the generator results")
    current = (
        exit_code == 0
        and failures == 0
        and payload.get("overall") == "pass"
        and payload.get("exitCode") == 0
    )
    return {
        "status": "current" if current else "refresh-pending",
        "checks": checks,
        "successes": successes,
        "failures": failures,
        "exitCode": payload.get("exitCode"),
        "sourceCommit": commit,
    }


def documentation_health_from_file(path: Path, commit: str) -> dict[str, Any]:
    """Consume health evidence a job already produced for this exact commit.

    docs/update-all-docs.sh writes this file itself, so reusing it costs one
    read instead of a second full regeneration; it is accepted only when it is
    bound to the commit being published.
    """
    payload = load_json(path, maximum=MAX_HEALTH_JSON_BYTES)
    exit_code = payload.get("exitCode") if isinstance(payload, dict) else None
    return summarize_documentation_health(payload, exit_code if isinstance(exit_code, int) else 1, commit)


def documentation_health(commit: str, *, emit_to: Path | None = None) -> dict[str, Any]:
    """Regenerate every declared documentation output in an isolated checkout.

    The verdict comes from the machine-readable evidence the generator writes
    for itself (docs/.health.json), not from scraped console text. A check that
    cannot run raises: an unrunnable check is not a passing one, and publishing
    it as "unknown" is how a never-executed gate reaches the website.
    """
    script_relative = Path("docs") / "update-all-docs.sh"
    if not (REPO_ROOT / script_relative).is_file():
        raise SiteDataError(f"documentation health requires {to_posix(script_relative)}")
    with tempfile.TemporaryDirectory(prefix="sparkengine-doc-health-") as temporary:
        checkout = Path(temporary) / "checkout"
        health_path = Path(temporary) / "health.json"
        add = subprocess.run(
            ["git", "worktree", "add", "--detach", str(checkout), commit],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        if add.returncode:
            detail = add.stderr.strip() or add.stdout.strip()
            raise SiteDataError(f"isolated documentation health check is unavailable: {detail}")
        try:
            environment = os.environ.copy()
            for key in DOC_OUTPUT_ENVIRONMENT:
                environment.pop(key, None)
            environment["SPARK_DOC_HEALTH_OUTPUT"] = str(health_path)
            result = run_bounded_process(
                ["bash", str(checkout / script_relative), "update"],
                cwd=checkout,
                environment=environment,
                timeout=DOC_HEALTH_TIMEOUT_SECONDS,
                label="isolated documentation health check",
            )
            if not health_path.is_file():
                raise SiteDataError("isolated documentation health check produced no health evidence")
            payload = load_json(health_path, maximum=MAX_HEALTH_JSON_BYTES)
            if emit_to is not None:
                # Publishing this evidence lets a later run in the same job
                # consume it with --doc-health-file. A determinism proof that
                # skips health does not cover the block that actually ships;
                # reusing one regeneration lets the proof and the published
                # payload run the same health mode inside one time budget.
                emit_to.parent.mkdir(parents=True, exist_ok=True)
                emit_to.write_bytes(health_path.read_bytes())
        finally:
            subprocess.run(
                ["git", "worktree", "remove", "--force", str(checkout)],
                cwd=REPO_ROOT,
                capture_output=True,
                text=True,
                check=False,
            )
    return summarize_documentation_health(payload, result.returncode, commit)


def documentation_health_block(
    commit: str,
    check_health: bool,
    health_file: Path | None,
    emit_health_file: Path | None = None,
) -> dict[str, Any]:
    if not check_health:
        if emit_health_file is not None:
            raise SiteDataError("--emit-doc-health-file cannot be combined with --skip-doc-health")
        return {"status": "skipped", "checks": []}
    if health_file is not None:
        if emit_health_file is not None and health_file.resolve() != emit_health_file.resolve():
            emit_health_file.parent.mkdir(parents=True, exist_ok=True)
            emit_health_file.write_bytes(health_file.read_bytes())
        return documentation_health_from_file(health_file, commit)
    return documentation_health(commit, emit_to=emit_health_file)


def build_documents(
    contract: dict[str, Any],
    source: dict[str, str],
    *,
    check_health: bool,
    health_file: Path | None = None,
    emit_health_file: Path | None = None,
) -> tuple[list[dict[str, Any]], list[dict[str, Any]], dict[str, Any], int]:
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
        "health": documentation_health_block(commit, check_health, health_file, emit_health_file),
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
    resolved = docs_contract.absolute_path(output)
    forbidden = {
        docs_contract.absolute_path(Path("/")),
        docs_contract.absolute_path(Path.home()),
        docs_contract.absolute_path(REPO_ROOT),
        docs_contract.absolute_path(REPO_ROOT.parent),
    }
    if resolved in forbidden:
        raise SiteDataError(f"refusing unsafe output directory: {resolved}")
    try:
        docs_contract.assert_no_reparse_ancestors(resolved.parent, label="site-data output")
    except docs_contract.ContractError as error:
        raise SiteDataError(str(error)) from error
    if os.path.lexists(resolved):
        try:
            docs_contract.generated_tree_snapshot(
                resolved,
                label="site-data output",
                max_files=MAX_OUTPUT_FILES,
                max_bytes=MAX_OUTPUT_BYTES,
            )
        except docs_contract.ContractError as error:
            raise SiteDataError(str(error)) from error
    if not preserve_existing and os.path.lexists(resolved):
        sentinel = resolved / ".sparkengine-site-data-output"
        narrow_name = re.fullmatch(r"\.site-data(?:[-._][a-zA-Z0-9_-]+)?", resolved.name)
        try:
            sentinel_is_safe = (
                docs_contract.read_regular_bytes(
                    sentinel, label="site-data output sentinel", maximum=1024
                )
                == b"SparkEngine repository site-data output\n"
            )
        except docs_contract.ContractError:
            sentinel_is_safe = False
        if not sentinel_is_safe and not narrow_name:
            raise SiteDataError(
                f"refusing to replace non-site-data directory without generator sentinel: {resolved}"
            )
        try:
            docs_contract.remove_generated_tree(
                resolved,
                label="site-data output",
                max_files=MAX_OUTPUT_FILES,
                max_bytes=MAX_OUTPUT_BYTES,
            )
        except docs_contract.ContractError as error:
            raise SiteDataError(str(error)) from error
    resolved.mkdir(parents=True, exist_ok=True)
    write_text(resolved / ".sparkengine-site-data-output", "SparkEngine repository site-data output\n")
    return resolved


def pointer(output: Path, path: Path, info: dict[str, Any]) -> dict[str, Any]:
    return {**info, "path": path.relative_to(output).as_posix()}


def prune_snapshots(output: Path, current_commit: str, retain: int) -> None:
    snapshots = output / "snapshots"
    if not snapshots.exists():
        return
    try:
        docs_contract.generated_tree_snapshot(
            snapshots,
            label="site-data snapshots",
            max_files=MAX_OUTPUT_FILES,
            max_bytes=MAX_OUTPUT_BYTES,
        )
    except docs_contract.ContractError as error:
        raise SiteDataError(str(error)) from error
    records: list[tuple[str, str, Path]] = []
    for directory in snapshots.iterdir():
        bundle = directory / "bundle.json"
        if not directory.is_dir() or directory.is_symlink():
            raise SiteDataError(f"site-data snapshot entry is unsafe: {directory}")
        if not bundle.is_file() or bundle.is_symlink():
            raise SiteDataError(f"site-data snapshot is missing its bundle: {directory}")
        try:
            data = load_json(bundle, maximum=5 * 1024 * 1024)
            if not isinstance(data, dict):
                raise SiteDataError("site-data snapshot bundle must be an object")
            records.append((data.get("generatedAt", ""), directory.name, directory))
        except (OSError, SiteDataError):
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
            try:
                docs_contract.remove_generated_tree(
                    directory,
                    label="obsolete site-data snapshot",
                    max_files=MAX_OUTPUT_FILES,
                    max_bytes=MAX_OUTPUT_BYTES,
                )
            except docs_contract.ContractError as error:
                raise SiteDataError(str(error)) from error


def enforce_publication_budgets(output: Path, bundle_path: Path, page_root: Path) -> None:
    budgets = {
        "latest.json": (output / "latest.json", 32 * 1024),
        "bundle metadata": (bundle_path, 5 * 1024 * 1024),
    }
    for label, (path, maximum) in budgets.items():
        try:
            size = docs_contract.regular_identity(path, label=label).size
        except docs_contract.ContractError as error:
            raise SiteDataError(str(error)) from error
        if size > maximum:
            raise SiteDataError(f"{label} is {size} bytes; publication budget is {maximum} bytes")
    try:
        pages = docs_contract.generated_tree_snapshot(page_root, label="site-data page output")
    except docs_contract.ContractError as error:
        raise SiteDataError(str(error)) from error
    oversize = sorted(
        (identity.size, page_root.joinpath(*PurePosixPath(raw).parts))
        for raw, identity in pages.items()
        if PurePosixPath(raw).suffix == ".json" and identity.size > 2 * 1024 * 1024
    )
    if oversize:
        size, path = oversize[-1]
        raise SiteDataError(
            f"documentation page {path.name} is {size} bytes; publication budget is {2 * 1024 * 1024} bytes"
        )


def generate(args: argparse.Namespace) -> dict[str, Any]:
    contract = validate_contract(allow_legacy_contract=args.allow_legacy_contract)
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
    exact_evidence: dict[str, Any] | None = None
    if args.exact_evidence_file is not None:
        try:
            exact_evidence = validate_exact_evidence_manifest(
                load_exact_evidence_manifest(args.exact_evidence_file)
            )
        except ExactEvidenceError as error:
            raise SiteDataError(f"exact CI evidence is invalid: {error}") from error
        if exact_evidence["sourceCommit"] != source["commit"]:
            raise SiteDataError(
                "exact CI evidence sourceCommit does not match the checked-out source commit"
            )

    regenerate_api_docs(source["commit"], source["committedAt"])

    output = ensure_safe_output(args.output, preserve_existing=args.preserve_existing)
    snapshot_root = output / "snapshots" / source["commit"]
    if os.path.lexists(snapshot_root):
        try:
            docs_contract.remove_generated_tree(
                snapshot_root,
                label="existing site-data snapshot",
                max_files=MAX_OUTPUT_FILES,
                max_bytes=MAX_OUTPUT_BYTES,
            )
        except docs_contract.ContractError as error:
            raise SiteDataError(str(error)) from error
    docs_root = snapshot_root / "docs"
    page_root = docs_root / "pages"
    page_root.mkdir(parents=True, exist_ok=True)

    documents, sections, docs_snapshot, authored_count = build_documents(
        contract,
        source,
        check_health=not args.skip_doc_health,
        health_file=args.doc_health_file,
        emit_health_file=args.emit_doc_health_file,
    )
    health = docs_snapshot["health"]
    if health["status"] != "current" and not args.skip_doc_health:
        stale = ", ".join(
            check["name"] for check in health["checks"] if check["status"] != "current"
        )
        raise SiteDataError(
            "documentation health is not current; refresh-pending generators: "
            f"{stale or 'unreported'}"
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
    execution = ctest_summary(args.ctest_junit) if args.ctest_junit else None
    metrics = collect_metrics(authored_count, modules, execution)
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
    if exact_evidence is not None:
        publication["exactEvidence"] = exact_evidence
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
    if exact_evidence is not None:
        exact_evidence_path = snapshot_root / "exact-ci-evidence.json"
        split_files["exactCiEvidence"] = pointer(
            output,
            exact_evidence_path,
            write_json(exact_evidence_path, exact_evidence),
        )
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
        "# SparkEngine site data\n\nGenerated exact-commit website data. Do not edit this moving tag by hand; update the repository contracts and let the publication workflow rebuild it.\n",
    )
    enforce_publication_budgets(output, bundle_path, page_root)
    prune_snapshots(output, source["commit"], max(1, args.retain))
    try:
        docs_contract.generated_tree_snapshot(
            output,
            label="published site-data output",
            max_files=MAX_OUTPUT_FILES,
            max_bytes=MAX_OUTPUT_BYTES,
        )
    except docs_contract.ContractError as error:
        raise SiteDataError(str(error)) from error
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
    parser.add_argument(
        "--exact-evidence-file",
        type=Path,
        help="validated exact build-matrix and CodeQL evidence manifest to retain in the snapshot",
    )
    parser.add_argument(
        "--allow-legacy-contract",
        action="store_true",
        help=(
            "DEPRECATED, retired waiver: accept unresolved CI job, test selector, and "
            "path references with a warning. The contract validates strictly, so this "
            "flag can only hide a newly added unresolvable reference"
        ),
    )
    parser.add_argument(
        "--doc-health-file",
        type=Path,
        help="schema-v1 documentation health evidence for this commit, instead of regenerating it",
    )
    parser.add_argument(
        "--ctest-junit",
        type=Path,
        help="CTest JUnit report for this exact commit; publishes tests.executed/failed/skipped",
    )
    parser.add_argument(
        "--emit-doc-health-file",
        type=Path,
        help=(
            "write the documentation health evidence this run computed, so a later run "
            "in the same job can consume it with --doc-health-file and publish the same "
            "health block the determinism proof compared"
        ),
    )
    parser.add_argument(
        "--skip-doc-health",
        action="store_true",
        help="publish with docs health 'skipped'; never valid for a release publication",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.allow_legacy_contract:
        warn_legacy_contract_flag_deprecated("generate.py")
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
