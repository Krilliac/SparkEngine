# SparkEngine Packaging and Distribution

This document defines SparkEngine binary package formats, install layout, components, and versioning policy.

## Package Generators

SparkEngine uses CPack from the top-level `CMakeLists.txt`.

- **Linux/macOS:** `ZIP`, `TGZ`
- **Windows:** `ZIP`, `NSIS`, `WIX`

Create packages after configure/build:

```bash
cmake --build build --config Release
cpack --config build/CPackConfig.cmake
```

## Package Components

CPack component install is enabled. Components are:

- `runtime` — engine runtime binaries and runtime shader content
- `sdk` — public headers, static libraries, and CMake package files (`SparkEngineConfig.cmake`)
- `tools` — helper tooling (`spark-cli`, `SparkShaderCompiler` when built)
- `templates` — starter project templates under `Templates/`
- `samples` — sample project content (`Samples/` when present, plus `GameModules/SparkGame` source snapshot)

## Install Layout

Default install prefix layout:

- `bin/` — runtime executables and runtime content
- `lib/` — static/shared libraries
- `include/` — `SparkSDK` and engine public headers
- `lib/cmake/SparkEngine/` — `SparkEngineConfig.cmake`, `SparkEngineConfigVersion.cmake`, target exports, helpers
- `tools/` — Spark command-line tooling
- `share/SparkEngine/templates/` — installable templates
- `share/SparkEngine/samples/` — installable sample content

## External Consumption (`find_package`)

After installation:

```bash
cmake -S MyGame -B build -DSparkEngine_DIR=/path/to/install/lib/cmake/SparkEngine
```

Then in the external CMake project:

```cmake
find_package(SparkEngine REQUIRED)
target_link_libraries(MyTarget PRIVATE Spark::SparkEngineLib)
```

A CI smoke project exists at `Tests/PackageSmoke/` and is used to validate package consumption on release pipelines.

## Versioning Policy

- Engine package version is driven by `SPARK_ENGINE_VERSION` (cache var) and mapped to `project(... VERSION ...)`.
- `SparkEngineConfigVersion.cmake` uses `SameMajorVersion` compatibility.
- Release tags are expected as `vMAJOR.MINOR.PATCH` and should align with `SPARK_ENGINE_VERSION` before final release publishing.
- Nightly/release branch package artifacts are CI-generated snapshots and may include pre-release behavior.
