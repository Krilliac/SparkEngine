#!/usr/bin/env python3
"""Stage only a frozen draft, checking channel policy before every API write."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import quote

from guard_release_mutation import guarded_run
from verify_release_environment import unique_object


NIGHTLY_TAG_RE = re.compile(r"nightly-[1-9][0-9]*-[1-9][0-9]*-[0-9a-f]{12}")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def file_digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return "sha256:" + result.hexdigest()


class DraftApi:
    def __init__(self, repository, is_versioned):
        self.repository = repository
        self.is_versioned = is_versioned

    def request(self, endpoint, *, method="GET", payload=None, upload=None):
        command = ["gh", "api"]
        body = None
        if method != "GET":
            command += ["--method", method]
        command.append(endpoint)
        if payload is not None:
            command += ["--input", "-"]
            body = json.dumps(payload).encode("utf-8")
        if upload is not None:
            command += ["-H", "Content-Type: application/octet-stream", "--input", str(upload)]
        if method == "GET":
            result = subprocess.run(command, capture_output=True, check=False, timeout=60)
        else:
            result = guarded_run(command, self.repository, self.is_versioned, input=body, capture_output=True)
        require(result.returncode == 0, f"draft API {method} failed; no publication was attempted")
        require(len(result.stdout) <= 4 * 1024 * 1024, "draft API response exceeds its bound")
        return json.loads(result.stdout, object_pairs_hook=unique_object) if result.stdout.strip() else None


def validate_draft(record, expected_id, tag, is_versioned):
    require(isinstance(record, dict) and type(record.get("id")) is int and record["id"] > 0,
            "draft has no exact release id")
    require(not expected_id or record["id"] == expected_id, "draft id differs from the frozen target")
    require(record.get("tag_name") == tag and record.get("prerelease") is (not is_versioned)
            and record.get("draft") is True and record.get("immutable") is False,
            "staging target must be the exact mutable draft and channel")
    return record["id"]


def stage(*, repository, tag, source_sha, is_versioned, expected_id, title, body, assets_file,
          root, api=None):
    require(re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository), "invalid repository")
    require(re.fullmatch(r"[0-9a-f]{40}", source_sha), "invalid source SHA")
    require((is_versioned and re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", tag))
            or (not is_versioned and NIGHTLY_TAG_RE.fullmatch(tag)),
            "invalid channel tag; rolling nightly tags must be unique immutable tags")
    require(type(expected_id) is int and expected_id >= 0, "invalid frozen release id")
    require(assets_file.stat().st_size <= 32768, "asset inventory is too large")
    names = assets_file.read_text(encoding="utf-8").splitlines()
    require(names and len(names) <= 100 and len({name.casefold() for name in names}) == len(names),
            "asset inventory must be nonempty, unique, and bounded")
    payloads = {}
    for name in names:
        require(re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", name), "asset must be one safe flat name")
        path = root / name
        require(path.is_file() and not path.is_symlink(), "asset is missing or link-like")
        payloads[name] = (path, file_digest(path), path.stat().st_size)
    api = api or DraftApi(repository, is_versioned)
    endpoint = f"repos/{repository}/releases"
    metadata = {"tag_name": tag, "target_commitish": source_sha, "name": title, "body": body,
                "draft": True, "prerelease": not is_versioned, "make_latest": "false"}
    if expected_id:
        validate_draft(api.request(f"{endpoint}/{expected_id}"), expected_id, tag, is_versioned)
        # Existing identity, channel and visibility are frozen by the ledger.
        # Never restore those fields over a concurrent external change.
        record = api.request(f"{endpoint}/{expected_id}", method="PATCH",
                             payload={"name": title, "body": body})
    else:
        # POST cannot overwrite a concurrently created release with this tag.
        record = api.request(endpoint, method="POST", payload=metadata)
    release_id = validate_draft(record, expected_id, tag, is_versioned)
    existing = api.request(f"{endpoint}/{release_id}/assets?per_page=100")
    require(isinstance(existing, list) and len(existing) < 100, "existing asset inventory is unbounded")
    by_name = {}
    for entry in existing:
        require(isinstance(entry, dict) and isinstance(entry.get("name"), str)
                and entry["name"] not in by_name and type(entry.get("id")) is int and entry["id"] > 0,
                "existing asset identity is malformed or duplicated")
        require(entry["name"] in payloads, "draft contains an asset outside the frozen inventory")
        by_name[entry["name"]] = entry
    for name, (path, digest, size) in payloads.items():
        old = by_name.get(name)
        if old and old.get("digest") == digest and old.get("size") == size and old.get("state") == "uploaded":
            continue
        # Recheck draft identity before each destructive/upload phase, then the
        # API adapter rechecks policy immediately before the individual write.
        validate_draft(api.request(f"{endpoint}/{release_id}"), release_id, tag, is_versioned)
        if old:
            require(not is_versioned, "stable draft assets must never be overwritten by staging")
            api.request(f"{endpoint}/assets/{old['id']}", method="DELETE")
            validate_draft(api.request(f"{endpoint}/{release_id}"), release_id, tag, is_versioned)
        uploaded = api.request(f"https://uploads.github.com/{endpoint}/{release_id}/assets?name={quote(name, safe='')}",
                               method="POST", upload=path)
        require(isinstance(uploaded, dict) and uploaded.get("name") == name
                and uploaded.get("state") == "uploaded" and uploaded.get("digest") == digest
                and uploaded.get("size") == size, "uploaded asset does not match the frozen local bytes")
    validate_draft(api.request(f"{endpoint}/{release_id}"), release_id, tag, is_versioned)
    return release_id


def main():
    try:
        channel = os.environ.get("IS_VERSIONED")
        require(channel in {"true", "false"}, "explicit release channel is required")
        release_id = stage(
            repository=os.environ["GITHUB_REPOSITORY"], tag=os.environ["RELEASE_TAG"],
            source_sha=os.environ["GITHUB_SHA"], is_versioned=channel == "true",
            expected_id=int(os.environ.get("EXPECTED_RELEASE_ID") or "0"),
            title=os.environ["RELEASE_TITLE"], body=os.environ["RELEASE_BODY"],
            assets_file=Path("expected-release-assets.txt"), root=Path.cwd(),
        )
        with Path(os.environ["GITHUB_OUTPUT"]).open("a", encoding="utf-8") as output:
            output.write(f"id={release_id}\n")
    except (ValueError, KeyError, OSError, subprocess.SubprocessError) as error:
        # Do not render CalledProcessError: command arguments can carry credentials.
        print(f"draft staging failed: {error}" if isinstance(error, ValueError)
              else "draft staging failed before completion; inspect API diagnostics", file=sys.stderr)
        return 1
    print(f"Staged exact draft release {release_id}; nothing was published")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
