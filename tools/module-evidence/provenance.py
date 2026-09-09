#!/usr/bin/env python3
"""Source-revision and timestamp provenance for module evidence.

The self-reference problem
--------------------------
A committed file cannot truthfully state the hash of the commit that contains
it: the hash is computed over the file's own bytes, so writing the hash in
changes the hash.  A `commitSHA` field inside a committed policy manifest is
therefore never evidence of anything — at best it names some *earlier* commit,
and any 40-hex string satisfies a shape check equally well.

The split this module enforces:

  * The **policy manifest** is committed and declares no revision at all.  It
    says what must be true, not what was observed.
  * A **run evidence** document is generated *after checkout*, is never
    committed, and records the exact revision and time at which the evidence
    artifacts were produced.
  * Validation binds the two: the run evidence's `commitSHA` must equal the
    revision under test, supplied by CI (`GITHUB_SHA`) or read from the
    checked-out repository, and must name a commit object that really exists.

Absent run evidence is a blocking gap, never a pass.
"""

from __future__ import annotations

import os
import re
import subprocess
from datetime import datetime, timedelta, timezone
from pathlib import Path

import strict_json

SHA_RE = re.compile(r"^[0-9a-f]{40}$")

# RFC 3339 section 5.6 date-time.  A trailing offset is mandatory: a naive
# timestamp cannot be ordered against anything and so proves no coherence.
RFC3339_RE = re.compile(
    r"^(?P<year>\d{4})-(?P<month>\d{2})-(?P<day>\d{2})"
    r"[Tt]"
    r"(?P<hour>\d{2}):(?P<minute>\d{2}):(?P<second>\d{2})"
    r"(?P<frac>\.\d+)?"
    r"(?P<offset>[Zz]|[+-]\d{2}:\d{2})$"
)

# Tolerance for clock skew between the machine that generated evidence and the
# machine validating it.  Generous enough not to flake, tight enough that a
# timestamp years in the future is still caught as incoherent.
MAX_FUTURE_SKEW = timedelta(hours=24)
EARLIEST_PLAUSIBLE = datetime(2020, 1, 1, tzinfo=timezone.utc)


def check_rfc3339(value: object, label: str, *, now: datetime | None = None) -> list[str]:
    """Validate an RFC3339 timestamp with a mandatory UTC offset."""
    if value is None:
        return [
            f"{label} is null — a missing timestamp is an absent fact, "
            f"not a satisfied one"
        ]
    if not isinstance(value, str) or not value:
        return [f"{label} must be a non-empty RFC3339 timestamp string, got {value!r}"]
    if not RFC3339_RE.match(value):
        return [
            f"{label} {value!r} is not an RFC3339 date-time with a mandatory "
            f"UTC offset (for example 2026-08-28T07:01:00Z)"
        ]
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00").replace("z", "+00:00"))
    except ValueError as exc:
        return [f"{label} {value!r} is not a real calendar instant: {exc}"]
    if parsed.tzinfo is None:
        return [f"{label} {value!r} has no timezone offset"]

    reference = now or datetime.now(timezone.utc)
    if parsed > reference + MAX_FUTURE_SKEW:
        return [
            f"{label} {value!r} is more than "
            f"{int(MAX_FUTURE_SKEW.total_seconds() // 3600)}h in the future — "
            f"evidence cannot predate its own production"
        ]
    if parsed < EARLIEST_PLAUSIBLE:
        return [
            f"{label} {value!r} predates {EARLIEST_PLAUSIBLE.date()} — "
            f"not a plausible evidence timestamp"
        ]
    return []


def check_sha_shape(value: object, label: str) -> list[str]:
    """Shape-only check.  Never sufficient on its own — see check_sha_exists."""
    if not isinstance(value, str) or not SHA_RE.match(value):
        return [
            f"{label} must be a 40-character lowercase hex commit SHA, got {value!r}"
        ]
    return []


def resolve_head_sha(repo_root: Path) -> tuple[str | None, str | None]:
    """Return (sha, error).  Reads the checked-out revision via git."""
    try:
        proc = subprocess.run(
            ["git", "-C", str(repo_root), "rev-parse", "HEAD"],
            capture_output=True, text=True, timeout=30, check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        return None, f"cannot run git to resolve HEAD: {exc}"
    if proc.returncode != 0:
        return None, f"git rev-parse HEAD failed: {proc.stderr.strip()}"
    sha = proc.stdout.strip()
    if not SHA_RE.match(sha):
        return None, f"git rev-parse HEAD returned an unexpected value: {sha!r}"
    return sha, None


def run_rooted_git(
    root: strict_json.NoFollowDirectoryLease, *arguments: str,
) -> subprocess.CompletedProcess[str]:
    """Run Git from an inherited POSIX descriptor-rooted checkout authority."""
    cwd, pass_fds = root.posix_git_cwd()
    # A relative cwd is not authority if Git's environment can replace the
    # repository, work tree, object store, index, or injected configuration.
    # Preserve ordinary process settings (PATH/locale/etc.) but remove every
    # Git-specific override and disable external system/global config files.
    environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith("GIT_")
    }
    environment["GIT_CONFIG_NOSYSTEM"] = "1"
    environment["GIT_CONFIG_GLOBAL"] = os.devnull
    # A checked-out release root must have a real .git directory below the held
    # root. A worktree-style .git file, commondir, or alternates file can
    # redirect object lookup outside that authority.
    git_entries = root.list_relative_names(".git")
    if "commondir" in git_entries:
        raise strict_json.NoFollowAuthorityError(
            "rooted Git rejects .git/commondir indirection"
        )
    if "objects" not in git_entries:
        raise strict_json.NoFollowAuthorityError(
            "rooted Git checkout has no .git/objects directory"
        )
    object_entries = root.list_relative_names(".git/objects")
    if "info" in object_entries:
        info_entries = root.list_relative_names(".git/objects/info")
        if "alternates" in info_entries:
            raise strict_json.NoFollowAuthorityError(
                "rooted Git rejects .git/objects/info/alternates indirection"
            )
    return subprocess.run(
        ["git", "-C", cwd, *arguments],
        capture_output=True, text=True, timeout=30, check=False,
        pass_fds=pass_fds, env=environment,
    )


def resolve_head_sha_rooted(
    root: strict_json.NoFollowDirectoryLease,
) -> tuple[str | None, str | None]:
    """Return HEAD through descriptor-rooted Git rather than a mutable path."""
    try:
        proc = run_rooted_git(root, "rev-parse", "HEAD")
    except (OSError, subprocess.SubprocessError, strict_json.NoFollowAuthorityError) as exc:
        return None, f"cannot run rooted git to resolve HEAD: {exc}"
    if proc.returncode != 0:
        return None, f"rooted git rev-parse HEAD failed: {proc.stderr.strip()}"
    sha = proc.stdout.strip()
    if not SHA_RE.match(sha):
        return None, f"rooted git rev-parse HEAD returned an unexpected value: {sha!r}"
    return sha, None


def check_sha_exists(sha: str, repo_root: Path, label: str) -> list[str]:
    """The SHA must name a commit object that really exists in this repository.

    This is what separates a real revision from an arbitrary 40-hex string:
    'a' * 40 has the right shape and names nothing.
    """
    try:
        proc = subprocess.run(
            ["git", "-C", str(repo_root), "cat-file", "-t", sha],
            capture_output=True, text=True, timeout=30, check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        return [f"{label}: cannot verify {sha!r} against the repository: {exc}"]
    if proc.returncode != 0:
        return [
            f"{label}: {sha!r} is not an object in this repository — an "
            f"arbitrary hex string is not evidence of a source revision"
        ]
    kind = proc.stdout.strip()
    if kind != "commit":
        return [f"{label}: {sha!r} is a {kind!r} object, not a commit"]
    return []


def check_sha_exists_rooted(
    sha: str, root: strict_json.NoFollowDirectoryLease, label: str,
) -> list[str]:
    """Verify a commit object through descriptor-rooted POSIX Git authority."""
    try:
        proc = run_rooted_git(root, "cat-file", "-t", sha)
    except (OSError, subprocess.SubprocessError, strict_json.NoFollowAuthorityError) as exc:
        return [f"{label}: cannot verify {sha!r} through rooted Git: {exc}"]
    if proc.returncode != 0:
        return [
            f"{label}: {sha!r} is not an object in the rooted repository — an "
            "arbitrary hex string is not evidence of a source revision"
        ]
    kind = proc.stdout.strip()
    if kind != "commit":
        return [f"{label}: {sha!r} is a {kind!r} object, not a commit"]
    return []


def check_revision_binding(
    declared_sha: object,
    expected_sha: str | None,
    repo_root: Path,
    label: str,
) -> list[str]:
    """Bind a run-evidence revision to the revision actually under test."""
    errors = check_sha_shape(declared_sha, label)
    if errors:
        return errors
    assert isinstance(declared_sha, str)
    errors += check_sha_exists(declared_sha, repo_root, label)
    if errors:
        return errors
    if expected_sha is None:
        return [
            f"{label}: no expected revision was supplied — pass --expected-sha "
            f"(CI supplies GITHUB_SHA) or run inside a git checkout so the "
            f"revision under test can be established. Evidence that is not "
            f"bound to a known revision is not evidence."
        ]
    if declared_sha != expected_sha:
        return [
            f"{label}: evidence was generated at {declared_sha} but the "
            f"revision under test is {expected_sha} — stale evidence cannot "
            f"stand in for the source being validated"
        ]
    return []


def check_revision_binding_rooted(
    declared_sha: object,
    expected_sha: str | None,
    root: strict_json.NoFollowDirectoryLease,
    label: str,
) -> list[str]:
    """Bind evidence revision using descriptor-rooted Git object authority."""
    errors = check_sha_shape(declared_sha, label)
    if errors:
        return errors
    assert isinstance(declared_sha, str)
    errors += check_sha_exists_rooted(declared_sha, root, label)
    if errors:
        return errors
    if expected_sha is None:
        return [
            f"{label}: no expected revision was supplied — pass --expected-sha "
            "so evidence is bound to a known revision"
        ]
    if declared_sha != expected_sha:
        return [
            f"{label}: evidence was generated at {declared_sha} but the "
            f"revision under test is {expected_sha} — stale evidence cannot "
            "stand in for the source being validated"
        ]
    return []
