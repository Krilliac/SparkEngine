# Accessing CI Build Logs Without gh CLI

**Last updated:** 2026-03-22
**Type:** Issue
**Status:** Resolved

## Description

When `gh` CLI is not installed (common in sandboxed/container environments), accessing GitHub Actions build logs requires workarounds. The standard `gh run view --log-failed` approach fails entirely.

## Context

The Claude Code web environment does not have `gh` CLI installed. `pip install gh` installs a wrong Python package (not GitHub CLI). `apt-get install` may be blocked by lock files or missing repos. GitHub Actions log URLs are JavaScript-rendered and not accessible via simple HTTP fetch.

## Methods Tried

1. **`gh run view <RUN_ID> --log-failed`** → FAILED — `gh` not installed, cannot be installed via pip.
2. **`pip install gh`** → FAILED — installs wrong package (Python gitpython wrapper, not GitHub CLI).
3. **`WebFetch` on GitHub Actions job page** → FAILED — logs are lazy-loaded via JavaScript; HTML contains only job metadata, not actual compiler output.
4. **GitHub REST API via curl (`/repos/.../actions/jobs/.../logs`)** → FAILED — requires admin rights / authentication token.
5. **Local proxy API** → FAILED — proxy only supports git operations, not GitHub API endpoints.
6. **WebFetch on commit checks page** → FAILED — same JS rendering issue.
7. **WebFetch on raw/download log URLs** → FAILED — 404, requires auth.
8. **Ask user to paste errors from CI** → WORKED — user provided the actual MSVC error messages directly.

## Solution

When `gh` CLI is unavailable and GitHub Actions logs cannot be fetched programmatically:

### Primary workaround: Ask the user
The most reliable approach is to ask the user to copy-paste the error messages from the GitHub Actions UI. This is fast and accurate.

### Secondary workaround: Reproduce locally
Match the CI build environment as closely as possible:
1. Initialize git submodules: `git submodule update --init --recursive`
2. Install CI dependencies: `libgl-dev libvulkan-dev libsdl2-dev`
3. Build with same flags as CI (see `.github/workflows/build.yml`)

**Key difference between local and CI**: CI uses `submodules: recursive` and installs `libsdl2-dev`, enabling code paths that are disabled locally (SDL2 windowing, editor on Linux, etc.). Always init submodules before attempting local reproduction.

### Tertiary workaround: Check commit/PR page metadata
`WebFetch` on the main run page can sometimes show:
- Which jobs failed and their exit codes
- Annotation counts
- Triggering commit SHA and branch
- PR number

This narrows the search space even without full logs.

### Common CI-only errors that don't reproduce locally
- **Windows API macro clashes**: `GetCurrentTime`, `CreateWindow`, `GetObject`, `SendMessage`, `GetMessage`, `LoadImage`, `DrawText` etc. are macros in `<windows.h>`. Methods with these names compile fine on Linux but fail on MSVC.
- **Missing includes**: Headers implicitly included on one platform but not another (e.g., `<cstring>` for `memcpy` on Clang).
- **SDL2-gated code paths**: Code inside `#ifdef SPARK_SDL2_AVAILABLE` only compiles when SDL2 is found by CMake.

## Notes

- Always check `Assert.h` for existing `#undef` workarounds before adding new ones.
- The `.github/workflows/build.yml` file documents the exact CI build configurations — mirror them locally.
- When encountering Windows macro clashes, rename the method rather than adding `#undef` — it's a more robust fix.
