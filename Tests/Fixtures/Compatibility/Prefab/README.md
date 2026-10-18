# Prefab compatibility fixtures

SAVE-230 fixtures for editor prefabs, `<Name>.sparkprefab`
(`SparkEditor::PrefabAsset`, `SparkEditor/Source/Prefabs/PrefabAsset.cpp`).

## Version window

- The editor reads `SPARKPREFAB` 1 (N-1) and 2 (N) and writes 2 only
  (`PrefabAsset::kOldestSupportedPrefabVersion`, `PrefabAsset::kPrefabFormatVersion`).
  This is the same N-1..N window as `.spark_save` slots.
- Version 1 is converted in memory. Loading never rewrites the file; the next
  save writes version 2 and keeps the version 1 bytes as `<Name>.sparkprefab.bak`.
- A larger version fails closed. `TryLoad` returns false with an error that names
  the file, its version and the supported window ("reads versions 1 to 2") and
  tells the user to open it with a newer SparkEditor. The retained `.bak` is not
  used, because loading an older copy and saving over the newer file would
  discard its data.

## Why version 2 exists

Version 1 wrote names and string values as bare text. It could not store a
component or property name containing whitespace, or a string value spanning
lines, so `Save` refused them. A file cut inside the value of its last property
still parsed as a shorter value, because nothing followed the last declared
property. Version 2 writes every name and string value as a double-quoted token
with `\\`, `\"`, `\n`, `\r` and `\t` escapes, writes numbers in their shortest
round-trip form (`std::to_chars`), sorts properties by name, and ends with an
`end` line so any cut is detected.

## Files

`v1-guard-tower.sparkprefab` is in the exact layout of the version 1 writer, the
`PrefabAsset::Save` of commit `be62a0cce` (the last commit before version 2). Its
floats and double are the bytes that writer emits: the value formatted with
`std::setprecision(max_digits10)` of `double` in the classic locale, which is
`%.17g` of the float widened to double. The properties are listed in an
arbitrary order, as the version 1 writer iterated an `unordered_map`; the reader
ignores their order. It holds everything version 1 can store losslessly: a
`Transform` with float3 `position` and `scale` and a float4 `rotation`, an int, a
bool, a double and a string value containing a space.

| File | SHA-256 of the committed (LF) blob | Purpose |
|---|---|---|
| `v1-guard-tower.sparkprefab` | `9f3a11558d970664ef7a623997afe58c674eab0339627c889efa0bb8e1d98d46` | Must load with every declared value bit-identical, must not be rewritten by the load, and must re-save as version 2 that reloads to the same values. |
| `v3-future.sparkprefab` | `be31c40ea698f8b17c6d9462cd425b030b11c3f7ed0cdd4f2ba6be1bf0bc73fe` | Declares `SPARKPREFAB 3` and a property type no current build knows. Must fail closed with a versioned error and leave the output prefab untouched. |

Tests (`PrefabMigration_*` in `Tests/TestPrefabCompatibility.cpp`, CTest
`SparkPrefabCompatibilityTests`) read these files in place and compare their bytes
before and after each load rather than a pinned digest, so a Windows checkout
that converts line endings still passes.
