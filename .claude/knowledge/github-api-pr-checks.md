# GitHub API / PR Check Status

**Last updated:** 2026-03-14
**Type:** Issue
**Status:** Resolved

## Issue

Monitoring CI check status after pushing a PR requires querying the GitHub API. Multiple `gh` CLI commands appear to do this, but they behave inconsistently: some hang indefinitely, some exit with non-zero codes even when checks are running normally, and some require flags that aren't available in all `gh` versions.

## Context

This comes up after every `git push` to a PR branch, when CLAUDE.md instructs polling CI checks until all required jobs pass. The issue is which exact commands to use and in what order.

## Methods Tried

1. **`gh pr checks --watch --fail-fast`** → UNRELIABLE
   Reason: Hangs indefinitely in non-TTY environments (Claude's shell). The `--watch` flag is not available in older `gh` versions. `--fail-fast` exits immediately on first failure even if you want to observe all check states. This command is designed for interactive terminals, not scripted polling.

2. **`gh pr checks`** (no flags) → PARTIALLY WORKS
   Limitation: Returns a snapshot of current check status but exits with code 1 if any check is currently failing — even when you just want to observe. This makes it awkward in a polling loop since you can't distinguish "checks genuinely failed" from "checks still running and one is currently red." Also provides no run IDs to drill into failures.

3. **`sleep 15 && gh pr checks`** (with initial wait) → PARTIALLY WORKS
   Limitation: Same issues as above. The sleep helps ensure checks have started, but doesn't solve the exit-code ambiguity problem.

4. **`gh run list --branch "$(git branch --show-current)" --limit 5`** → WORKS
   Lists recent workflow runs with their IDs and statuses. Reliable, doesn't hang, doesn't exit with misleading codes. Use this to get the RUN_ID for any failing job.

5. **`gh run view <RUN_ID>`** → WORKS
   Shows the full status breakdown of a specific run by ID. Non-blocking, precise. Add `--log-failed` to download logs for failed jobs.

6. **`gh api /repos/{owner}/{repo}/commits/{sha}/check-runs`** → WORKS (fallback)
   Direct GitHub REST API call. Use when `gh pr checks` is broken or unavailable. Replace `{owner}`, `{repo}`, and `{sha}` with actual values. To get the current commit SHA: `git rev-parse HEAD`.

7. **`gh run watch <RUN_ID>`** → UNRELIABLE
   Same TTY/hanging problem as `gh pr checks --watch`. Avoid.

## Solution (Reliable Method)

Use `gh run list` to get run IDs, then `gh run view` to inspect status. Poll manually in a loop:

```bash
# Step 1: Get the run ID for the current branch's latest push
gh run list --branch "$(git branch --show-current)" --limit 5

# Step 2: View the status of a specific run (replace RUN_ID with the ID from step 1)
gh run view <RUN_ID>

# Step 3: If a job failed, get its logs
gh run view <RUN_ID> --log-failed

# Step 4: Re-poll after fixing and pushing
gh run list --branch "$(git branch --show-current)" --limit 3
gh run view <NEW_RUN_ID>
```

**Fallback — direct API** (when `gh pr checks` is unavailable):

```bash
# Get check-runs for the HEAD commit
OWNER=$(gh repo view --json owner -q .owner.login)
REPO=$(gh repo view --json name -q .name)
SHA=$(git rev-parse HEAD)
gh api /repos/$OWNER/$REPO/commits/$SHA/check-runs --jq '.check_runs[] | {name: .name, status: .status, conclusion: .conclusion}'
```

## Notes

- The `build-windows-vs2026` and `clang-tidy` CI jobs have `continue-on-error: true` — they can fail without blocking the PR. Don't waste time debugging them unless asked. See [ci-failures.md](ci-failures.md) for the blocking vs. non-blocking job list.
- `gh pr checks` (no flags, used once) is still useful for a quick snapshot — just don't rely on its exit code in a polling loop.
- Always wait at least 15 seconds after pushing before polling — GitHub takes time to register the new push and start workflows.
- If `gh run list` shows no runs at all for the branch, the push may not have triggered a workflow yet. Wait 30 seconds and retry.
