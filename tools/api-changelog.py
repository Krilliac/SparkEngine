#!/usr/bin/env python3
"""
api-changelog.py — Automated API changelog generation for SparkEngine

Diffs public SDK headers between two git refs (tags, branches, commits) and
generates a structured changelog of new, removed, and changed API symbols.

Usage:
    python3 tools/api-changelog.py v1.0.0 v1.1.0
    python3 tools/api-changelog.py HEAD~10 HEAD
    python3 tools/api-changelog.py v1.0.0 HEAD --format json
    python3 tools/api-changelog.py v1.0.0 HEAD --output api-changes.md

Output: Markdown or JSON report of API changes.
"""

import argparse
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path


# Directories containing public API headers
API_DIRS = [
    "SparkSDK/Include",
    "SparkEngine/Source/Core/EngineContext.h",
    "SparkEngine/Source/Core/EngineBootstrap.h",
]

# Patterns to extract API symbols from headers
CLASS_PATTERN = re.compile(r'^\s*(?:class|struct)\s+(\w+)', re.MULTILINE)
ENUM_PATTERN = re.compile(r'^\s*enum\s+(?:class\s+)?(\w+)', re.MULTILINE)
FUNCTION_PATTERN = re.compile(
    r'^\s*(?:virtual\s+|static\s+|inline\s+|constexpr\s+)*'
    r'(?:[\w:*&<>]+\s+)+(\w+)\s*\(',
    re.MULTILINE
)
TYPEDEF_PATTERN = re.compile(r'^\s*using\s+(\w+)\s*=', re.MULTILINE)


@dataclass
class APISymbol:
    """A single API symbol extracted from a header."""
    name: str
    kind: str  # "class", "struct", "enum", "function", "typedef"
    file: str
    line_content: str = ""


@dataclass
class APIChange:
    """A change between two API versions."""
    symbol: str
    kind: str
    change_type: str  # "added", "removed", "modified"
    file: str
    old_signature: str = ""
    new_signature: str = ""


@dataclass
class APIChangeReport:
    """Complete API change report."""
    from_ref: str
    to_ref: str
    added: list = field(default_factory=list)
    removed: list = field(default_factory=list)
    modified: list = field(default_factory=list)

    @property
    def total_changes(self):
        return len(self.added) + len(self.removed) + len(self.modified)

    @property
    def has_breaking_changes(self):
        return len(self.removed) > 0 or len(self.modified) > 0


def get_file_at_ref(ref, filepath):
    """Get file contents at a specific git ref."""
    try:
        result = subprocess.run(
            ["git", "show", f"{ref}:{filepath}"],
            capture_output=True, text=True, cwd=find_repo_root()
        )
        if result.returncode == 0:
            return result.stdout
    except (subprocess.SubprocessError, FileNotFoundError):
        pass
    return None


def find_repo_root():
    """Find the git repository root."""
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            capture_output=True, text=True
        )
        if result.returncode == 0:
            return result.stdout.strip()
    except (subprocess.SubprocessError, FileNotFoundError):
        pass
    return os.getcwd()


def list_api_headers(ref):
    """List all API header files at a given ref."""
    repo_root = find_repo_root()
    headers = []

    for api_dir in API_DIRS:
        if api_dir.endswith(".h"):
            # Single file
            content = get_file_at_ref(ref, api_dir)
            if content is not None:
                headers.append(api_dir)
        else:
            # Directory — list files via git ls-tree
            try:
                result = subprocess.run(
                    ["git", "ls-tree", "-r", "--name-only", ref, api_dir],
                    capture_output=True, text=True, cwd=repo_root
                )
                if result.returncode == 0:
                    for line in result.stdout.strip().split("\n"):
                        if line.endswith(".h") or line.endswith(".hpp"):
                            headers.append(line)
            except (subprocess.SubprocessError, FileNotFoundError):
                pass

    return headers


def extract_symbols(content, filepath):
    """Extract API symbols from header file content."""
    symbols = {}

    for match in CLASS_PATTERN.finditer(content):
        name = match.group(1)
        symbols[name] = APISymbol(name=name, kind="class", file=filepath)

    for match in ENUM_PATTERN.finditer(content):
        name = match.group(1)
        symbols[name] = APISymbol(name=name, kind="enum", file=filepath)

    for match in FUNCTION_PATTERN.finditer(content):
        name = match.group(1)
        # Skip common false positives
        if name in ("if", "for", "while", "switch", "return", "sizeof", "static_cast",
                     "dynamic_cast", "reinterpret_cast", "const_cast"):
            continue
        line = match.group(0).strip()
        symbols[f"{filepath}::{name}"] = APISymbol(
            name=name, kind="function", file=filepath, line_content=line
        )

    for match in TYPEDEF_PATTERN.finditer(content):
        name = match.group(1)
        symbols[name] = APISymbol(name=name, kind="typedef", file=filepath)

    return symbols


def diff_apis(old_symbols, new_symbols):
    """Compare two sets of API symbols and return changes."""
    added = []
    removed = []
    modified = []

    old_keys = set(old_symbols.keys())
    new_keys = set(new_symbols.keys())

    for key in new_keys - old_keys:
        sym = new_symbols[key]
        added.append(APIChange(
            symbol=sym.name, kind=sym.kind,
            change_type="added", file=sym.file,
            new_signature=sym.line_content
        ))

    for key in old_keys - new_keys:
        sym = old_symbols[key]
        removed.append(APIChange(
            symbol=sym.name, kind=sym.kind,
            change_type="removed", file=sym.file,
            old_signature=sym.line_content
        ))

    for key in old_keys & new_keys:
        old_sym = old_symbols[key]
        new_sym = new_symbols[key]
        if old_sym.line_content and new_sym.line_content:
            if old_sym.line_content != new_sym.line_content:
                modified.append(APIChange(
                    symbol=new_sym.name, kind=new_sym.kind,
                    change_type="modified", file=new_sym.file,
                    old_signature=old_sym.line_content,
                    new_signature=new_sym.line_content
                ))

    return added, removed, modified


def generate_report(from_ref, to_ref):
    """Generate a full API change report between two refs."""
    old_headers = list_api_headers(from_ref)
    new_headers = list_api_headers(to_ref)

    old_symbols = {}
    new_symbols = {}

    for header in old_headers:
        content = get_file_at_ref(from_ref, header)
        if content:
            old_symbols.update(extract_symbols(content, header))

    for header in new_headers:
        content = get_file_at_ref(to_ref, header)
        if content:
            new_symbols.update(extract_symbols(content, header))

    added, removed, modified = diff_apis(old_symbols, new_symbols)

    return APIChangeReport(
        from_ref=from_ref,
        to_ref=to_ref,
        added=added,
        removed=removed,
        modified=modified
    )


def format_markdown(report):
    """Format report as Markdown."""
    lines = [
        f"# API Changelog: {report.from_ref} → {report.to_ref}",
        "",
        f"**Total changes:** {report.total_changes}",
        f"**Breaking changes:** {'Yes' if report.has_breaking_changes else 'No'}",
        "",
    ]

    if report.added:
        lines.append(f"## Added ({len(report.added)})")
        lines.append("")
        for change in sorted(report.added, key=lambda c: c.symbol):
            lines.append(f"- `{change.symbol}` ({change.kind}) in `{change.file}`")
        lines.append("")

    if report.removed:
        lines.append(f"## Removed ({len(report.removed)}) ⚠️ BREAKING")
        lines.append("")
        for change in sorted(report.removed, key=lambda c: c.symbol):
            lines.append(f"- `{change.symbol}` ({change.kind}) from `{change.file}`")
        lines.append("")

    if report.modified:
        lines.append(f"## Modified ({len(report.modified)}) ⚠️ POTENTIALLY BREAKING")
        lines.append("")
        for change in sorted(report.modified, key=lambda c: c.symbol):
            lines.append(f"- `{change.symbol}` ({change.kind}) in `{change.file}`")
            if change.old_signature:
                lines.append(f"  - Old: `{change.old_signature}`")
            if change.new_signature:
                lines.append(f"  - New: `{change.new_signature}`")
        lines.append("")

    if report.total_changes == 0:
        lines.append("No API changes detected.")
        lines.append("")

    return "\n".join(lines)


def format_json(report):
    """Format report as JSON."""
    data = {
        "from": report.from_ref,
        "to": report.to_ref,
        "totalChanges": report.total_changes,
        "hasBreakingChanges": report.has_breaking_changes,
        "added": [
            {"symbol": c.symbol, "kind": c.kind, "file": c.file}
            for c in report.added
        ],
        "removed": [
            {"symbol": c.symbol, "kind": c.kind, "file": c.file}
            for c in report.removed
        ],
        "modified": [
            {
                "symbol": c.symbol, "kind": c.kind, "file": c.file,
                "oldSignature": c.old_signature,
                "newSignature": c.new_signature,
            }
            for c in report.modified
        ],
    }
    return json.dumps(data, indent=2)


def main():
    parser = argparse.ArgumentParser(
        description="Generate API changelog between two git refs"
    )
    parser.add_argument("from_ref", help="Starting git ref (tag, branch, or commit)")
    parser.add_argument("to_ref", help="Ending git ref (tag, branch, or commit)")
    parser.add_argument("--format", "-f", default="markdown",
                       choices=["markdown", "json"],
                       help="Output format (default: markdown)")
    parser.add_argument("--output", "-o", default=None,
                       help="Output file (default: stdout)")

    args = parser.parse_args()

    report = generate_report(args.from_ref, args.to_ref)

    if args.format == "json":
        output = format_json(report)
    else:
        output = format_markdown(report)

    if args.output:
        Path(args.output).write_text(output, encoding="utf-8")
        print(f"API changelog written to {args.output}")
        print(f"  {len(report.added)} added, {len(report.removed)} removed, "
              f"{len(report.modified)} modified")
    else:
        print(output)

    # Exit with non-zero if breaking changes exist (useful for CI)
    if report.has_breaking_changes:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
