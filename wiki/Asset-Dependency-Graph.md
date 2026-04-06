# Asset Dependency Graph

Asset reference tracking, dependency analysis, unused asset detection, circular dependency detection, and size budgeting for editor workflows.

**Source:** `SparkEditor/Source/Panels/AssetDependencyGraph.h`

## Overview

The Asset Dependency Graph builds and maintains a directed graph of all asset references in the project. Each node represents an asset (texture, material, mesh, etc.) with its file size and type. Directed edges represent "depends on" relationships -- for example, a material depends on its textures, and a scene depends on the prefabs it places.

The graph supports both forward queries (what does this asset depend on?) and reverse queries (what references this asset?). Transitive dependency resolution uses BFS to collect the full closure of an asset's dependency tree. This powers the Reference Viewer panel, which lets artists and designers understand the impact of changing or deleting an asset.

Two key audit features are built in: unused asset detection finds assets with no incoming references that are not marked as root assets (scenes/levels), and circular dependency detection uses DFS with path tracking to find and report dependency cycles. The size budget system computes the total transitive size for a scene or level, broken down by asset type, helping teams stay within memory and download budgets.

## Key Classes

| Class / Struct | Description |
|---|---|
| `AssetDependencyGraph` | Singleton that owns the graph and provides all query/audit operations |
| `AssetNode` | A single asset: path, type, size, dependencies, reverse references, root flag |
| `AssetType` | Enum classifying assets: Texture, Material, Mesh, Animation, Audio, Scene, Prefab, Script, Shader, Font, Data, Video |
| `CircularDependency` | A detected cycle represented as a chain of asset paths |
| `SizeBudgetReport` | Total size, asset count, per-type breakdown, and top-N largest assets for a root |
| `AuditReport` | Project-wide summary: totals, unused count/size, circular dependency count, per-type breakdowns |

## Usage

```cpp
auto& graph = SparkEditor::AssetDependencyGraph::GetInstance();
graph.Initialize();

// Register assets and their relationships
graph.RegisterAsset("Materials/Metal.mat", SparkEditor::AssetType::Material, 2048);
graph.RegisterAsset("Textures/Metal_Albedo.png", SparkEditor::AssetType::Texture, 524288);
graph.RegisterAsset("Textures/Metal_Normal.png", SparkEditor::AssetType::Texture, 262144);
graph.AddDependency("Materials/Metal.mat", "Textures/Metal_Albedo.png");
graph.AddDependency("Materials/Metal.mat", "Textures/Metal_Normal.png");

// Mark scenes as root assets (they won't appear as "unused")
graph.SetRootAsset("Scenes/Level_01.scene");

// Query references
auto deps = graph.GetDependencies("Materials/Metal.mat");       // direct deps
auto refs = graph.GetReferencedBy("Textures/Metal_Albedo.png"); // who uses this?
auto all  = graph.GetTransitiveDependencies("Scenes/Level_01.scene"); // full closure

// Audit
auto unused  = graph.FindUnusedAssets();
auto cycles  = graph.FindCircularDependencies();
auto budget  = graph.GetSizeBudget("Scenes/Level_01.scene");
auto audit   = graph.GenerateAudit();
```

## API Reference

### Graph Construction

| Method | Return | Description |
|---|---|---|
| `RegisterAsset(path, type, sizeBytes)` | `void` | Add an asset node to the graph |
| `AddDependency(from, to)` | `void` | Add a directed edge: `from` depends on `to` |
| `RemoveDependency(from, to)` | `void` | Remove a dependency edge |
| `RemoveAsset(path)` | `void` | Remove an asset and all its edges |
| `SetRootAsset(path, isRoot)` | `void` | Mark/unmark an asset as a root (scene/level) |
| `Clear()` | `void` | Remove all nodes and edges |

### Queries

| Method | Return | Description |
|---|---|---|
| `GetDependencies(path)` | `vector<string>` | Direct dependencies of an asset |
| `GetReferencedBy(path)` | `vector<string>` | Assets that reference this asset |
| `GetTransitiveDependencies(path)` | `vector<string>` | All recursive dependencies (BFS) |
| `GetAsset(path)` | `const AssetNode*` | Get the full node for an asset |
| `GetAssetCount()` | `size_t` | Total number of registered assets |
| `GetAllAssetPaths()` | `vector<string>` | All asset paths in the graph |

### Audit and Budgeting

| Method | Return | Description |
|---|---|---|
| `FindUnusedAssets()` | `vector<string>` | Assets with no references and not marked as root |
| `FindCircularDependencies()` | `vector<CircularDependency>` | All detected dependency cycles |
| `GetSizeBudget(rootPath)` | `SizeBudgetReport` | Size breakdown for a scene and its transitive deps |
| `GenerateAudit()` | `AuditReport` | Full project audit: totals, unused, cycles, per-type stats |

## Related Systems

- [Asset Browser](Asset-Browser.md) -- displays assets with reference counts from this graph
- [Editor Panels](Editor-Panels.md) -- Reference Viewer and Asset Audit panels consume this data
- [File Watcher](File-Watcher.md) -- triggers graph updates when assets change on disk
- [Selection Manager](Selection-Manager.md) -- selecting an asset in the browser can show its dependencies
