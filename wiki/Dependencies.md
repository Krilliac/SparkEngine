# SparkEngine Dependencies

> **Audience:** Programmers, build engineers, and maintainers
>
> **Thread Context:** Not applicable — dependency index
>
> **Platform/Backend Scope:** Dependency set varies by platform, backend, and CMake options

SparkEngine combines bundled third-party libraries with platform SDKs and optional system packages. Use the build configuration for the exact dependency set enabled by a preset.

## Dependency references

- [Third-Party Dependencies Audit](advanced/ThirdParty-Dependencies-Audit.md) — inventory, ownership, and maintenance findings.
- [Third-Party Library Evaluation](research/Third-Party-Library-Evaluation.md) — evaluated alternatives and integration notes.
- [System Requirements](platform/System-Requirements.md) — compiler, SDK, driver, and runtime requirements.
- [Build System and CMake Modules](advanced/Build-System-and-CMake-Modules.md) — discovery and feature toggles.
- [`ThirdParty/`](https://github.com/Krilliac/SparkEngine/tree/Working/ThirdParty) — bundled dependency source and shims.

## Core technology

The current engine architecture uses EnTT for ECS, Jolt Physics, AngelScript, Dear ImGui, and backend-specific graphics APIs through the SparkEngine RHI. Some backends and features require optional SDKs; disabled features should not force their dependencies into unrelated builds.

For reproducible contributor builds, follow [CI Reproducible Builds](development/CI-Reproducible-Builds.md) rather than installing every optional dependency.

## Source & Freshness

The source tree, CMake configuration, and dependency audit on `Working` are authoritative when this summary and a concrete build differ.
