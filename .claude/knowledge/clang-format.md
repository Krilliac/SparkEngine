# clang-format — CI-matching invocation

**Last updated:** 2026-04-17
**Type:** Pattern
**Status:** Active

## Description

The CI `check-format` job runs clang-format over the full source tree. Local pre-commit runs must use **exactly the same command** as CI, or local passes will be followed by CI failures on files the local run never touched.

---

## Wrong (commonly seen in old examples)

```bash
find SparkEngine/Source SparkEditor/Source SparkConsole/src GameModules \
  -name '*.h' -o -name '*.cpp' \
  | head -50 \
  | xargs clang-format --dry-run --Werror
```

`head -50` caps the input to 50 files. Locally this passes while leaving thousands of files unchecked. CI runs without `head -50` and immediately fails.

## Right — matches CI

```bash
find SparkEngine/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src GameModules \
  -name '*.h' -o -name '*.cpp' \
  | xargs clang-format --dry-run --Werror
```

Omit `head -50`. Include `SparkShaderCompiler/src` (easy to miss — CI includes it).

## Fix formatting in place

```bash
find SparkEngine/Source SparkEditor/Source SparkConsole/src SparkShaderCompiler/src GameModules \
  -name '*.h' -o -name '*.cpp' \
  | xargs clang-format -i
```

## Rules

- Never use `head -N` in a format check command. If the argv is too long, use `xargs -n 500` to batch instead.
- `--Werror` is required — without it, `--dry-run` only reports diffs and exits 0.
- `.clang-format` at the repo root is the source of truth. Don't pass `--style=...` explicitly.

## CI command source

The exact command CI runs lives in `.github/workflows/build.yml` under the `check-format` job. When you change formatting rules, match that job.

## See also

- [workflow-patterns.md](workflow-patterns.md) — pre-commit flow, where format-check is step 1.
- [build-optimizations.md](build-optimizations.md) — `--parallel $(nproc)` for the subsequent build step.
- Parent project guide: `CLAUDE.md` — "Pre-commit checks" → "Code changes" section.
