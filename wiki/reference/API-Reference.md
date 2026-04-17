# API Reference

A complete, auto-generated reference to every symbol, file, and folder in the SparkEngine codebase — no third-party tool required. Everything on this page is produced by pure-shell scripts under `docs/`, consumes only source files in the repository, and renders natively on GitHub.

> **How to regenerate:** `docs/update-all-docs.sh` (or individually, e.g. `docs/generate-symbol-index.sh generate`).

---

## Master indexes

| Page | Contents |
|------|----------|
| [Symbol Index](Symbol-Index.md) | Every class, struct, enum, function, method, macro, and type alias — alphabetical, with file-and-line source links. |
| [Function Index](Function-Index.md) | Every free function and out-of-line method definition (headers + `.cpp`). |
| [Class & Struct Index](Class-Index.md) | Every declared class and struct in the codebase. |
| [Enum Index](Enum-Index.md) | Every enum with its source location. |
| [Macro & Alias Index](Macro-Index.md) | Every `#define` (excluding header guards) plus `typedef`/`using` aliases. |
| [File Tree](File-Tree.md) | Hierarchical listing of every source file with LOC and `@brief`. Includes a Mermaid module-dependency graph. |
| [Class Hierarchy](Class-Hierarchy.md) | Mermaid `classDiagram` per top-level module showing `public` inheritance. |

## Quick-reference indexes (scoped)

| Page | Contents |
|------|----------|
| [Entity Component System](../subsystems/Entity-Component-System.md) | ECS components and systems overview. |
| [Architecture Overview](../getting-started/Architecture-Overview.md) | High-level engine architecture. |
| [Codebase Statistics](../advanced/Codebase-Statistics.md) | LOC, file counts, subsystem metrics. |
| [Engine Architecture Flowchart](../getting-started/Engine-Architecture-Flowchart.md) | ASCII/Mermaid flowcharts of subsystems. |

## Per-header detail pages

Running `docs/generate-api-docs.sh generate` also produces ~750 per-header Markdown pages under `docs/api/` (one per `.h`/`.hpp`). These are not committed — regenerate locally whenever you want the deeper view. Each page lists classes, enums, free functions, macros, and type aliases in that header, each with a clickable source-line anchor.

---

## What gets scanned

The generator scans **every** `.h`, `.hpp`, and `.cpp` under:

- `SparkEngine/Source/`
- `SparkEditor/Source/`
- `SparkConsole/src/`
- `SparkShaderCompiler/src/`
- `SparkSDK/`
- `Tests/`
- `GameModules/*/Source/` (all game modules auto-discovered)

`ThirdParty/` and build directories are excluded.

## What gets extracted

For every source file:

- **Classes & structs** — name, base class (when `: public X`), surrounding `@brief` comment, source line.
- **Enums** — name, source line.
- **Functions & methods** — free functions, static functions, and out-of-line method definitions (`Class::Method`) in `.cpp`. Each gets a signature and a clickable `file:line` anchor.
- **Macros** — every `#define`, excluding header guards.
- **Type aliases** — every `using X = …` and `typedef … X;`.

All symbols are also emitted to `docs/api/.symbols.tsv` for programmatic use.

## Why not Doxygen?

The SparkEngine documentation pipeline is intentionally **dependency-free**: the generator scripts use only `find`, `grep`, `awk`, `sed`, `sort`, and `wc` — tools present on every developer machine and every CI runner. The legacy `docs/Doxyfile.txt` and `docs/generate-docs.sh` remain available for anyone who wants full HTML with GraphViz, but nothing on this page needs them.
