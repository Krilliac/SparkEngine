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
Module 'libMyGame.so' rejected before OS load: SDK ABI version mismatch: field 'sdk_version' host expects 4, module declares 3; stable-v1 module ABI is exact-match only (N-1 modules are not loaded); rebuild the module against this host's Spark SDK and toolchain
```

The checked fields are `struct_size`, `magic`, `format`, `sdk_version`,
`runtime_abi_version`, `compiler_family`, `compiler_abi_version`,
`cxx_language_level`, `runtime_library`, `iterator_debug_level`, and
`pointer_size`. Recompile the module with the same supported toolchain
and SDK package; do not work around the check by copying private engine headers
or libraries into the consumer project.
