import contextlib
import datetime as dt
import io
import json
import sys
import unittest
import unittest.mock
from pathlib import Path
from urllib import error

sys.path.insert(0, str(Path(__file__).resolve().parent))
import prune_expired_nightlies as prune  # noqa: E402

NOW = dt.datetime(2026, 9, 28, 12, 0, tzinfo=dt.timezone.utc)
REPOSITORY = "owner/repo"
BASE = f"https://api.github.com/repos/{REPOSITORY}/releases"


def release(release_id, tag, *, age_days, prerelease=True, draft=False, published_at=None):
    if published_at is None:
        published_at = (NOW - dt.timedelta(days=age_days)).isoformat().replace("+00:00", "Z")
    return {"id": release_id, "tag_name": tag, "prerelease": prerelease, "draft": draft,
            "published_at": published_at}


class FakeResponse:
    def __init__(self, status, payload):
        self.status = status
        self._raw = b"" if payload is None else json.dumps(payload).encode("utf-8")

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def read(self, limit):
        return self._raw[:limit]


class FakeGitHub:
    """Serves release pages and single releases; records every request."""

    def __init__(self, releases, *, current=None, fail_page=None):
        self.releases = releases
        self.current = current or {}
        self.fail_page = fail_page
        self.calls = []

    def __call__(self, api_request, timeout):
        method, url = api_request.get_method(), api_request.full_url
        self.calls.append((method, url))
        if method == "GET" and "?per_page=" in url:
            page = int(url.rsplit("page=", 1)[1])
            if page == self.fail_page:
                raise error.HTTPError(url, 502, "Bad Gateway", {}, None)
            size = prune.PAGE_SIZE
            return FakeResponse(200, self.releases[(page - 1) * size : page * size])
        release_id = int(url.rsplit("/", 1)[1])
        if method == "GET":
            by_id = {entry["id"]: entry for entry in self.releases}
            return FakeResponse(200, self.current.get(release_id, by_id[release_id]))
        if method == "DELETE":
            return FakeResponse(204, None)
        raise AssertionError(f"unexpected {method} {url}")

    def deletes(self):
        return [url for method, url in self.calls if method == "DELETE"]


def run(fake, *extra):
    stdout, stderr = io.StringIO(), io.StringIO()
    argv = ["--repository", REPOSITORY, "--now", NOW.isoformat(), *extra]
    with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
        with unittest.mock.patch.dict("os.environ", {"GH_TOKEN": "generated-test-token"}):
            code = prune.main(argv, opener=fake)
    return code, stdout.getvalue(), stderr.getvalue()


class SelectExpiredTests(unittest.TestCase):
    def test_only_unique_nightly_prereleases_past_30_days_are_selected(self):
        old = release(1, "nightly-12-1-0123456789ab", age_days=31)
        young = release(2, "nightly-13-1-0123456789ab", age_days=29)
        boundary = release(3, "nightly-14-2-0123456789ab", age_days=30)
        self.assertEqual(prune.select_expired([old, young, boundary], NOW), [old, boundary])

    def test_stable_draft_legacy_and_malformed_releases_are_never_selected(self):
        never = {
            "legacy rolling": release(1, "nightly", age_days=400),
            "stable": release(2, "v1.0.0", age_days=400, prerelease=False),
            "nightly tag not prerelease": release(3, "nightly-1-1-0123456789ab", age_days=400, prerelease=False),
            "draft nightly": release(4, "nightly-1-1-0123456789ab", age_days=400, draft=True),
            "zero run id": release(5, "nightly-0-1-0123456789ab", age_days=400),
            "uppercase sha": release(6, "nightly-1-1-0123456789AB", age_days=400),
            "missing published_at": {key: value for key, value in
                                     release(7, "nightly-1-1-0123456789ab", age_days=400).items()
                                     if key != "published_at"},
            "unparseable published_at": release(8, "nightly-1-1-0123456789ab", age_days=0, published_at="soon"),
            "naive published_at": release(9, "nightly-1-1-0123456789ab", age_days=0,
                                          published_at="2020-01-01T00:00:00"),
            "missing draft flag": {key: value for key, value in
                                   release(10, "nightly-1-1-0123456789ab", age_days=400).items() if key != "draft"},
        }
        for name, entry in never.items():
            with self.subTest(case=name):
                self.assertEqual(prune.select_expired([entry], NOW), [])


class PruneCommandTests(unittest.TestCase):
    def setUp(self):
        self.expired = release(101, "nightly-5-1-0123456789ab", age_days=45)
        self.kept = release(102, "nightly-6-1-0123456789ab", age_days=3)
        self.stable = release(103, "v1.0.0", age_days=300, prerelease=False)

    def test_dry_run_is_the_default_and_deletes_nothing(self):
        fake = FakeGitHub([self.expired, self.kept, self.stable])
        code, out, _ = run(fake)
        self.assertEqual(code, 0)
        self.assertIn("nightly-5-1-0123456789ab", out)
        self.assertIn("dry run: 1 nightly release(s) would be deleted", out)
        self.assertEqual(fake.deletes(), [])
        self.assertTrue(all(method == "GET" for method, _ in fake.calls))

    def test_apply_deletes_only_the_expired_release_after_rereading_it(self):
        fake = FakeGitHub([self.expired, self.kept, self.stable])
        code, out, _ = run(fake, "--apply")
        self.assertEqual(code, 0, out)
        self.assertEqual(fake.deletes(), [f"{BASE}/101"])
        self.assertLess(fake.calls.index(("GET", f"{BASE}/101")), fake.calls.index(("DELETE", f"{BASE}/101")))
        # The release is deleted through the releases API; no git ref is touched.
        self.assertFalse(any("/git/refs" in url for _, url in fake.calls))

    def test_apply_refuses_when_the_release_changed_since_listing(self):
        for field, value in (("tag_name", "nightly-5-2-0123456789ab"), ("prerelease", False),
                             ("published_at", "2026-09-27T00:00:00Z"), ("draft", True)):
            with self.subTest(field=field):
                fake = FakeGitHub([self.expired], current={101: {**self.expired, field: value}})
                code, _, err = run(fake, "--apply")
                self.assertEqual(code, 1)
                self.assertIn(field, err)
                self.assertEqual(fake.deletes(), [])

    def test_more_candidates_than_the_limit_fail_before_any_delete(self):
        many = [release(200 + index, f"nightly-{index + 1}-1-0123456789ab", age_days=40) for index in range(3)]
        for extra in ((), ("--apply",)):
            with self.subTest(extra=extra):
                fake = FakeGitHub(many)
                code, _, err = run(fake, "--max-deletions", "2", *extra)
                self.assertEqual(code, 1)
                self.assertIn("exceed --max-deletions 2", err)
                self.assertEqual(fake.deletes(), [])

    def test_a_failing_listing_page_exits_non_zero_without_deleting(self):
        first_page = [release(1000 + index, f"nightly-{index + 1}-1-0123456789ab", age_days=40)
                      for index in range(prune.PAGE_SIZE)]
        fake = FakeGitHub(first_page + [self.expired], fail_page=2)
        code, _, err = run(fake, "--apply", "--max-deletions", "500")
        self.assertEqual(code, 1)
        self.assertIn("HTTP 502", err)
        self.assertEqual(fake.deletes(), [])

    def test_a_failing_delete_exits_non_zero(self):
        class FailingDelete(FakeGitHub):
            def __call__(self, api_request, timeout):
                if api_request.get_method() == "DELETE":
                    self.calls.append(("DELETE", api_request.full_url))
                    raise error.HTTPError(api_request.full_url, 403, "Forbidden", {}, None)
                return super().__call__(api_request, timeout)

        second = release(104, "nightly-7-1-0123456789ab", age_days=60)
        fake = FailingDelete([self.expired, second])
        code, _, err = run(fake, "--apply")
        self.assertEqual(code, 1)
        self.assertIn("HTTP 403", err)
        self.assertEqual(len(fake.deletes()), 1)

    def test_missing_token_and_bad_now_fail_closed(self):
        fake = FakeGitHub([self.expired])
        with unittest.mock.patch.dict("os.environ", {"GH_TOKEN": ""}):
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(prune.main(["--repository", REPOSITORY], opener=fake), 1)
        with contextlib.redirect_stderr(io.StringIO()):
            code = prune.main(["--repository", REPOSITORY, "--now", "2026-09-28T00:00:00"], opener=fake)
        self.assertEqual(code, 1)
        self.assertEqual(fake.calls, [])


if __name__ == "__main__":
    unittest.main()
