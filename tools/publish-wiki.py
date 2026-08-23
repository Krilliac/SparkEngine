#!/usr/bin/env python3
"""Build the flat, GitHub Wiki-compatible SparkEngine documentation tree.

The repository's ``wiki/`` directory is the canonical authored source and keeps
category subdirectories for maintainability. GitHub Wiki is a separate git
repository and addresses pages by filename, so publication flattens the source
tree and rewrites repository-relative links for Gollum.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath
from urllib.parse import unquote


REPOSITORY = "Krilliac/SparkEngine"
BRANCH = "Working"
REPO_ROOT = Path(__file__).resolve().parents[1]
WIKI_ROOT = REPO_ROOT / "wiki"
STABLE_SITE_PAGES = {
    "Home",
    "Documentation",
    "Docs",
    "Wiki",
    "Getting-Started",
    "Guides",
    "Tutorials",
    "Samples",
    "Examples",
    "API-Reference",
    "API",
    "Reference",
    "Build-Guide",
    "Dependencies",
    "FAQ",
    "Changelog",
    "Roadmap",
    "Contributing",
    "Code-of-Conduct",
}

INLINE_LINK_RE = re.compile(
    r"(?P<prefix>!?\[[^\]]*\]\()"
    r"(?P<target><[^>]+>|[^)\s]+)"
    r"(?P<title>\s+(?:\"[^\"]*\"|'[^']*'))?"
    r"(?P<suffix>\))"
)
REFERENCE_LINK_RE = re.compile(
    r"^(?P<prefix>\s*\[[^\]]+\]:\s*)"
    r"(?P<target><[^>]+>|\S+)"
    r"(?P<title>\s+(?:\"[^\"]*\"|'[^']*'|\([^)]*\)))?\s*$"
)
HTML_SOURCE_RE = re.compile(
    r"(?P<prefix>\b(?:src|href)=['\"])(?P<target>[^'\"]+)(?P<suffix>['\"])"
)
SCHEME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*:")


class PublishError(RuntimeError):
    """Raised when the authored wiki cannot be published safely."""


def git_output(*args: str) -> str:
    result = subprocess.run(
        ["git", *args],
        cwd=REPO_ROOT,
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode:
        raise PublishError(result.stderr.strip() or "git command failed")
    return result.stdout.strip()


def source_pages() -> list[Path]:
    pages = sorted(
        path
        for path in WIKI_ROOT.rglob("*.md")
        if path.name != "_Template.md"
    )
    if not pages:
        raise PublishError("wiki/ contains no publishable Markdown pages")

    by_name: dict[str, Path] = {}
    for path in pages:
        folded = path.name.casefold()
        if folded in by_name:
            first = by_name[folded].relative_to(REPO_ROOT)
            second = path.relative_to(REPO_ROOT)
            raise PublishError(
                f"GitHub Wiki page-name collision: {first} and {second}"
            )
        by_name[folded] = path
    return pages


def split_fragment(target: str) -> tuple[str, str]:
    if "#" not in target:
        return target, ""
    path, fragment = target.split("#", 1)
    return path, f"#{fragment}"


def inside(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def absolute_repository_url(path: Path, *, image: bool) -> str:
    relative = path.relative_to(REPO_ROOT).as_posix()
    if image:
        return f"https://raw.githubusercontent.com/{REPOSITORY}/{BRANCH}/{relative}"
    return f"https://github.com/{REPOSITORY}/blob/{BRANCH}/{relative}"


def rewrite_target(raw_target: str, source: Path, *, image: bool) -> str:
    wrapped = raw_target.startswith("<") and raw_target.endswith(">")
    target = raw_target[1:-1] if wrapped else raw_target
    if not target or target.startswith(("#", "//")) or SCHEME_RE.match(target):
        return raw_target

    target_path, fragment = split_fragment(target)
    decoded = unquote(target_path)
    if not decoded or decoded.startswith("/"):
        return raw_target

    resolved = (source.parent / PurePosixPath(decoded)).resolve()
    if inside(resolved, WIKI_ROOT) and resolved.suffix.lower() == ".md":
        rewritten = resolved.stem + fragment
    elif inside(resolved, REPO_ROOT) and resolved.exists():
        rewritten = absolute_repository_url(resolved, image=image) + fragment
    else:
        return raw_target
    return f"<{rewritten}>" if wrapped else rewritten


def rewrite_markdown(content: str, source: Path) -> str:
    def inline(match: re.Match[str]) -> str:
        image = match.group("prefix").startswith("!")
        target = rewrite_target(match.group("target"), source, image=image)
        return (
            match.group("prefix")
            + target
            + (match.group("title") or "")
            + match.group("suffix")
        )

    def reference(match: re.Match[str]) -> str:
        target = rewrite_target(match.group("target"), source, image=False)
        return match.group("prefix") + target + (match.group("title") or "")

    def html_source(match: re.Match[str]) -> str:
        image = match.group("prefix").lower().startswith("src")
        target = rewrite_target(match.group("target"), source, image=image)
        return match.group("prefix") + target + match.group("suffix")

    rewritten = INLINE_LINK_RE.sub(inline, content)
    rewritten = "\n".join(
        REFERENCE_LINK_RE.sub(reference, line) for line in rewritten.splitlines()
    )
    rewritten = HTML_SOURCE_RE.sub(html_source, rewritten)
    return rewritten.rstrip() + "\n"


def internal_targets(content: str) -> list[str]:
    targets: list[str] = []
    for match in INLINE_LINK_RE.finditer(content):
        target = match.group("target").strip("<>")
        if target and not target.startswith(("#", "//")) and not SCHEME_RE.match(target):
            targets.append(split_fragment(target)[0])
    for line in content.splitlines():
        match = REFERENCE_LINK_RE.match(line)
        if not match:
            continue
        target = match.group("target").strip("<>")
        if target and not target.startswith(("#", "//")) and not SCHEME_RE.match(target):
            targets.append(split_fragment(target)[0])
    return targets


def validate_output(output: Path, expected_pages: int) -> None:
    markdown = sorted(output.glob("*.md"))
    page_names = {path.stem for path in markdown}
    missing_stable = sorted(STABLE_SITE_PAGES - page_names)
    if missing_stable:
        raise PublishError(
            "missing stable website wiki pages: " + ", ".join(missing_stable)
        )
    if len(markdown) != expected_pages + 1:  # generated _Footer.md
        raise PublishError(
            f"expected {expected_pages + 1} published pages, found {len(markdown)}"
        )

    broken: list[str] = []
    for page in markdown:
        content = page.read_text(encoding="utf-8")
        for target in internal_targets(content):
            normalized = Path(target).stem
            if normalized not in page_names:
                broken.append(f"{page.name} -> {target}")
    if broken:
        raise PublishError("broken internal wiki links:\n  " + "\n  ".join(broken[:40]))


def prepare(output: Path) -> None:
    output = output.resolve()
    if output == REPO_ROOT or inside(output, REPO_ROOT) or inside(REPO_ROOT, output):
        raise PublishError(f"refusing unsafe output directory: {output}")
    if output.exists() and any(output.iterdir()):
        raise PublishError(f"output directory must be empty: {output}")
    output.mkdir(parents=True, exist_ok=True)

    pages = source_pages()
    for source in pages:
        destination = output / source.name
        content = source.read_text(encoding="utf-8")
        destination.write_text(rewrite_markdown(content, source), encoding="utf-8")

    revision = git_output("rev-parse", "HEAD")
    footer = (
        "---\n"
        f"Published from [`{revision[:12]}`](https://github.com/{REPOSITORY}/commit/{revision}). "
        f"Edit the canonical source in [`wiki/`](https://github.com/{REPOSITORY}/tree/{BRANCH}/wiki).\n"
    )
    (output / "_Footer.md").write_text(footer, encoding="utf-8")
    validate_output(output, len(pages))
    print(f"Prepared {len(pages)} wiki pages at {output}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--output", type=Path, help="empty directory to populate")
    group.add_argument(
        "--check",
        action="store_true",
        help="build and validate in a temporary directory",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        if args.check:
            with tempfile.TemporaryDirectory(prefix="sparkengine-wiki-") as directory:
                prepare(Path(directory) / "published")
        else:
            prepare(args.output)
    except (OSError, PublishError) as error:
        print(f"wiki publication failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
