#!/usr/bin/env python3
"""Select, and on request delete, nightly prereleases past their retention window.

Owner decision OD-17 (docs/readiness/OWNER-DECISIONS.md) keeps nightly
prereleases for 30 days. A release is expired only when all of these hold:
its tag is a unique ``nightly-<run>-<attempt>-<sha12>`` tag (the historical
rolling ``nightly`` tag never matches), it is a published prerelease, and it
was published at least 30 days before ``now``. Stable releases, drafts and
anything with a malformed tag or timestamp are never selected.

The default is a dry run that prints the plan. ``--apply`` deletes each
selected release after re-reading it and confirming its identity is
unchanged. Only the release is deleted; the git tag stays, which preserves
provenance and keeps the tag from being reused. Any API error exits
non-zero; nothing continues past one.

Usage: GH_TOKEN=... prune_expired_nightlies.py --repository owner/name [--apply]
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import sys
from pathlib import Path
from typing import Any, Callable, Iterable
from urllib import error, request

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nightly_release_tag import TAG_RE  # noqa: E402

RETENTION_DAYS = 30
DEFAULT_MAX_DELETIONS = 60
PAGE_SIZE = 100
MAX_PAGES = 50
MAX_RESPONSE_BYTES = 16 * 1024 * 1024
API_VERSION = "2022-11-28"
REPOSITORY_RE = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+")

Opener = Callable[..., Any]


class PruneError(RuntimeError):
    pass


def parse_timestamp(value: Any) -> dt.datetime | None:
    """An ISO-8601 timestamp with an explicit offset, or None."""
    if not isinstance(value, str):
        return None
    try:
        parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        return None
    return parsed if parsed.tzinfo is not None else None


def is_expired(release: dict[str, Any], now: dt.datetime, days: int = RETENTION_DAYS) -> bool:
    tag = release.get("tag_name")
    if not isinstance(tag, str) or TAG_RE.fullmatch(tag) is None:
        return False
    if release.get("prerelease") is not True or release.get("draft") is not False:
        return False
    if not isinstance(release.get("id"), int):
        return False
    published = parse_timestamp(release.get("published_at"))
    return published is not None and published + dt.timedelta(days=days) <= now


def select_expired(
    releases: Iterable[dict[str, Any]], now: dt.datetime, days: int = RETENTION_DAYS
) -> list[dict[str, Any]]:
    return [release for release in releases if isinstance(release, dict) and is_expired(release, now, days)]


class GitHubReleases:
    """The three release API calls the pruner needs; every failure raises PruneError."""

    def __init__(self, repository: str, token: str, *, opener: Opener = request.urlopen, timeout: float = 30.0):
        if REPOSITORY_RE.fullmatch(repository) is None:
            raise PruneError("repository must be owner/name")
        if not token:
            raise PruneError("GH_TOKEN is missing")
        self._base = f"https://api.github.com/repos/{repository}/releases"
        self._token = token
        self._opener = opener
        self._timeout = timeout

    def _call(self, method: str, url: str) -> Any:
        headers = {
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {self._token}",
            "User-Agent": "SparkEngine-nightly-retention",
            "X-GitHub-Api-Version": API_VERSION,
        }
        api_request = request.Request(url, headers=headers, method=method)
        try:
            with self._opener(api_request, timeout=self._timeout) as response:
                status = getattr(response, "status", 200)
                raw = response.read(MAX_RESPONSE_BYTES + 1)
        except error.HTTPError as exc:
            raise PruneError(f"{method} {url} returned HTTP {exc.code}") from exc
        except error.URLError as exc:
            raise PruneError(f"{method} {url} failed: {exc.reason}") from exc
        if method == "DELETE":
            if status != 204:
                raise PruneError(f"DELETE {url} returned HTTP {status}, expected 204")
            return None
        if len(raw) > MAX_RESPONSE_BYTES:
            raise PruneError(f"{method} {url} response is unexpectedly large")
        try:
            return json.loads(raw)
        except (UnicodeError, json.JSONDecodeError) as exc:
            raise PruneError(f"{method} {url} returned invalid JSON") from exc

    def list_releases(self) -> list[dict[str, Any]]:
        releases: list[dict[str, Any]] = []
        for page in range(1, MAX_PAGES + 1):
            batch = self._call("GET", f"{self._base}?per_page={PAGE_SIZE}&page={page}")
            if not isinstance(batch, list):
                raise PruneError(f"release listing page {page} is not a list")
            releases.extend(batch)
            if len(batch) < PAGE_SIZE:
                return releases
        raise PruneError(f"release listing exceeds {MAX_PAGES} pages")

    def get_release(self, release_id: int) -> dict[str, Any]:
        value = self._call("GET", f"{self._base}/{release_id}")
        if not isinstance(value, dict):
            raise PruneError(f"release {release_id} is not an object")
        return value

    def delete_release(self, release_id: int) -> None:
        self._call("DELETE", f"{self._base}/{release_id}")


IDENTITY_FIELDS = ("id", "tag_name", "prerelease", "draft", "published_at")


def prune(
    api: GitHubReleases, now: dt.datetime, *, apply: bool, max_deletions: int, days: int = RETENTION_DAYS
) -> list[dict[str, Any]]:
    """Plan (and with apply, perform) deletions; returns the selected releases."""
    expired = select_expired(api.list_releases(), now, days)
    for release in expired:
        print(f"expired: {release['tag_name']} (release {release['id']}, published {release['published_at']})")
    if len(expired) > max_deletions:
        raise PruneError(f"{len(expired)} expired nightlies exceed --max-deletions {max_deletions}; nothing deleted")
    if not apply:
        print(f"dry run: {len(expired)} nightly release(s) would be deleted; pass --apply to delete them")
        return expired
    for release in expired:
        current = api.get_release(release["id"])
        changed = [field for field in IDENTITY_FIELDS if current.get(field) != release.get(field)]
        if changed:
            raise PruneError(f"release {release['id']} changed since listing ({', '.join(changed)}); not deleted")
        api.delete_release(release["id"])
        print(f"deleted: {release['tag_name']} (git tag kept)")
    return expired


def main(argv: list[str] | None = None, *, opener: Opener = request.urlopen) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--repository", required=True)
    parser.add_argument("--now", help="ISO-8601 time with offset (tests); defaults to the current UTC time")
    parser.add_argument("--apply", action="store_true", help="delete the selected releases (default: dry run)")
    parser.add_argument("--max-deletions", type=int, default=DEFAULT_MAX_DELETIONS)
    args = parser.parse_args(argv)
    try:
        if args.max_deletions < 0:
            raise PruneError("--max-deletions must not be negative")
        now = dt.datetime.now(dt.timezone.utc) if args.now is None else parse_timestamp(args.now)
        if now is None:
            raise PruneError("--now must be an ISO-8601 time with an offset")
        api = GitHubReleases(args.repository, os.environ.get("GH_TOKEN", ""), opener=opener)
        prune(api, now, apply=args.apply, max_deletions=args.max_deletions)
    except PruneError as exc:
        print(f"nightly retention failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
