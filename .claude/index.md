# Persistence Context — Index

_Read this at every session start (after git sync). Each row links to a detailed knowledge file. If a topic matches your current task, read the full file before proceeding._

| Topic | File | Status | Last Updated |
|-------|------|--------|--------------|
| GitHub API / PR check status | [knowledge/github-api-pr-checks.md](knowledge/github-api-pr-checks.md) | Resolved | 2026-03-14 |
| CI build failure patterns | [knowledge/ci-failures.md](knowledge/ci-failures.md) | Resolved | 2026-03-14 |
| Git rebase conflicts | [knowledge/git-rebase-conflicts.md](knowledge/git-rebase-conflicts.md) | Resolved | 2026-03-14 |
| clang-format issues | [knowledge/clang-format.md](knowledge/clang-format.md) | Resolved | 2026-03-14 |

## Quick Reference

**Checking PR / CI status** → Use `gh run list` + `gh run view`, NOT `gh pr checks --watch`. See [github-api-pr-checks.md](knowledge/github-api-pr-checks.md).

**CI check failed** → Identify blocking vs. non-blocking jobs first. See [ci-failures.md](knowledge/ci-failures.md).

**Rebase conflict** → `<!-- AUTO:* -->` and `docs/api/` always take upstream. See [git-rebase-conflicts.md](knowledge/git-rebase-conflicts.md).

**clang-format failure** → Don't use `head -50` shortcut; match CI's Metal exclusion. See [clang-format.md](knowledge/clang-format.md).

---

_To add a new entry: create a file in `knowledge/`, add a row here, commit both._
