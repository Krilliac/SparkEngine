# Documentation Coverage Audit

> **Audience:** Programmers | Mixed
>
> **Thread Context:** N/A (audit/reference)
>
> **Platform/Backend Scope:** All platforms / all backends

## Overview

An audit of how completely SparkEngine is documented across three layers: the wiki, Doxygen header comments, and the `docs/` specifications. Coverage is strong and has continued to grow since the original audit (April 2026): the wiki has expanded from 125 to **~173 pages**, and Doxygen `@file`/`@brief` coverage remains effectively complete (522 of 523 engine headers).

---

## Wiki Coverage (re-measured 2026-06-08)

- **~173 wiki pages** (Markdown files under `wiki/`, excluding `_Sidebar.md`), up from 125 in the original audit.
- All engine subsystems, graphics subsystems, and user-facing guides are covered.
- Graphics/rendering pages include: Render Graph, GPU Particles, GPU-Driven Rendering, Volumetric Fog, Global Illumination, Virtual Texturing, Water Rendering, Clustered Lighting, Mesh Shaders, Shader Graph, Post-Processing, Neural Rendering, Material System, Decal System, Particle System, Sky and Atmosphere, Shadow System, Foliage System.
- User-facing docs: FAQ, Quick-Start Tutorial, Editor Walkthrough, Configuration Reference, Performance Tips.
- Broad topic pages: Error Handling Patterns, Hot Reload Overview.

Measurement:

```bash
find wiki -name '*.md' -type f ! -name '_Sidebar.md' | wc -l
```

---

## API Documentation (Doxygen)

- **522 of 523** `SparkEngine/Source` headers carry an `@file` tag — effectively 100% coverage (one header outstanding).
- Doxygen tags use the `@`-style convention throughout (not `\`-style).
- `docs/api/` holds the auto-generated per-header API pages produced by `docs/generate-api-docs.sh`. **Note:** these are generated artifacts and may be absent from the working tree until the script is run (0 generated pages were present at re-measure time). The maintained coverage gate is `tools/check-doxygen-coverage.sh` (95% threshold).

Measurement:

```bash
find SparkEngine/Source -name '*.h' | wc -l                       # 523
grep -rl '@file' SparkEngine/Source --include='*.h' | wc -l       # 522
```

---

## docs/ Directory

```
docs/
├── generate-api-docs.sh
├── generate-flowchart.sh
├── sync-wiki.sh
├── plans/
└── specs/
    ├── asset-format.md
    ├── networking-wire-format.md
    └── plugin-abi-guide.md
```

`docs/specs/` contains the detailed wire-format, asset-format, and plugin-ABI specifications (all three confirmed present).

---

## Top Documentation Gaps — All Resolved

The original audit's five standing gaps were all closed by the time of writing and remain closed:

| Gap | Status |
|-----|--------|
| Networking protocol wire format | Resolved (`docs/specs/networking-wire-format.md` + wiki page) |
| Asset format specifications | Resolved (`docs/specs/asset-format.md` + wiki page) |
| Plugin ABI stability & versioning | Resolved (`docs/specs/plugin-abi-guide.md` + wiki page) |
| Physics solver tuning guide | Resolved (Solver Tuning section in Physics page) |
| Migration / upgrade guide | Resolved (Migration Guide wiki page) |

---

## What's Done Right

- ~173 wiki pages covering all major subsystems, graphics, and rendering.
- Effectively complete header Doxygen coverage (522/523).
- `Codebase-Statistics.md` (metrics) and `Codebase-Health.md` (maturity) maintained.
- Unified Error Handling and Hot Reload documentation.
- README badges for all platforms, compilers, and quality tools.
- All analysis data lives in the wiki — no orphaned `docs/` files.
- `docs/specs/` holds detailed wire/asset/plugin specs.

---

## Keeping Coverage Current

```bash
docs/update-all-docs.sh             # runs all six doc scripts
docs/update-all-docs.sh check       # dry-run, reports what is stale
tools/check-doxygen-coverage.sh     # header @file/@brief gate (95%)
```

---

## Source & Freshness

- **Original audit:** `.claude/knowledge/documentation-coverage-audit.md`, last updated 2026-04-06.
- **Re-measured against codebase 2026-06-08.**
- OLD → NEW numbers updated:
  - Wiki pages OLD 125 → NEW ~173.
  - Doxygen header coverage OLD 246/246 → NEW 522/523 (header count grew as the codebase grew; coverage still effectively complete).
  - Auto-generated API pages OLD "370 pages from 382 headers" → NEW 0 present in the working tree (generated on demand; not committed at re-measure time).
- Findings now resolved since the original audit: all five top documentation gaps (wire format, asset format, plugin ABI, physics tuning, migration guide) were already resolved and remain so; verified the three `docs/specs/` files still exist.

## Related Pages

- [Codebase Statistics](Codebase-Statistics.md)
- [Codebase Health](Codebase-Health.md)
- [Contributing](Contributing.md)
- [Error Handling Patterns](Error-Handling-Patterns.md)
- [Hot Reload Overview](Hot-Reload-Overview.md)
