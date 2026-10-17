#!/usr/bin/env python3
"""Generate a self-contained 3D "code city" of the SparkEngine source tree.

Every tracked C/C++/shader/script source outside ThirdParty becomes a building:
footprint and height grow with its line count, and it stands in a district (a
subsystem directory) inside a project block (engine, editor, a game module,
tests, tools...). Include directives are resolved to files and aggregated into
district-to-district dependency arcs. Each file also carries its recent commit
count and the readiness work items whose entry points name it.

The output is one HTML file (data embedded, three.js loaded from jsDelivr) that
opens directly from disk:

    python3 tools/architecture-viz/generate_code_city.py            # -> build/code-city/index.html
    python3 tools/architecture-viz/generate_code_city.py --output /tmp/city.html

Standard library only. Paths come from `git ls-files`, so the map shows exactly
what is committed plus tracked-file edits in the working tree.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TEMPLATE = Path(__file__).with_name("code_city_template.html")
DATA_MARKER = "/*__CODE_CITY_DATA__*/null"

SOURCE_SUFFIXES = (".h", ".hpp", ".inl", ".cpp", ".cc", ".c", ".mm", ".as", ".hlsl", ".glsl", ".hlsli")
EXCLUDED_PREFIXES = ("ThirdParty/", "build/", "docs/")
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^">]+)[">]', re.MULTILINE)
CLASS_RE = re.compile(r"^\s*(?:class|struct)\s+(?:[A-Z_]+_API\s+|alignas\([^)]*\)\s+)?([A-Za-z_]\w*)\s*(?:final\s*)?[:{]",
                      re.MULTILINE)
CHURN_DAYS = 180


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=REPO_ROOT, check=True, capture_output=True,
                          text=True, encoding="utf-8", errors="replace").stdout


def tracked_sources() -> list[str]:
    paths = []
    for path in git("ls-files", "-z").split("\0"):
        if path and path.endswith(SOURCE_SUFFIXES) and not path.startswith(EXCLUDED_PREFIXES):
            if (REPO_ROOT / path).is_file():
                paths.append(path)
    return sorted(paths)


def locate(path: str) -> tuple[str, str]:
    """Map a repository path to (project, district)."""

    parts = path.split("/")
    directories = parts[:-1]

    def district_after(index: int, depth: int = 2) -> str:
        rest = directories[index:index + depth]
        return "/".join(rest) if rest else "(root)"

    top = parts[0]
    if top == "SparkEngine" and len(parts) > 2 and parts[1] == "Source":
        # Engine/<Subsystem> and Graphics/RHI are the natural subsystem granularity.
        if len(directories) > 2 and directories[2] in ("Engine", "Graphics"):
            return "SparkEngine", district_after(2, 2)
        return "SparkEngine", district_after(2, 1)
    if top == "GameModules" and len(parts) > 2:
        module = parts[1]
        if len(parts) > 3 and parts[2] == "Source":
            return f"GameModules/{module}", district_after(3, 1)
        return f"GameModules/{module}", district_after(2, 1)
    if top == "SparkEditor" and len(parts) > 2 and parts[1] == "Source":
        return "SparkEditor", district_after(2, 1)
    if top == "Tests":
        if len(directories) > 1:
            return "Tests", district_after(1, 1)
        stem = Path(parts[-1]).stem
        match = re.match(r"Test_?([A-Z][a-z]+|[A-Z]+(?=[A-Z][a-z]|\d|_|$))", stem)
        return "Tests", (match.group(1) if match else "(root)")
    return top, district_after(1, 1)


def file_kind(path: str) -> str:
    name = path.rsplit("/", 1)[-1]
    if path.startswith("Tests/") or "/tests/" in path.lower() or name.startswith("Test"):
        return "test"
    if name.endswith((".h", ".hpp", ".inl", ".hlsli")):
        return "header"
    if name.endswith((".hlsl", ".glsl")):
        return "shader"
    if name.endswith(".as"):
        return "script"
    return "source"


def include_roots(paths: list[str]) -> list[str]:
    roots = {"", "SparkEngine/Source", "SparkEditor/Source", "SparkSDK/Include", "Tests", "SparkEngine/Source/Engine"}
    for path in paths:
        parts = path.split("/")
        if parts[0] == "GameModules" and len(parts) > 3 and parts[2] == "Source":
            roots.add(f"GameModules/{parts[1]}/Source")
        if len(parts) > 2 and parts[1] in ("src", "Source", "Include", "include"):
            roots.add(f"{parts[0]}/{parts[1]}")
    return sorted(roots)


def resolve_include(include: str, including: str, known: set[str], roots: list[str],
                    by_name: dict[str, list[str]]) -> str | None:
    include = include.replace("\\", "/")
    base = including.rsplit("/", 1)[0] if "/" in including else ""
    candidates = [f"{base}/{include}" if base else include] + [f"{root}/{include}" if root else include for root in roots]
    for candidate in candidates:
        normalized = str(Path(candidate).as_posix())
        parts: list[str] = []
        for part in normalized.split("/"):
            if part == "..":
                if parts:
                    parts.pop()
            elif part not in ("", "."):
                parts.append(part)
        joined = "/".join(parts)
        if joined in known:
            return joined
    # Unique basename fallback for includes written against an include path we did not model.
    matches = by_name.get(include.rsplit("/", 1)[-1], [])
    suffix_matches = [m for m in matches if m.endswith("/" + include) or m == include]
    if len(suffix_matches) == 1:
        return suffix_matches[0]
    return None


def churn_counts() -> Counter:
    counts: Counter = Counter()
    try:
        log = git("log", f"--since={CHURN_DAYS}.days", "--name-only", "--format=")
    except subprocess.CalledProcessError:
        return counts
    for line in log.splitlines():
        if line:
            counts[line] += 1
    return counts


def readiness_links(paths: list[str]) -> tuple[list[dict], dict[str, list[int]]]:
    items = []
    for work_file in sorted((REPO_ROOT / "docs" / "readiness" / "work-items").glob("*.json")):
        items.extend(json.loads(work_file.read_text(encoding="utf-8")).get("workItems", []))
    summary = [{"id": item["id"], "title": item.get("title", ""), "status": item.get("status", ""),
                "priority": item.get("priority", "")} for item in items]
    links: dict[str, list[int]] = defaultdict(list)
    for index, item in enumerate(items):
        for entry in item.get("entryPoints", []) or []:
            entry = str(entry).rstrip("/")
            for path in paths:
                if path == entry or path.startswith(entry + "/"):
                    links[path].append(index)
    return summary, links


def squarify(sizes: list[float], x: float, y: float, width: float, height: float) -> list[tuple[float, float, float, float]]:
    """Squarified treemap (Bruls et al.) over sizes sorted descending; returns rects in input order."""

    order = sorted(range(len(sizes)), key=lambda i: -sizes[i])
    total = sum(sizes) or 1.0
    scale = (width * height) / total
    areas = [(order_index, sizes[order_index] * scale) for order_index in order]
    rects: dict[int, tuple[float, float, float, float]] = {}

    def worst(row: list[float], side: float) -> float:
        s = sum(row)
        return max(max(side * side * r / (s * s), (s * s) / (side * side * r)) for r in row)

    while areas:
        side = min(width, height)
        row = [areas[0]]
        areas = areas[1:]
        while areas and worst([a for _, a in row] + [areas[0][1]], side) <= worst([a for _, a in row], side):
            row.append(areas[0])
            areas = areas[1:]
        row_area = sum(a for _, a in row)
        if width >= height:
            column_width = row_area / height if height else 0
            offset = y
            for index, area in row:
                h = area / column_width if column_width else 0
                rects[index] = (x, offset, column_width, h)
                offset += h
            x += column_width
            width -= column_width
        else:
            row_height = row_area / width if width else 0
            offset = x
            for index, area in row:
                w = area / row_height if row_height else 0
                rects[index] = (offset, y, w, row_height)
                offset += w
            y += row_height
            height -= row_height
    return [rects[i] for i in range(len(sizes))]


def footprint(loc: int) -> float:
    return max(1.2, min(6.0, 0.9 + math.sqrt(max(loc, 1)) / 9.0))


def build_city(paths: list[str]) -> dict:
    known = set(paths)
    roots = include_roots(paths)
    by_name: dict[str, list[str]] = defaultdict(list)
    for path in paths:
        by_name[path.rsplit("/", 1)[-1]].append(path)

    churn = churn_counts()
    work_items, work_links = readiness_links(paths)

    files = []
    index_of = {path: i for i, path in enumerate(paths)}
    raw_includes: list[list[str]] = []
    for path in paths:
        text = (REPO_ROOT / path).read_text(encoding="utf-8", errors="replace")
        lines = text.count("\n") + (0 if text.endswith("\n") or not text else 1)
        project, district = locate(path)
        classes = sorted(set(CLASS_RE.findall(text)))
        raw_includes.append(INCLUDE_RE.findall(text))
        files.append({"path": path, "project": project, "district": district, "loc": lines,
                      "kind": file_kind(path), "classes": classes[:40], "classCount": len(classes),
                      "churn": churn.get(path, 0), "work": work_links.get(path, [])})

    unresolved = 0
    for i, includes in enumerate(raw_includes):
        targets = []
        for delimiter, include in includes:
            resolved = resolve_include(include, paths[i], known, roots, by_name)
            if resolved is None:
                # <...> includes are mostly the standard library and vendored SDKs; only a
                # quoted include that names no tracked file counts as unresolved.
                unresolved += delimiter == '"'

            elif resolved != paths[i]:
                targets.append(index_of[resolved])
        files[i]["inc"] = sorted(set(targets))

    # Layout: projects (treemap) -> districts (treemap) -> buildings (grid).
    projects: dict[str, dict[str, list[int]]] = defaultdict(lambda: defaultdict(list))
    for i, entry in enumerate(files):
        projects[entry["project"]][entry["district"]].append(i)

    gap = 1.0
    district_area = {}
    for project, districts in projects.items():
        for district, members in districts.items():
            area = sum((footprint(files[m]["loc"]) + gap) ** 2 for m in members)
            district_area[(project, district)] = area * 1.15 + 16.0
    project_names = sorted(projects)
    project_sizes = [sum(district_area[(p, d)] for d in projects[p]) * 1.12 + 64.0 for p in project_names]
    total = sum(project_sizes)
    side = math.sqrt(total) * 1.05
    project_rects = squarify(project_sizes, -side / 2, -side / 2, side, side)

    project_out, district_out = [], []
    for p_index, (project, rect) in enumerate(zip(project_names, project_rects)):
        px, pz, pw, ph = rect
        margin = min(4.0, 0.06 * min(pw, ph))
        inner = (px + margin, pz + margin + 2.0, max(pw - 2 * margin, 1.0), max(ph - 2 * margin - 2.0, 1.0))
        project_out.append({"name": project, "rect": [round(v, 2) for v in rect], "loc": 0, "files": 0})
        names = sorted(projects[project])
        rects = squarify([district_area[(project, d)] for d in names], *inner)
        for district, (dx, dz, dw, dh) in zip(names, rects):
            d_index = len(district_out)
            pad = min(1.5, 0.08 * min(dw, dh))
            members = sorted(projects[project][district], key=lambda m: -files[m]["loc"])
            cells = max(len(members), 1)
            cols = max(1, round(math.sqrt(cells * max(dw - 2 * pad, 0.5) / max(dh - 2 * pad, 0.5))))
            rows = math.ceil(cells / cols)
            cell_w = max(dw - 2 * pad, 0.5) / cols
            cell_h = max(dh - 2 * pad, 0.5) / rows
            for k, m in enumerate(members):
                col, row = k % cols, k // cols
                size = min(footprint(files[m]["loc"]), cell_w * 0.82, cell_h * 0.82)
                files[m]["x"] = round(dx + pad + (col + 0.5) * cell_w, 2)
                files[m]["z"] = round(dz + pad + (row + 0.5) * cell_h, 2)
                files[m]["s"] = round(max(size, 0.25), 2)
                files[m]["h"] = round(max(0.6, 1.2 * max(files[m]["loc"], 1) ** 0.55), 2)
                files[m]["d"] = d_index
            loc = sum(files[m]["loc"] for m in members)
            district_out.append({"name": district, "project": p_index, "rect": [round(v, 2) for v in (dx, dz, dw, dh)],
                                 "files": len(members), "loc": loc})
            project_out[p_index]["loc"] += loc
            project_out[p_index]["files"] += len(members)

    edge_weights: Counter = Counter()
    for entry in files:
        for target in entry["inc"]:
            a, b = entry["d"], files[target]["d"]
            if a != b:
                edge_weights[(a, b)] += 1
    edges = [[a, b, w] for (a, b), w in edge_weights.most_common(900)]

    head = git("rev-parse", "--short", "HEAD").strip()
    branch = git("rev-parse", "--abbrev-ref", "HEAD").strip()
    return {
        "meta": {"commit": head, "branch": branch, "files": len(files), "loc": sum(f["loc"] for f in files),
                 "includeEdges": sum(len(f["inc"]) for f in files), "unresolvedIncludes": unresolved,
                 "churnDays": CHURN_DAYS, "districtEdges": len(edge_weights), "shownDistrictEdges": len(edges)},
        "projects": project_out, "districts": district_out, "files": files, "edges": edges, "workItems": work_items,
    }


def render(city: dict) -> str:
    template = TEMPLATE.read_text(encoding="utf-8")
    if template.count(DATA_MARKER) != 1:
        raise SystemExit(f"template {TEMPLATE} must contain the data marker exactly once")
    payload = json.dumps(city, separators=(",", ":"), ensure_ascii=True).replace("</", "<\\/")
    return template.replace(DATA_MARKER, payload)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, default=REPO_ROOT / "build" / "code-city" / "index.html")
    args = parser.parse_args(argv)

    paths = tracked_sources()
    if not paths:
        print("no tracked sources found", file=sys.stderr)
        return 1
    city = build_city(paths)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(render(city), encoding="utf-8")
    meta = city["meta"]
    print(f"wrote {args.output} ({meta['files']} files, {meta['loc']} lines, {len(city['districts'])} districts, "
          f"{meta['includeEdges']} include edges, {meta['unresolvedIncludes']} unresolved includes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
