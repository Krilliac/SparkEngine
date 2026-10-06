#!/usr/bin/env python3
"""Verify live immutable-release evidence before deploying a ready site bundle."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import sys
from urllib.parse import quote
from urllib.request import HTTPRedirectHandler, Request, build_opener

API_ROOT = "https://api.github.com"
PUBLICATION_JOB = "verify-stable-publication"
CI_REFERENCE = re.compile(r"ci:([A-Za-z0-9_.-]+)/([1-9][0-9]*)@([0-9a-f]{40})")
STABLE_TAG = re.compile(r"v[0-9]+\.[0-9]+\.[0-9]+")
SHA = re.compile(r"[0-9a-f]{40}")
MAX_RESPONSE_BYTES = 4 * 1024 * 1024
MAX_JOBS = 10000


class PublicationEvidenceError(RuntimeError):
    """Publication authority is absent, inconsistent, incomplete, or unsuccessful."""


def require(condition, message):
    if not condition:
        raise PublicationEvidenceError(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate GitHub metadata field: {key}")
        result[key] = value
    return result


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise PublicationEvidenceError("GitHub API redirect refused")


class GithubApi:
    def __init__(self, repository: str, token: str, *, opener=None, api_root: str = API_ROOT):
        require(re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository), "invalid repository")
        self.repository, self.token = repository, token
        self.opener = opener or build_opener(NoRedirect()).open
        self.api_root = api_root.rstrip("/")

    def get(self, path: str) -> dict:
        require(path.startswith(f"/repos/{self.repository}/") and "#" not in path, "invalid GitHub API path")
        request = Request(self.api_root + path, headers={
            "Accept": "application/vnd.github+json", "Authorization": f"Bearer {self.token}",
            "X-GitHub-Api-Version": "2022-11-28"})
        try:
            with self.opener(request, timeout=30) as response:
                body = response.read(MAX_RESPONSE_BYTES + 1)
            require(len(body) <= MAX_RESPONSE_BYTES, "GitHub API response exceeds the size bound")
            payload = json.loads(body.decode("utf-8"), object_pairs_hook=unique_object)
        except PublicationEvidenceError:
            raise
        except Exception as error:
            raise PublicationEvidenceError(f"GitHub API request failed ({type(error).__name__})") from None
        require(isinstance(payload, dict), "GitHub API returned a non-object")
        return payload


def publication_references(contract: dict) -> set[tuple[str, str, str]]:
    items = {item.get("id"): item for item in contract.get("workItems", []) if isinstance(item, dict)}
    references = set()
    profiles = contract.get("readiness", {}).get("releaseProfiles", [])
    require(isinstance(profiles, list) and profiles, "no declared release profiles")
    for profile in profiles:
        finalization = profile.get("publicationFinalization", {})
        finalizers = finalization.get("workItemIds", [])
        require(isinstance(finalizers, list) and finalizers, "profile has no publication finalizer")
        for item_id in finalizers:
            entries = items.get(item_id, {}).get("acceptanceStatus")
            require(isinstance(entries, list) and entries, f"{item_id}: no publication acceptance evidence")
            for entry in entries:
                require(isinstance(entry, dict) and entry.get("state") == "evidenced",
                        f"{item_id}: publication criterion is not evidenced")
                evidence = entry.get("evidence")
                require(isinstance(evidence, list), f"{item_id}: invalid publication evidence")
                entry_refs = set()
                for value in evidence:
                    match = CI_REFERENCE.fullmatch(value) if isinstance(value, str) else None
                    if isinstance(value, str) and value.startswith("ci:"):
                        require(match is not None, f"{item_id}: malformed CI reference")
                    if match and match.group(1) == "release.yml":
                        entry_refs.add(match.groups())
                require(entry_refs, f"{item_id}: criterion has no release.yml publication evidence")
                references.update(entry_refs)
    return references


def _tag_commit(api, tag_name):
    ref = api.get(f"/repos/{api.repository}/git/ref/tags/{quote(tag_name, safe='')}")
    require(ref.get("ref") == f"refs/tags/{tag_name}", "published tag identity drifted")
    obj = ref.get("object")
    for _ in range(4):
        require(isinstance(obj, dict) and isinstance(obj.get("sha"), str) and SHA.fullmatch(obj["sha"]),
                "published tag has no valid object SHA")
        if obj.get("type") == "commit":
            return obj["sha"]
        require(obj.get("type") == "tag", "published tag does not resolve to a commit")
        obj = api.get(f"/repos/{api.repository}/git/tags/{obj['sha']}").get("object")
    raise PublicationEvidenceError("published tag nesting exceeds the bound")


def publication_job(api, run_id, attempt, sha):
    jobs, total, seen = [], None, set()
    for page in range(1, MAX_JOBS // 100 + 1):
        payload = api.get(f"/repos/{api.repository}/actions/runs/{run_id}/attempts/{attempt}/jobs?per_page=100&page={page}")
        count, batch = payload.get("total_count"), payload.get("jobs")
        require(type(count) is int and 0 < count <= MAX_JOBS and isinstance(batch, list)
                and 0 < len(batch) <= 100, "incomplete or malformed job pagination")
        if total is None:
            total = count
        require(total == count, "job count changed during pagination")
        for job in batch:
            require(isinstance(job, dict) and type(job.get("id")) is int and job["id"] > 0
                    and job["id"] not in seen, "invalid or duplicate job identity")
            seen.add(job["id"])
            # The attempt is bound by the endpoint. GitHub does not promise a
            # run_attempt field on job objects; reject a conflicting one if sent.
            require(job.get("run_id") == int(run_id) and job.get("run_attempt", attempt) == attempt
                    and job.get("head_sha") == sha, "job run attempt or source identity drifted")
            jobs.append(job)
        require(len(jobs) <= total, "job count exceeds declared total")
        if len(jobs) == total:
            break
        require(len(batch) == 100, "incomplete job pagination")
    require(len(jobs) == total, "job pagination exceeds the bound")
    matches = [job for job in jobs if job.get("name") == PUBLICATION_JOB]
    require(len(matches) == 1 and matches[0].get("status") == "completed"
            and matches[0].get("conclusion") == "success",
            f"release run {run_id} lacks one successful {PUBLICATION_JOB} job")


def verify(contract: dict, source_sha: str, api: GithubApi) -> None:
    require(SHA.fullmatch(source_sha), "source SHA must be lowercase 40-hex")
    refs = publication_references(contract)
    commits = {sha for _, _, sha in refs}
    require(len(commits) == 1, "publication evidence cites different commits")
    for _, run_id, sha in sorted(refs):
        run = api.get(f"/repos/{api.repository}/actions/runs/{run_id}")
        require(run.get("id") == int(run_id) and isinstance(run.get("path"), str)
                and run["path"].split("@", 1)[0] == ".github/workflows/release.yml",
                f"release run {run_id} identity or workflow differs")
        require(run.get("status") == "completed" and run.get("conclusion") == "success"
                and run.get("head_sha") == sha, f"release run {run_id} did not succeed at its cited SHA")
        attempt = run.get("run_attempt")
        require(type(attempt) is int and attempt > 0, "release run has no valid attempt")
        publication_job(api, run_id, attempt, sha)
    release = api.get(f"/repos/{api.repository}/releases/latest")
    tag_name = release.get("tag_name")
    require(release.get("draft") is False and release.get("prerelease") is False
            and release.get("immutable") is True and isinstance(tag_name, str) and STABLE_TAG.fullmatch(tag_name),
            "latest published release is not an immutable stable version tag")
    tag_sha = _tag_commit(api, tag_name)
    require(commits == {tag_sha}, "publication evidence is not bound to the published stable tag")
    comparison = api.get(f"/repos/{api.repository}/compare/{tag_sha}...{source_sha}")
    require(comparison.get("status") in {"identical", "ahead"}
            and comparison.get("head_commit", {}).get("sha") == source_sha
            and comparison.get("merge_base_commit", {}).get("sha") == tag_sha,
            "publication SHA is not an ancestor of SOURCE_COMMIT")


def load_repository_contract():
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/site-data"))
    from common import load_contract
    return load_contract()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--source-sha", required=True)
    args = parser.parse_args(argv)
    try:
        verify(load_repository_contract(), args.source_sha,
               GithubApi(args.repository, os.environ.get("GITHUB_TOKEN", "")))
    except (OSError, ValueError, PublicationEvidenceError) as error:
        print(f"verify-publication-evidence: {error}", file=sys.stderr)
        return 1
    print(f"verify-publication-evidence: {args.repository} {args.source_sha} verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
