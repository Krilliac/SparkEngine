# SparkEditor ⇄ Engine ECS Round-Trip — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make SparkEditor edit the engine's real ECS `World` — render it, inspect/edit real components, and save a reflection-driven JSON scene that a runtime loads and renders identically.

**Architecture:** Add one engine-side, GPU-free serializer (`ReflectedSceneSerializer`) that walks the existing `TypeRegistry` reflection to convert a `Spark::World` ⇄ JSON. The editor gains a live `Spark::World` document; its Scene View renders that world via the proven device-direct mesh path, its Inspector/Hierarchy operate on real entities/components through reflection, and Ctrl+S calls the serializer. A `-scene <path>` flag on `SparkEngine.exe` proves the runtime loads the same file.

**Tech Stack:** C++23, MSVC (VS2022), D3D11, EnTT (via `Spark::World`), Dear ImGui, the in-repo `TypeRegistry`/`ComponentFactory` reflection, the header-only `Tests/TestFramework.h` suite.

## Global Constraints

- Platform: Windows / MSVC. Build every target via `powershell -File "C:\Users\natew\.claude\scripts\build.ps1" "<full cmake build command>"` (pins `CC/CXX=cl` through vcvars; exit code = build exit code).
- Canonical model is the **engine ECS** (`Spark::World` + reflected components). Do **not** introduce a parallel component model. The editor's legacy `SceneFile` structs stay in the tree but are not on the ECS path.
- Serializer field support is scoped to the `FieldType` values the reflection layer round-trips today: `Bool, Int, Float, Double, String, Vector2, Vector3, Vector4`. `Enum/Custom/Unknown` fields are **logged and skipped** (documented gap), never silently dropped.
- Reflection entry points (verbatim): `Spark::TypeRegistry::Get().FindTypeByName(name) -> const TypeInfo*` (`.fields` is `std::vector<FieldInfo>`); `Spark::GetFieldAsString(const void*, const FieldInfo&) -> std::string`; `Spark::SetFieldFromString(void*, const FieldInfo&, const std::string&) -> bool`; `Spark::ComponentFactory::Get()` with `GetRegisteredNames()`, `HasComponent(name, void* world, uint32_t entity)`, `AddComponent(...)`, `GetComponentRaw(...) -> void*`.
- ECS World (verbatim): `Spark::World` in `Engine/ECS/Components.h`; `EntityID = entt::entity`; `CreateEntity(const std::string& name="")`, `GetComponent<T>(EntityID)->T*` (nullptr if absent), `HasComponent<T>`, `AddComponent<T>`, `GetEntitiesWith<Ts...>()` returns an `entt::view`, `GetRegistry()`.
- Core components (verbatim): `NameComponent{ std::string name; }`, `Transform{ XMFLOAT3 position,rotation,scale; EntityID parent=entt::null; std::vector<EntityID> children; }`, `MeshRenderer{ std::string meshPath, materialPath; bool castShadows,receiveShadows,visible; }`, `Script{ std::string scriptPath,className,moduleName; bool enabled,started; }` (all in `Engine/ECS/Components/CoreComponents.h`).
- Commit after every task. Commit trailer: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`. Branch: `claude/editor-ecs-roundtrip`.
- TDD where the logic is GPU-free (Unit A). For GPU/UI units, validation is build-green + a scripted run + screenshot compare (the spec's stated testing method); each such task states the exact command and the expected visual.

---

## File / responsibility map

| File | Responsibility | Task |
|---|---|---|
| `SparkEngine/Source/SceneManager/ReflectedSceneSerializer.h` | Public API: `SerializeWorld`, `DeserializeInto`, `SaveWorld`, `LoadWorld` | A1 |
| `SparkEngine/Source/SceneManager/ReflectedSceneSerializer.cpp` | Reflection walk → JSON and back; parent resolution | A1–A2 |
| `Tests/TestReflectedScene.cpp` | Headless round-trip + field-coverage tests | A3–A4 |
| `SparkEditor/Source/Rendering/EditorSceneRenderer.h/.cpp` | Draw a `Spark::World` into an RTV via basic-shader; fly-camera; gizmo overlay | B1–B3 |
| `SparkEditor/Source/Panels/SceneViewPanel.*` | Call `EditorSceneRenderer` instead of the clear-only placeholder | B1 |
| `SparkEditor/Source/Core/EditorUI.*` | Own `std::unique_ptr<Spark::World> m_world`; wire Save/Open to the serializer | C1, C4 |
| `SparkEditor/Source/Panels/HierarchyPanel.*` | Enumerate/create/rename world entities when a world is present | C2 |
| `SparkEditor/Source/Panels/InspectorPanel.*` | Inspect the selected entity's components via `TypeRegistry` | C3 |
| `SparkEngine/Source/Core/SparkEngineWindows.cpp` / `SparkEngineLinux.cpp` | `-scene <path>` CLI flag → load + render a reflected scene | D1 |
| `Tests/CMakeLists.txt`, `SparkEditor/CMakeLists.txt` | Add new sources to targets | A3, B1 |

---

# UNIT A — Reflected scene serializer (engine, GPU-free, TDD)

### Task A1: Serializer header + `SerializeWorld`

**Files:**
- Create: `SparkEngine/Source/SceneManager/ReflectedSceneSerializer.h`
- Create: `SparkEngine/Source/SceneManager/ReflectedSceneSerializer.cpp`
- Modify: `CMakeLists.txt` (add the .cpp to `SPARK_ENGINE_LIB_SOURCES`)

**Interfaces:**
- Consumes: `Spark::World`, `Spark::TypeRegistry`, `Spark::ComponentFactory`, `Spark::GetFieldAsString`, `Spark::FieldInfo`, `Spark::FieldType`.
- Produces (later tasks + editor + runtime rely on these EXACT signatures):
  ```cpp
  namespace Spark {
    // Serialize the whole world to a JSON string (no disk).
    std::string SerializeWorld(const World& world);
    // Parse JSON into an existing (typically empty) world. Returns false on malformed JSON.
    bool DeserializeInto(World& world, const std::string& json);
    // Convenience disk wrappers.
    bool SaveWorld(const World& world, const std::string& path);
    bool LoadWorld(World& world, const std::string& path);
  }
  ```

- [ ] **Step 1: Write the header**

Create `SparkEngine/Source/SceneManager/ReflectedSceneSerializer.h`:

```cpp
#pragma once
#include <string>

namespace Spark {
class World;

/// Serialize/deserialize a Spark::World to a reflection-driven JSON scene.
/// Every component that is registered in ComponentFactory + TypeRegistry is
/// handled generically — no per-type code. Field types beyond the scalar/
/// string/vector set the reflection layer round-trips are logged and skipped.
std::string SerializeWorld(const World& world);
bool        DeserializeInto(World& world, const std::string& json);
bool        SaveWorld(const World& world, const std::string& path);
bool        LoadWorld(World& world, const std::string& path);
} // namespace Spark
```

- [ ] **Step 2: Write `SerializeWorld` (the reflection walk)**

Create `SparkEngine/Source/SceneManager/ReflectedSceneSerializer.cpp`. Serialize: for every entity in the registry, emit its integer id, name (from `NameComponent`), parent id (from `Transform::parent`, or -1), and for every registered component the entity has, its `type` and a `fields` object built from `GetFieldAsString` over the reflected, serialized, non-vector-of-entities fields. Use nlohmann json (already used across the repo — `#include <nlohmann/json.hpp>`; confirm include path matches existing usage, e.g. `MaterialEditor.cpp`).

```cpp
#include "SceneManager/ReflectedSceneSerializer.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include "Core/Reflection.h"
#include "Utils/LogMacros.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <unordered_map>

using nlohmann::json;

namespace Spark {

namespace {
// Components handled specially at the entity level, not in the generic "components" list.
bool IsEntityLevel(const std::string& type) { return type == "NameComponent"; }

// Emit one component's fields via reflection.
json SerializeComponentFields(const std::string& typeName, const void* comp)
{
    json fields = json::object();
    const TypeInfo* ti = TypeRegistry::Get().FindTypeByName(typeName);
    if (!ti) return fields;
    for (const FieldInfo& f : ti->fields) {
        if (!f.serialized) continue;
        switch (f.type) {
            case FieldType::Bool: case FieldType::Int: case FieldType::Float:
            case FieldType::Double: case FieldType::String:
            case FieldType::Vector2: case FieldType::Vector3: case FieldType::Vector4:
                fields[f.fieldName] = GetFieldAsString(comp, f);
                break;
            default:
                SPARK_LOG_WARN(Spark::LogCategory::Core,
                    "[ReflectedScene] skip unsupported field %s.%s (type %d)",
                    typeName.c_str(), f.fieldName.c_str(), (int)f.type);
                break;
        }
    }
    return fields;
}
} // namespace

std::string SerializeWorld(const World& world)
{
    json root;
    root["version"] = 1;
    json entities = json::array();

    auto& factory = ComponentFactory::Get();
    const std::vector<std::string> names = factory.GetRegisteredNames();
    const entt::registry& reg = world.GetRegistry();
    // Non-const World handle for the factory (its ops take void* world, uint32 entity).
    World& mutWorld = const_cast<World&>(world);

    reg.each([&](entt::entity e) {
        json ent;
        ent["id"] = (uint32_t)e;
        if (const NameComponent* nc = world.GetComponent<NameComponent>(e))
            ent["name"] = nc->name;
        else
            ent["name"] = "";
        int parentId = -1;
        if (const Transform* t = world.GetComponent<Transform>(e))
            if (t->parent != entt::null) parentId = (int)(uint32_t)t->parent;
        ent["parent"] = parentId;

        json comps = json::array();
        for (const std::string& type : names) {
            if (IsEntityLevel(type)) continue; // name handled above
            if (!factory.HasComponent(type, &mutWorld, (uint32_t)e)) continue;
            void* comp = factory.GetComponentRaw(type, &mutWorld, (uint32_t)e);
            if (!comp) continue;
            json c;
            c["type"] = type;
            c["fields"] = SerializeComponentFields(type, comp);
            comps.push_back(std::move(c));
        }
        ent["components"] = std::move(comps);
        entities.push_back(std::move(ent));
    });

    root["entities"] = std::move(entities);
    return root.dump(2);
}

} // namespace Spark
```

- [ ] **Step 3: Register the source with the build**

In top-level `CMakeLists.txt`, add `SparkEngine/Source/SceneManager/ReflectedSceneSerializer.cpp` to the `SPARK_ENGINE_LIB_SOURCES` list (find the existing `SceneManager/SceneManager.cpp` entry and add the new file beside it).

- [ ] **Step 4: Build the engine lib**

Run: `powershell -File "C:\Users\natew\.claude\scripts\build.ps1" "cmake --build build --config Release --target SparkEngineLib"`
Expected: `BUILD_EXIT=0`. (No test yet — deserialize + tests land in A2/A3.)

- [ ] **Step 5: Commit**

```bash
git add SparkEngine/Source/SceneManager/ReflectedSceneSerializer.h SparkEngine/Source/SceneManager/ReflectedSceneSerializer.cpp CMakeLists.txt
git commit -m "feat(scene): reflection-driven SerializeWorld (World -> JSON)"
```

---

### Task A2: `DeserializeInto` + parent resolution + disk wrappers

**Files:**
- Modify: `SparkEngine/Source/SceneManager/ReflectedSceneSerializer.cpp`

**Interfaces:**
- Consumes: `SerializeComponentFields` counterpart via `SetFieldFromString`; `ComponentFactory::AddComponent`, `GetComponentRaw`; `World::CreateEntity`, `GetComponent<Transform>`.
- Produces: `DeserializeInto`, `SaveWorld`, `LoadWorld` (signatures from A1).

- [ ] **Step 1: Implement `DeserializeInto` with a two-pass parent map**

Append to `ReflectedSceneSerializer.cpp` (before the closing `} // namespace Spark`):

```cpp
bool DeserializeInto(World& world, const std::string& jsonText)
{
    json root;
    try { root = json::parse(jsonText); }
    catch (const std::exception& ex) {
        SPARK_LOG_ERROR(Spark::LogCategory::Core, "[ReflectedScene] parse error: %s", ex.what());
        return false;
    }
    if (!root.contains("entities") || !root["entities"].is_array()) return false;

    auto& factory = ComponentFactory::Get();
    std::unordered_map<uint32_t, entt::entity> idMap; // serialized id -> live entity
    struct PendingParent { entt::entity child; uint32_t parentId; };
    std::vector<PendingParent> pending;

    for (const json& ent : root["entities"]) {
        const std::string name = ent.value("name", std::string());
        entt::entity e = world.CreateEntity(name); // emplaces NameComponent when name non-empty
        const uint32_t sid = ent.value("id", 0u);
        idMap[sid] = e;

        if (ent.contains("components") && ent["components"].is_array()) {
            for (const json& c : ent["components"]) {
                const std::string type = c.value("type", std::string());
                if (type.empty() || !factory.IsRegistered(type)) {
                    SPARK_LOG_WARN(Spark::LogCategory::Core,
                        "[ReflectedScene] unknown component type '%s' skipped", type.c_str());
                    continue;
                }
                if (!factory.HasComponent(type, &world, (uint32_t)e))
                    factory.AddComponent(type, &world, (uint32_t)e);
                void* comp = factory.GetComponentRaw(type, &world, (uint32_t)e);
                if (!comp) continue;
                const TypeInfo* ti = TypeRegistry::Get().FindTypeByName(type);
                if (!ti || !c.contains("fields")) continue;
                for (const FieldInfo& f : ti->fields) {
                    auto it = c["fields"].find(f.fieldName);
                    if (it == c["fields"].end() || !it->is_string()) continue;
                    SetFieldFromString(comp, f, it->get<std::string>());
                }
            }
        }
        const int parentId = ent.value("parent", -1);
        if (parentId >= 0) pending.push_back({ e, (uint32_t)parentId });
    }

    // Second pass: resolve parents now that all ids exist.
    for (const PendingParent& p : pending) {
        auto it = idMap.find(p.parentId);
        if (it == idMap.end()) continue;
        Transform* childT = world.GetComponent<Transform>(p.child);
        if (!childT) childT = &world.AddComponent<Transform>(p.child);
        childT->parent = it->second;
        if (Transform* parentT = world.GetComponent<Transform>(it->second))
            parentT->children.push_back(p.child);
    }
    return true;
}

bool SaveWorld(const World& world, const std::string& path)
{
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f << SerializeWorld(world);
    return f.good();
}

bool LoadWorld(World& world, const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return DeserializeInto(world, text);
}
```

- [ ] **Step 2: Build the engine lib**

Run: `powershell -File "C:\Users\natew\.claude\scripts\build.ps1" "cmake --build build --config Release --target SparkEngineLib"`
Expected: `BUILD_EXIT=0`.

- [ ] **Step 3: Commit**

```bash
git add SparkEngine/Source/SceneManager/ReflectedSceneSerializer.cpp
git commit -m "feat(scene): DeserializeInto + parent resolution + Save/LoadWorld"
```

---

### Task A3: Round-trip test (headless)

**Files:**
- Create: `Tests/TestReflectedScene.cpp`
- Modify: `Tests/CMakeLists.txt` (add the file to the `add_executable(SparkTests ...)` source list)

**Interfaces:**
- Consumes: `Spark::SerializeWorld`, `Spark::DeserializeInto`, `Spark::World`, `Transform`, `MeshRenderer`, `NameComponent`.

- [ ] **Step 1: Write the failing round-trip test**

Create `Tests/TestReflectedScene.cpp`:

```cpp
#include "TestFramework.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include "SceneManager/ReflectedSceneSerializer.h"

using namespace Spark;

TEST(ReflectedScene_RoundTrip_TransformAndMesh)
{
    World src;
    EntityID ground = src.CreateEntity("Ground");
    Transform& gt = src.AddComponent<Transform>(ground);
    gt.position = {1.0f, 2.0f, 3.0f};
    gt.scale    = {10.0f, 1.0f, 10.0f};
    MeshRenderer& gm = src.AddComponent<MeshRenderer>(ground);
    gm.meshPath = "Assets/Models/x.obj";
    gm.materialPath = "Assets/Materials/y.json";

    EntityID child = src.CreateEntity("Prop");
    Transform& ct = src.AddComponent<Transform>(child);
    ct.position = {5.0f, 0.0f, -5.0f};
    ct.parent = ground; // child of ground

    const std::string json = SerializeWorld(src);

    World dst;
    EXPECT_TRUE(DeserializeInto(dst, json));

    // Same entity count.
    EXPECT_EQ(dst.GetEntityCount(), (size_t)2);

    // Find the entity named "Ground" and verify its fields survived.
    bool foundGround = false, foundChildParented = false;
    for (auto e : dst.GetEntitiesWith<Transform>()) {
        const NameComponent* nc = dst.GetComponent<NameComponent>(e);
        const Transform* t = dst.GetComponent<Transform>(e);
        if (nc && nc->name == "Ground") {
            foundGround = true;
            EXPECT_NEAR(t->position.x, 1.0f, 0.001f);
            EXPECT_NEAR(t->position.y, 2.0f, 0.001f);
            EXPECT_NEAR(t->position.z, 3.0f, 0.001f);
            EXPECT_NEAR(t->scale.x, 10.0f, 0.001f);
            const MeshRenderer* mr = dst.GetComponent<MeshRenderer>(e);
            EXPECT_TRUE(mr != nullptr);
            EXPECT_STR_CONTAINS(mr->meshPath, "x.obj");
            EXPECT_STR_CONTAINS(mr->materialPath, "y.json");
        }
        if (nc && nc->name == "Prop") {
            EXPECT_TRUE(t->parent != entt::null); // parent resolved
            foundChildParented = true;
        }
    }
    EXPECT_TRUE(foundGround);
    EXPECT_TRUE(foundChildParented);
}
```

- [ ] **Step 2: Add the test source to the SparkTests target**

In `Tests/CMakeLists.txt`, add `TestReflectedScene.cpp` to the `add_executable(SparkTests ...)` source list (beside `TestECSWorld.cpp`).

- [ ] **Step 3: Build the tests, verify it compiles then fails/passes**

Run: `powershell -File "C:\Users\natew\.claude\scripts\build.ps1" "cmake --build build --config Release --target SparkTests"`
Then run: `build\windows-release\bin\SparkTests.exe ReflectedScene_RoundTrip_TransformAndMesh`
Expected: PASS (green). If the reflected Transform/MeshRenderer field names differ from `position`/`meshPath`, the test fails loudly — fix the field name mapping, not the assertion.

- [ ] **Step 4: Commit**

```bash
git add Tests/TestReflectedScene.cpp Tests/CMakeLists.txt
git commit -m "test(scene): reflected-scene round-trip (transform, mesh, parent)"
```

---

### Task A4: Field-type coverage guard

**Files:**
- Modify: `Tests/TestReflectedScene.cpp`

- [ ] **Step 1: Write the coverage test**

Append to `Tests/TestReflectedScene.cpp`. This guards against silent data loss by asserting each supported `FieldType` survives a `GetFieldAsString`→`SetFieldFromString` cycle on a real reflected component (`Camera` covers Float; `MeshRenderer` covers Bool + String; `Transform` covers Vector3):

```cpp
#include "Core/Reflection.h"

TEST(ReflectedScene_FieldCoverage_ScalarsAndVectors)
{
    World w;
    EntityID e = w.CreateEntity("Probe");
    MeshRenderer& mr = w.AddComponent<MeshRenderer>(e);
    mr.visible = false;               // Bool
    mr.meshPath = "a/b/c.obj";        // String
    Transform& t = w.AddComponent<Transform>(e);
    t.rotation = {15.0f, 30.0f, 45.0f}; // Vector3

    const std::string json = SerializeWorld(w);
    World w2;
    EXPECT_TRUE(DeserializeInto(w2, json));

    EntityID e2 = *w2.GetEntitiesWith<MeshRenderer>().begin();
    const MeshRenderer* mr2 = w2.GetComponent<MeshRenderer>(e2);
    EXPECT_FALSE(mr2->visible);
    EXPECT_STR_CONTAINS(mr2->meshPath, "c.obj");
    const Transform* t2 = w2.GetComponent<Transform>(e2);
    EXPECT_NEAR(t2->rotation.y, 30.0f, 0.001f);
}
```

- [ ] **Step 2: Build + run**

Run: `powershell -File "C:\Users\natew\.claude\scripts\build.ps1" "cmake --build build --config Release --target SparkTests"`
Then: `build\windows-release\bin\SparkTests.exe ReflectedScene_FieldCoverage_ScalarsAndVectors`
Expected: PASS.

- [ ] **Step 3: Commit**

```bash
git add Tests/TestReflectedScene.cpp
git commit -m "test(scene): field-type coverage guard (bool/string/vector)"
```

---

# UNIT B — Editor scene renderer (GPU; build + screenshot validation)

> Note on gizmo type: `GizmoSystem::Render/HandleMouseInput` take `std::vector<Transform*>`. **Task B3 Step 1 is an investigation step** to confirm whether that `Transform` is `Spark::Transform` (engine) or an editor-local type, and wire accordingly. No code is written against an unverified type.

### Task B1: `EditorSceneRenderer` draws the World into the Scene View RT

**Files:**
- Create: `SparkEditor/Source/Rendering/EditorSceneRenderer.h`
- Create: `SparkEditor/Source/Rendering/EditorSceneRenderer.cpp`
- Modify: `SparkEditor/Source/Panels/SceneViewPanel.h/.cpp` (hold + call the renderer)
- Modify: `SparkEditor/CMakeLists.txt` (add the .cpp to `SPARK_EDITOR_SOURCES`)

**Interfaces:**
- Consumes: `Spark::World`, `GraphicsEngine` (device-direct draw), `LoadOrPlaceholderMesh` (`Game/PlaceholderMesh.h`), `Mesh`, the panel's `ID3D11Device*/Context*/RTV`.
- Produces:
  ```cpp
  class EditorSceneRenderer {
   public:
    void Initialize(ID3D11Device*, ID3D11DeviceContext*);
    // Draw all entities with Transform+MeshRenderer into `rtv` (already bound+cleared by caller),
    // using the supplied view/proj. Meshes are cached per path.
    void Render(Spark::World& world, const DirectX::XMMATRIX& view,
                const DirectX::XMMATRIX& proj);
    Mesh* GetOrLoadMesh(const std::string& path);
   private:
    ID3D11Device* m_device{}; ID3D11DeviceContext* m_context{};
    std::unordered_map<std::string, std::unique_ptr<Mesh>> m_meshCache;
  };
  ```

- [ ] **Step 1: Write the renderer header**

Create `SparkEditor/Source/Rendering/EditorSceneRenderer.h` with the class above (`#include <DirectXMath.h>`, `<memory>`, `<unordered_map>`, `<string>`; forward-declare `Spark::World`, `Mesh`, `GraphicsEngine`).

- [ ] **Step 2: Implement the renderer**

Create `SparkEditor/Source/Rendering/EditorSceneRenderer.cpp`. Mirror the proven `TFWorldSetup::GetOrLoadEcsMesh` + basic-shader draw. The editor exe links `SparkEngineLib`, so it can construct/use a `GraphicsEngine` OR draw directly with the device + `SetBasicShaders`. Use the GraphicsEngine the editor already owns (obtained the same way SceneViewPanel got its device — via `EditorUI`; pass a `GraphicsEngine*` into `Initialize` if available, else fall back to raw-device basic draw). Concretely:

```cpp
#include "Rendering/EditorSceneRenderer.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"
#include "Game/PlaceholderMesh.h"

using namespace DirectX;

void EditorSceneRenderer::Initialize(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{ m_device = dev; m_context = ctx; }

Mesh* EditorSceneRenderer::GetOrLoadMesh(const std::string& path)
{
    if (path.empty() || !m_device) return nullptr;
    if (auto it = m_meshCache.find(path); it != m_meshCache.end()) return it->second.get();
    auto mesh = std::make_unique<Mesh>();
    LoadOrPlaceholderMesh(*mesh, m_device, m_context, std::wstring(path.begin(), path.end()));
    Mesh* raw = mesh.get();
    m_meshCache.emplace(path, std::move(mesh));
    return raw;
}

void EditorSceneRenderer::Render(Spark::World& world, const XMMATRIX& view, const XMMATRIX& proj)
{
    // The editor owns a GraphicsEngine (Unit B1 Step 3 wires it in). Here we assume a
    // module-scope GraphicsEngine* g set at Initialize; draw each mesh entity.
    extern GraphicsEngine* EditorGraphics(); // provided by EditorUI accessor
    GraphicsEngine* g = EditorGraphics();
    if (!g) return;
    g->SetBasicShaders();
    for (auto e : world.GetEntitiesWith<Spark::Transform, Spark::MeshRenderer>()) {
        const Spark::MeshRenderer* mr = world.GetComponent<Spark::MeshRenderer>(e);
        if (!mr->visible) continue;
        Mesh* mesh = GetOrLoadMesh(mr->meshPath);
        if (!mesh || mesh->GetIndexCount() == 0) continue;
        const Spark::Transform* t = world.GetComponent<Spark::Transform>(e);
        const XMMATRIX wmat = t->GetWorldMatrix(world.GetRegistry());
        ID3D11ShaderResourceView* srv =
            mr->materialPath.empty() ? nullptr : g->GetOrLoadTextureSRV(mr->materialPath);
        g->UpdateBasicConstants(wmat, view, proj, XMFLOAT4(1,1,1,1), XMFLOAT2(1,1));
        g->SetBasicTexture(srv);
        mesh->Render(m_context);
    }
    g->SetBasicTexture(nullptr);
}
```

> The `EditorGraphics()` accessor + `GraphicsEngine` ownership is wired in Step 3. If the editor does not already own a `GraphicsEngine`, create one in `EditorUI` from the existing D3D11 device (`GraphicsEngine` has an init-from-device path used by the runtime; confirm and call it). Material-path handling here reads the material JSON's albedo through `GetOrLoadTextureSRV` only if `materialPath` is a texture; for a `.json` material, resolve via the engine's `GetOrLoadBasicMaterial(materialPath)` (see `GraphicsEngine.h:702`) and use its `.srv`. Use `GetOrLoadBasicMaterial` when the path ends in `.json`, else `GetOrLoadTextureSRV`.

- [ ] **Step 3: Give the editor a GraphicsEngine + `EditorGraphics()` accessor**

In `EditorUI` add `std::unique_ptr<GraphicsEngine> m_graphics;` initialized from the editor's D3D11 device during startup (same device passed to `SceneViewPanel::SetDevice`). Add a free function `GraphicsEngine* EditorGraphics()` (defined in `EditorUI.cpp`, returning the singleton EditorUI's `m_graphics.get()`), declared `extern` where used. Have `SceneViewPanel` own an `EditorSceneRenderer m_sceneRenderer;`, `Initialize` it in `SetDevice`, and pass the current `Spark::World*` (added in Unit C1; until then, a renderer-owned demo world with one cube entity for this task's screenshot).

- [ ] **Step 4: Call the renderer from `SceneViewPanel::RenderSceneContent`**

In `SceneViewPanel.cpp:294`, after binding `m_rtv` and clearing, replace the "clear only" body's ground/sky fill with: compute a view/proj from the panel camera (existing `UpdateCamera` state), then `m_sceneRenderer.Render(*world, view, proj);` before restoring the previous RTV. For this task, render a demo world: one entity with `MeshRenderer.meshPath = "Assets/Models/MMOFPS/characters/soldier.obj"` at identity, to prove geometry draws.

- [ ] **Step 5: Register source + build the editor**

Add `Source/Rendering/EditorSceneRenderer.cpp` to `SPARK_EDITOR_SOURCES` in `SparkEditor/CMakeLists.txt`. Run:
`powershell -File "C:\Users\natew\.claude\scripts\build.ps1" "cmake --build build --config Release --target SparkEditor"`
Expected: `BUILD_EXIT=0`.

- [ ] **Step 6: Screenshot validation**

Launch the editor, open the Scene View panel. Expected: the soldier mesh renders in the viewport instead of the blue/green split. Capture the window (Alt+PrtScn or the editor's screenshot if present) and confirm visible geometry. Record the result in the commit message.

- [ ] **Step 7: Commit**

```bash
git add SparkEditor/Source/Rendering/EditorSceneRenderer.h SparkEditor/Source/Rendering/EditorSceneRenderer.cpp SparkEditor/Source/Panels/SceneViewPanel.h SparkEditor/Source/Panels/SceneViewPanel.cpp SparkEditor/Source/Core/EditorUI.h SparkEditor/Source/Core/EditorUI.cpp SparkEditor/CMakeLists.txt
git commit -m "feat(editor): render real ECS meshes in the Scene View (device-direct)"
```

---

### Task B2: Editor fly-camera

**Files:**
- Modify: `SparkEditor/Source/Panels/SceneViewPanel.cpp` (`UpdateCamera`, `.cpp:515`)

- [ ] **Step 1: Replace orbit-proxy with a fly-camera**

In `UpdateCamera`, when the viewport is hovered and RMB is held: mouse-delta yaws/pitches a stored `m_camYaw/m_camPitch`; WASD moves `m_camPos` along the camera basis; scroll adjusts speed. Build `view = XMMatrixLookToLH(m_camPos, forward, up)` and `proj = XMMatrixPerspectiveFovLH(60°, aspect, 0.1f, 6000.f)` and expose them via getters used by `RenderSceneContent`. Add `XMFLOAT3 m_camPos{0,3,-8}; float m_camYaw{0}, m_camPitch{0.2f};` members.

- [ ] **Step 2: Build + validate**

Build `SparkEditor`. Launch, hold RMB + WASD in the viewport. Expected: camera flies around the rendered soldier smoothly. 

- [ ] **Step 3: Commit**

```bash
git add SparkEditor/Source/Panels/SceneViewPanel.cpp SparkEditor/Source/Panels/SceneViewPanel.h
git commit -m "feat(editor): fly-camera (RMB-look + WASD) in Scene View"
```

---

### Task B3: Gizmo overlay on the selected entity

**Files:**
- Modify: `SparkEditor/Source/Panels/SceneViewPanel.cpp`
- Read first: `SparkEditor/Source/Gizmos/GizmoSystem.h`

- [ ] **Step 1: Confirm the gizmo's `Transform*` type**

Open `GizmoSystem.h` and the top of `GizmoSystem.cpp`; determine whether `std::vector<Transform*>` is `Spark::Transform` (engine, `CoreComponents.h`) or an editor-local `Transform`. Record which. If it is `Spark::Transform`, proceed to Step 2 as written. If it is an editor-local type, add an adapter in Step 2 that copies position/rotation/scale from the engine `Spark::Transform` into a scratch gizmo-`Transform`, runs the gizmo, then writes the mutated pos/rot/scale back to the engine component.

- [ ] **Step 2: Feed the selected entity's transform to the gizmo each frame**

After `m_sceneRenderer.Render(...)`, get the selected `EntityID` (from Unit C's selection; until C lands, the demo entity), fetch its `Spark::Transform*`, and call the editor's gizmo (`EditorUI::GetGizmoSystem()`):

```cpp
GizmoSystem* gizmo = EditorUI::Instance().GetGizmoSystem();
std::vector<Spark::Transform*> sel{ selectedTransform }; // or scratch adapter per Step 1
XMFLOAT4 viewport{ (float)panelX, (float)panelY, (float)w, (float)h };
gizmo->Render(sel, view, proj, viewport);
gizmo->HandleMouseInput(mouseX, mouseY, mouseDown, view, proj, viewport, sel);
```

The gizmo mutates the transform in place (its own undo snapshots apply). Toolbar mode buttons already call `SetGizmoMode`.

- [ ] **Step 3: Build + validate**

Build `SparkEditor`. Launch, select the entity, drag the gizmo. Expected: the mesh translates/rotates/scales with the handles; the change persists.

- [ ] **Step 4: Commit**

```bash
git add SparkEditor/Source/Panels/SceneViewPanel.cpp
git commit -m "feat(editor): gizmo overlay drives the selected entity's transform"
```

---

# UNIT C — Inspector + Hierarchy on the live World

### Task C1: EditorUI owns a `Spark::World` document

**Files:**
- Modify: `SparkEditor/Source/Core/EditorUI.h/.cpp`

**Interfaces:**
- Produces: `Spark::World* EditorUI::GetWorld()`; a `NewScene()` that resets the world; panels receive `World*`.

- [ ] **Step 1: Add the world + accessor**

In `EditorUI.h` add `#include "Engine/ECS/Components.h"`, member `std::unique_ptr<Spark::World> m_world;`, and `Spark::World* GetWorld() { return m_world.get(); }`. In `EditorUI` init, `m_world = std::make_unique<Spark::World>();` and seed it with one demo entity (soldier MeshRenderer) so the viewport shows content. Pass `m_world.get()` to `SceneViewPanel`, `HierarchyPanel`, `InspectorPanel` via new `SetWorld(Spark::World*)` setters.

- [ ] **Step 2: Build + commit**

Build `SparkEditor` (expect `BUILD_EXIT=0`).
```bash
git add SparkEditor/Source/Core/EditorUI.h SparkEditor/Source/Core/EditorUI.cpp
git commit -m "feat(editor): EditorUI owns a live Spark::World document"
```

---

### Task C2: Hierarchy over the World

**Files:**
- Modify: `SparkEditor/Source/Panels/HierarchyPanel.h/.cpp`

- [ ] **Step 1: Add a world-backed listing + create/rename/delete**

Add `void SetWorld(Spark::World* w);` and `Spark::World* m_world`. When `m_world` is set, the panel's tree enumerates `m_world->GetRegistry().each(...)`, labeling each row from `NameComponent`, drawing parent/child nesting from `Transform::parent/children`. Selection publishes the `EntityID` (as the id `SelectionManager` carries). Create → `m_world->CreateEntity("Entity")` + `AddComponent<Transform>`. Delete → `m_world->DestroyEntity`. Rename → edit `NameComponent::name`. Keep these on `CommandHistory` using the same `LambdaCommand` pattern already in the panel.

- [ ] **Step 2: Build + validate + commit**

Build `SparkEditor`. Launch: create/rename/delete entities in the Hierarchy; confirm the Scene View reflects new mesh entities when you add a MeshRenderer (via Inspector, Task C3).
```bash
git add SparkEditor/Source/Panels/HierarchyPanel.h SparkEditor/Source/Panels/HierarchyPanel.cpp
git commit -m "feat(editor): Hierarchy lists/creates/deletes real ECS entities"
```

---

### Task C3: Inspector over the selected entity's components

**Files:**
- Modify: `SparkEditor/Source/Panels/InspectorPanel.h/.cpp`, `InspectorComponentRenderers_Reflected.cpp`

- [ ] **Step 1: Render the selected entity's components via TypeRegistry**

Add `void SetWorld(Spark::World*)` and, when a world + a selected `EntityID` are present, replace the `SceneFile`-driven body with: for each `type` in `ComponentFactory::Get().GetRegisteredNames()`, if `HasComponent(type, world, entity)`, draw a collapsing header `type` and call `RenderReflectedFields(GetComponentRaw(type, world, entity), TypeRegistry::Get().FindTypeByName(type)->fields)`. The generic renderer already edits by `field.offset`. Add-Component menu lists `GetRegisteredNames()`; picking one calls `factory.AddComponent(type, world, entity)`.

- [ ] **Step 2: Route edits through CommandHistory**

`RenderReflectedFields` writes directly to component memory. Wrap each committed edit in the existing `CommandHistory::GetInstance().Execute(LambdaCommand(do, undo, desc))` pattern, capturing the field's before/after string (`GetFieldAsString`) and re-applying via `SetFieldFromString` on undo/redo (offset+size are known from `FieldInfo`).

- [ ] **Step 3: Build + validate + commit**

Build `SparkEditor`. Launch: select an entity, edit its Transform position — the mesh moves in the viewport; Ctrl+Z reverts.
```bash
git add SparkEditor/Source/Panels/InspectorPanel.h SparkEditor/Source/Panels/InspectorPanel.cpp SparkEditor/Source/Panels/InspectorComponentRenderers_Reflected.cpp
git commit -m "feat(editor): Inspector edits real ECS components via reflection + undo"
```

---

### Task C4: Wire Save/Open to the serializer

**Files:**
- Modify: `SparkEditor/Source/Core/EditorUI.cpp` (`SaveCurrentScene`, and the Open path)

- [ ] **Step 1: Save via `SaveWorld`, Open via `LoadWorld`**

Replace the lossy body of `EditorUI::SaveCurrentScene(path)` with `return Spark::SaveWorld(*m_world, path);` (include `SceneManager/ReflectedSceneSerializer.h`). For Open, add `bool OpenScene(path)` → `m_world = std::make_unique<Spark::World>(); bool ok = Spark::LoadWorld(*m_world, path);` then re-point the panels' `SetWorld(m_world.get())`.

- [ ] **Step 2: Build + validate the editor round-trip**

Build `SparkEditor`. Launch, create 2 entities with meshes + transforms, `Ctrl+S` to `test_roundtrip.scene.json`, then Open it into a fresh session. Expected: identical hierarchy + transforms + meshes rendered. Confirm the JSON on disk contains the component fields (not just names).

- [ ] **Step 3: Commit**

```bash
git add SparkEditor/Source/Core/EditorUI.cpp SparkEditor/Source/Core/EditorUI.h
git commit -m "feat(editor): Ctrl+S/Open use the reflected-scene serializer (full fidelity)"
```

---

# UNIT D — Runtime loads the same file (acceptance)

### Task D1: `-scene <path>` on SparkEngine.exe

**Files:**
- Modify: `SparkEngine/Source/Core/SparkEngineWindows.cpp` (+ `SparkEngineLinux.cpp` arg parser, `:119-179` region)

**Interfaces:**
- Consumes: `Spark::LoadWorld`, the engine's default (no-module) render loop, `EditorSceneRenderer`'s draw logic (extract the mesh-draw loop from Task B1 into a shared free function `Spark::RenderWorldBasic(World&, GraphicsEngine&, view, proj)` in `SparkEngine/Source/Graphics/` so both editor and runtime call it — do this extraction as Step 1).

- [ ] **Step 1: Extract the generic draw into `SparkEngineLib`**

Create `SparkEngine/Source/Graphics/WorldBasicRenderer.{h,cpp}` exposing:
```cpp
namespace Spark {
  void RenderWorldBasic(World& world, GraphicsEngine& g, class WorldMeshCache& cache,
                        const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj);
  class WorldMeshCache { public: Mesh* GetOrLoad(GraphicsEngine&, const std::string& path);
    private: std::unordered_map<std::string, std::unique_ptr<Mesh>> m_cache; };
}
```
Move the Task-B1 draw loop body here. Update `EditorSceneRenderer::Render` to call `RenderWorldBasic`. Add the .cpp to `SPARK_ENGINE_LIB_SOURCES`. Build `SparkEngineLib` + `SparkEditor` (expect green; editor still renders).

- [ ] **Step 2: Parse `-scene <path>` and render it**

In the arg parser next to `-window-size` (`SparkEngineLinux.cpp:148`), recognize `-scene <path>` into a `std::string g_scenePath`. When set and no `-game` module is present, after graphics init the engine's main loop should: `World w; LoadWorld(w, g_scenePath);` and each frame `BeginFrame; RenderWorldBasic(w, *graphics, cache, editorlessView, proj); EndFrame;` with a fixed overhead camera (`XMMatrixLookAtLH({mapCenterX, 300, mapCenterZ-1}, {mapCenterX,0,mapCenterZ}, up)`). Reuse the existing `-test-frames N`/screenshot harness for capture.

- [ ] **Step 3: Build + validate load path**

Build `SparkEngine`:
`powershell -File "C:\Users\natew\.claude\scripts\build.ps1" "cmake --build build --config Release --target SparkEngine"`
Run: `build\windows-release\bin\SparkEngine.exe -scene test_roundtrip.scene.json -window-size 1280 720 -test-frames 60`
Expected: the runtime window shows the same meshes authored in the editor.

- [ ] **Step 4: Commit**

```bash
git add SparkEngine/Source/Graphics/WorldBasicRenderer.h SparkEngine/Source/Graphics/WorldBasicRenderer.cpp SparkEditor/Source/Rendering/EditorSceneRenderer.cpp SparkEngine/Source/Core/SparkEngineWindows.cpp SparkEngine/Source/Core/SparkEngineLinux.cpp CMakeLists.txt
git commit -m "feat(runtime): -scene loads + renders a reflected scene via shared WorldBasicRenderer"
```

---

### Task D2: Acceptance — editor→file→runtime identical

**Files:**
- Create: `docs/superpowers/acceptance/2026-07-05-roundtrip.md` (record the evidence)

- [ ] **Step 1: Author in the editor**

Launch `SparkEditor`. Create a ground entity (MeshRenderer = a plane/terrain OBJ) + a soldier entity offset on it; move both with the gizmo; `Ctrl+S` → `acceptance.scene.json`. Screenshot the editor viewport.

- [ ] **Step 2: Load in the runtime**

Run `build\windows-release\bin\SparkEngine.exe -scene acceptance.scene.json -window-size 1280 720 -test-frames 90` and capture a frame (existing screenshot harness).

- [ ] **Step 3: Compare + record**

Place the two screenshots side by side; confirm the same entities at the same relative positions. Write the result + both image paths into `docs/superpowers/acceptance/2026-07-05-roundtrip.md`.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/acceptance/2026-07-05-roundtrip.md
git commit -m "docs(editor): sub-project 1 acceptance — editor->file->runtime round-trip verified"
```

---

## Self-review notes

- **Spec coverage:** Goal 1 (render real World) → B1–B3; Goal 2 (inspect real components) → C3; Goal 3 (hierarchy of real entities) → C2; Goal 4 (full-fidelity Ctrl+S) → A1–A2 + C4; Goal 5 (runtime loads identically) → D1–D2. Testing section → A3–A4 (headless), B/C/D (build+screenshot). Non-goals (asset pickers, drag-drop, scripts, TERRAFRONT migration) are untouched.
- **Field-type gap** is explicit (Global Constraints + A4 guard): Enum/Custom logged+skipped this slice.
- **Two known integration unknowns are handled as investigation steps, not placeholders:** the gizmo `Transform*` type (B3 Step 1) and whether the editor already owns a `GraphicsEngine` (B1 Step 3). Both have concrete fallbacks written inline.
- **Type consistency:** `SerializeWorld/DeserializeInto/SaveWorld/LoadWorld` names are stable across A1→A2→A3→C4→D1. `EditorSceneRenderer::Render(World&, view, proj)` stable B1→B3→D1 (superseded by `RenderWorldBasic` in D1, with the editor updated in the same task).
