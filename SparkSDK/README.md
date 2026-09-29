# SparkEngine SDK

The SparkEngine SDK is the public contract for standalone C++ game modules. It
contains the `Spark::IModule` interface, the host context, the fixed-layout
module compatibility descriptor, and the CMake helper used to build a module
against an installed engine package.

## Consume an installed SDK

Install the `sdk` component from a configured engine build, then point a
standalone project at its CMake package directory:

```powershell
cmake --install <engine-build> --prefix <spark-sdk> --config Release --component sdk
cmake -S <my-game> -B <my-game>/build `
    -DSparkEngine_DIR="<spark-sdk>/lib/cmake/SparkEngine"
cmake --build <my-game>/build --config Release
```

The project should use the installed package and the module helper:

```cmake
find_package(SparkEngine CONFIG REQUIRED)
spark_add_game_module(MyGame Source/GameModule.cpp Source/GameModule.h)
target_include_directories(MyGame PRIVATE Source)
```

Do not add the engine checkout's `SparkEngine/Source` or `SparkSDK/Include`
directories to a standalone project. If a module needs either path, it is no
longer proving the installed SDK contract.

## Public headers and ABI

The single-include entry point is:

```cpp
#include <Spark/SparkSDK.h>
```

The SDK ABI version is `SPARK_SDK_VERSION`. It is an exact-match contract: a
module built against a different version is incompatible and must be rebuilt
against the installed headers. Every module also emits a sibling `.sparkabi`
descriptor. The host validates that fixed-layout descriptor before calling a
module factory, so an incompatible module is rejected before module code runs.

Use `SPARK_IMPLEMENT_MODULE` in exactly one source file to provide the exported
`CreateModule`, `DestroyModule`, and compatibility entry points.

`Spark/GameTypes.h` owns the shared gameplay enum declarations. FPS consumes
these public types directly; `Enums/GameSystemEnums.h` in the engine is only a
compatibility include. Do not copy the declarations into a module. Their names,
underlying types and numeric values are unchanged, so this extraction does not
change SDK ABI v7.

Use `IEngineContext::GetConsole()` for module command registration and
`Spark/ModuleLog.h` for logging. Track only successful registrations and remove
them in `OnUnload()` before releasing module state or its context. ARPG, RPG,
Racing, OpenWorld, RTS and Platformer use these public services. FPS uses the
public command service in its main and headless entrypoints; other FPS
implementation files still depend on private logging and engine systems.

### Changing the SDK ABI (maintainers)

The binary surface a module compiles against — every SDK interface's virtuals
in vtable order, the `ModuleInfo` and compatibility-descriptor layouts, the
module-ABI macros and factory typedefs — is pinned in
`SparkSDK/ABI/sdk-abi-surface.json` and checked by
`python3 SparkSDK/Tools/sdk_abi_surface.py check` (ctest `SparkSDKABISurface`).
It fails when that surface changes without a `SPARK_SDK_VERSION` bump, when a
bump was not re-pinned, when `EngineContextVirtualCount` is stale, or when
`Spark/Version.h` lacks a `// vN:` note for the current version. To make an ABI
change: bump `SPARK_SDK_VERSION`, add the `// vN:` note, re-pin the
`static_assert` layout blocks in `Spark/IModule.h` and `Spark/ModuleABI.h`, then
run `python3 SparkSDK/Tools/sdk_abi_surface.py update`. The `.sparkabi` sidecar
values are derived from `Spark/Version.h` and `Spark/ModuleABI.h` by
`cmake/SparkGameModule.cmake`, so no CMake edit is needed.

## Package contents

The SDK component is self-contained and includes:

- public headers under `include/Spark/`;
- exported CMake targets and `spark_add_game_module` under
  `lib/cmake/SparkEngine/`;
- `LICENSE.txt` and `THIRD_PARTY_NOTICES.txt` under
  `share/SparkEngine/sdk/`;
- this README; and
- a buildable `EmptyProject` example under
  `share/SparkEngine/sdk/examples/EmptyProject/`.

On Windows, the `SDKConsumer_InstalledEmptyProjectTemplate` CTest installs
only the `sdk` component and builds that example from the installed copy, in
the engine's configuration. The test fails if any header, library or flag
path the build resolves points into the engine source or build tree. It also
fails if the module's `.sparkabi` sidecar would be rejected by the host's
pre-load check. This test is registered only when `BUILD_GAME_MODULES` is on.

The installed package also contains the engine libraries required by the
exported targets. The SDK package is a consumer surface, not a declaration
that every engine subsystem or game module is stable on every platform.

## Compatibility diagnostics

When a module is rejected, inspect the host's module-load diagnostic and the
module's `.sparkabi` sidecar. The stable-v1 module ABI is exact-match only;
there is no N-1 load or migration path. The diagnostic names the failing
sidecar field, the value the host expects, and the value the module declares,
for example:

```text
Module 'libMyGame.so' rejected before OS load: SDK ABI version mismatch: field 'sdk_version' host expects 5, module declares 4; stable-v1 module ABI is exact-match only (N-1 modules are not loaded); rebuild the module against this host's Spark SDK and toolchain
```

The checked fields are `struct_size`, `magic`, `format`, `sdk_version`,
`runtime_abi_version`, `compiler_family`, `compiler_abi_version`,
`cxx_language_level`, `runtime_library`, `iterator_debug_level`, and
`pointer_size`. Recompile the module with the same supported toolchain
and SDK package; do not work around the check by copying private engine headers
or libraries into the consumer project.
