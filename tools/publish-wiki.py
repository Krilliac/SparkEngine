#!/usr/bin/env python3
"""Build the flat, GitHub Wiki-compatible SparkEngine documentation tree.

The repository's ``wiki/`` directory is the canonical authored source and keeps
category subdirectories for maintainability. GitHub Wiki is a separate git
repository and addresses pages by filename, so publication flattens the source
tree and rewrites repository-relative links for Gollum.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath
from urllib.parse import unquote, urlsplit


BRANCH = "Working"
REPO_ROOT = Path(__file__).resolve().parents[1]
WIKI_ROOT = REPO_ROOT / "wiki"
REPOSITORY_RE = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
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
FENCE_OPEN_RE = re.compile(r"^[ \t]{0,3}(?P<fence>`{3,}|~{3,})")


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


def parse_repository(value: str) -> str:
    """Return an owner/name slug from a slug or common Git remote URL."""
    candidate = value.strip().removesuffix(".git")
    if REPOSITORY_RE.fullmatch(candidate):
        return candidate

    if candidate.startswith("git@github.com:"):
        candidate = candidate.removeprefix("git@github.com:")
    else:
        parsed = urlsplit(candidate)
        if parsed.hostname != "github.com":
            raise PublishError(f"cannot derive GitHub repository from: {value}")
        candidate = parsed.path.strip("/")

    if not REPOSITORY_RE.fullmatch(candidate):
        raise PublishError(f"invalid GitHub repository: {value}")
    return candidate


def repository_name(explicit: str | None = None) -> str:
    if explicit:
        return parse_repository(explicit)
    if environment := os.environ.get("GITHUB_REPOSITORY"):
        return parse_repository(environment)
    return parse_repository(git_output("config", "--get", "remote.origin.url"))


def path_uses_symlink(path: Path, root: Path) -> bool:
    """Check every existing path component below root without following it."""
    try:
        relative = path.absolute().relative_to(root.absolute())
    except ValueError:
        return False
    current = root.absolute()
    if current.is_symlink():
        return True
    for component in relative.parts:
        current /= component
        if current.is_symlink():
            return True
    return False


def source_pages() -> list[Path]:
    if not WIKI_ROOT.is_dir() or WIKI_ROOT.is_symlink():
        raise PublishError(f"wiki source must be a real directory: {WIKI_ROOT}")
    pages = sorted(
        path
        for path in WIKI_ROOT.rglob("*.md")
        if path.name != "_Template.md"
    )
    if not pages:
        raise PublishError("wiki/ contains no publishable Markdown pages")

    by_name: dict[str, Path] = {}
    for path in pages:
        if not path.is_file() or path_uses_symlink(path, WIKI_ROOT):
            raise PublishError(f"wiki source must not use symlinks: {path}")
        if not inside(path.resolve(), WIKI_ROOT.resolve()):
            raise PublishError(f"wiki source escapes wiki root: {path}")
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


def absolute_repository_url(
    path: Path, *, image: bool, repository: str, revision: str
) -> str:
    relative = path.relative_to(REPO_ROOT).as_posix()
    if image:
        return f"https://raw.githubusercontent.com/{repository}/{revision}/{relative}"
    return f"https://github.com/{repository}/blob/{revision}/{relative}"


def pin_repository_url(target: str, repository: str, revision: str) -> str:
    """Pin authored links to this repository's mutable branch to the source SHA."""
    quoted_repository = re.escape(repository)
    github_pattern = re.compile(
        rf"^https://github\.com/{quoted_repository}/(?P<kind>blob|tree)/{BRANCH}(?P<rest>/.*)?$"
    )
    raw_pattern = re.compile(
        rf"^https://raw\.githubusercontent\.com/{quoted_repository}/{BRANCH}(?P<rest>/.*)?$"
    )
    if match := github_pattern.match(target):
        return (
            f"https://github.com/{repository}/{match.group('kind')}/{revision}"
            f"{match.group('rest') or ''}"
        )
    if match := raw_pattern.match(target):
        return (
            f"https://raw.githubusercontent.com/{repository}/{revision}"
            f"{match.group('rest') or ''}"
        )
    return target


def rewrite_target(
    raw_target: str,
    source: Path,
    *,
    image: bool,
    repository: str,
    revision: str,
) -> str:
    wrapped = raw_target.startswith("<") and raw_target.endswith(">")
    target = raw_target[1:-1] if wrapped else raw_target
    target = pin_repository_url(target, repository, revision)
    if not target or target.startswith(("#", "//")) or SCHEME_RE.match(target):
        return f"<{target}>" if wrapped else target

    target_path, fragment = split_fragment(target)
    decoded = unquote(target_path)
    if not decoded or decoded.startswith("/"):
        return raw_target

    unresolved = source.parent / PurePosixPath(decoded)
    resolved = unresolved.resolve()
    if inside(resolved, WIKI_ROOT) and resolved.suffix.lower() == ".md":
        if path_uses_symlink(unresolved, WIKI_ROOT):
            raise PublishError(f"wiki link traverses a symlink: {source} -> {target_path}")
        rewritten = resolved.stem + fragment
    elif inside(resolved, REPO_ROOT) and resolved.exists():
        if path_uses_symlink(unresolved, REPO_ROOT):
            raise PublishError(f"repository link traverses a symlink: {source} -> {target_path}")
        if not resolved.is_file() and not resolved.is_dir():
            raise PublishError(f"repository link is not a regular path: {source} -> {target_path}")
        rewritten = absolute_repository_url(
            resolved, image=image, repository=repository, revision=revision
        ) + fragment
    else:
        return raw_target
    return f"<{rewritten}>" if wrapped else rewritten


def rewrite_markdown(
    content: str, source: Path, *, repository: str, revision: str
) -> str:
    def inline(match: re.Match[str]) -> str:
        image = match.group("prefix").startswith("!")
        target = rewrite_target(
            match.group("target"),
            source,
            image=image,
            repository=repository,
            revision=revision,
        )
        return (
            match.group("prefix")
            + target
            + (match.group("title") or "")
            + match.group("suffix")
        )

    def reference(match: re.Match[str]) -> str:
        target = rewrite_target(
            match.group("target"),
            source,
            image=False,
            repository=repository,
            revision=revision,
        )
        return match.group("prefix") + target + (match.group("title") or "")

    def html_source(match: re.Match[str]) -> str:
        image = match.group("prefix").lower().startswith("src")
        target = rewrite_target(
            match.group("target"),
            source,
            image=image,
            repository=repository,
            revision=revision,
        )
        return match.group("prefix") + target + match.group("suffix")

    rewritten: list[str] = []
    fence_character = ""
    fence_length = 0
    for line in content.splitlines():
        fence = FENCE_OPEN_RE.match(line)
        if fence_character:
            rewritten.append(line)
            if (
                fence
                and fence.group("fence")[0] == fence_character
                and len(fence.group("fence")) >= fence_length
                and not line[fence.end() :].strip()
            ):
                fence_character = ""
                fence_length = 0
            continue
        if fence:
            marker = fence.group("fence")
            fence_character = marker[0]
            fence_length = len(marker)
            rewritten.append(line)
            continue

        updated = INLINE_LINK_RE.sub(inline, line)
        updated = REFERENCE_LINK_RE.sub(reference, updated)
        updated = HTML_SOURCE_RE.sub(html_source, updated)
        rewritten.append(updated)
    return "\n".join(rewritten).rstrip() + "\n"


def internal_targets(content: str) -> list[str]:
    targets: list[str] = []
    fence_character = ""
    fence_length = 0
    for line in content.splitlines():
        fence = FENCE_OPEN_RE.match(line)
        if fence_character:
            if (
                fence
                and fence.group("fence")[0] == fence_character
                and len(fence.group("fence")) >= fence_length
                and not line[fence.end() :].strip()
            ):
                fence_character = ""
                fence_length = 0
            continue
        if fence:
            marker = fence.group("fence")
            fence_character = marker[0]
            fence_length = len(marker)
            continue

        for match in INLINE_LINK_RE.finditer(line):
            target = match.group("target").strip("<>")
            if target and not target.startswith(("#", "//")) and not SCHEME_RE.match(target):
                targets.append(split_fragment(target)[0])
        if match := REFERENCE_LINK_RE.match(line):
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


def prepare(output: Path, *, repository: str) -> None:
    requested_output = output.absolute()
    if requested_output.is_symlink():
        raise PublishError(f"refusing symlink output directory: {requested_output}")
    output = requested_output.resolve()
    if output == REPO_ROOT or inside(output, REPO_ROOT) or inside(REPO_ROOT, output):
        raise PublishError(f"refusing unsafe output directory: {output}")
    if output.exists() and any(output.iterdir()):
        raise PublishError(f"output directory must be empty: {output}")
    output.mkdir(parents=True, exist_ok=True)

    pages = source_pages()
    revision = git_output("rev-parse", "--verify", "HEAD^{commit}")
    for source in pages:
        destination = output / source.name
        content = source.read_text(encoding="utf-8")
        destination.write_text(
            rewrite_markdown(
                content, source, repository=repository, revision=revision
            ),
            encoding="utf-8",
        )

    footer = (
        "---\n"
        f"Published from [`{revision[:12]}`](https://github.com/{repository}/commit/{revision}). "
        f"Edit the canonical source in [`wiki/`](https://github.com/{repository}/tree/{revision}/wiki).\n"
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
    parser.add_argument(
        "--repository",
        help="GitHub owner/name (defaults to GITHUB_REPOSITORY or origin)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        repository = repository_name(args.repository)
        if args.check:
            with tempfile.TemporaryDirectory(prefix="sparkengine-wiki-") as directory:
                prepare(Path(directory) / "published", repository=repository)
        else:
            prepare(args.output, repository=repository)
    except (OSError, PublishError) as error:
        print(f"wiki publication failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
