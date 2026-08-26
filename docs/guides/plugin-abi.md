# Stable plugin ABI

SparkEngine exposes two complementary extension contracts:

- Game modules use the existing C++ `IModule` contract and its strict
  compiler/runtime `.sparkabi` compatibility sidecar.
- Importers, processors, editor extensions, runtime extensions, and external
  tools can use `Spark/PluginABI.h`, a stable C ABI that avoids C++ runtime
  coupling.

For the compiler-specific C++ game-module contract, lifecycle, and `.sparkabi`
rules, see the [C++ Game Module ABI Guide](../specs/plugin-abi-guide.md).

## Boundary rules

The public plugin boundary carries only fixed-width values, opaque handles,
byte spans, and append-only function tables. Do not pass STL containers,
exceptions, RTTI objects, C++ vtables, or memory owned by a plugin allocator
across it. Returned memory must use the allocator and allocation tag supplied
in `SparkPluginHostAPI`.

Every table begins with `struct_size`, `abi_major`, and `abi_minor`. A host
accepts the same major version and a minor version no newer than it implements;
new optional fields are appended. Required lifecycle functions are `create`
and `destroy`. Optional capabilities advertise ticking, hot reload, asset
import/processing, editor integration, and runtime integration.

Hot-reloadable plugins must quiesce their scheduled tasks in
`prepare_unload`, serialize only stable IDs and bytes through `save_state`,
restore them through `restore_state`, and reverse a prepared-but-aborted
transaction through `cancel_unload`. Host resources are reacquired by stable
ID rather than persisting process-local pointers or handles. Advertising
`SPARK_PLUGIN_CAP_HOT_RELOAD` in ABI minor 1 or newer requires all four
callbacks. ABI minor 0 plugins remain loadable, but cannot use transactional
hot reload because they have no rollback callback. During
`DynamicPluginHost::Reload`, `save_state` receives a host-owned buffer with
`size` initially zero and `capacity` limited to 16 MiB; the plugin writes only
within `capacity` and sets `size` to the bytes produced. The replacement must
restore those bytes successfully before the old image is destroyed. The old
generation is stopped before the replacement starts, so two generations never
own runtime resources concurrently. Load, save, restore, or replacement-start
failures keep the old image loaded; pre-commit failures call `cancel_unload`,
and a replacement-start failure restarts the stopped old generation before the
host reopens task scheduling.

## Building a plugin

```cmake
find_package(SparkEngine REQUIRED)

spark_add_plugin(MyImporter
    ID "org.example.my-importer"
    VERSION "1.0.0"
    TYPE "asset-importer"
    SOURCES MyImporter.cpp)
```

The helper deliberately does not link `SparkEngineLib`. It exports only the
entry point declared by `SPARK_DECLARE_PLUGIN_ENTRY_POINT()` and writes a
deterministic `<binary>.sparkplugin.json` sidecar containing identity, ABI,
entry-point, and SHA-256 metadata. `DynamicPluginHost` requires this sidecar,
strictly validates its schema, ABI, entry point, and binary filename, then
streams and verifies the binary SHA-256 before asking the operating system to
map executable code. After mapping, the host also requires the sidecar identity
and version to match the in-image descriptor before calling `create`.

The SHA-256 binds a binary to its adjacent package metadata and detects damage
or substitution within a package. It is not a publisher signature: distribution
systems must authenticate or sign the package when provenance is a security
requirement.
