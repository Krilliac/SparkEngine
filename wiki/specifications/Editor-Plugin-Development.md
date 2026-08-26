# Editor Plugin Development

This page documents how to create plugins for SparkEditor, including the plugin interface, lifecycle, registration, and extension points.

**Source:** `SparkEditor/Source/Core/IEditorPlugin.h`, `EditorPluginManager.h`, `EditorPanel.h`

---

## Overview

SparkEditor supports two kinds of plugins:

| Type | Registration | Use case |
|------|-------------|----------|
| **Built-in C++** | `RegisterPlugin<T>()` at compile time | Engine-shipped panels and tools |
| **Stable C ABI** | `LoadPlugin(path)` at runtime | Third-party lifecycle/tick extensions |

Built-ins implement `IEditorPlugin`. Native shared libraries use
`Spark/PluginABI.h`; they never exchange C++ objects, STL types, exceptions,
allocations, or vtables with the editor.

---

## Built-In Plugin Interface

Every built-in plugin implements `IEditorPlugin`:

```cpp
class IEditorPlugin {
public:
    virtual ~IEditorPlugin() = default;

    // Required — identification
    virtual const char* GetName() const = 0;
    virtual const char* GetVersion() const = 0;

    // Required — lifecycle
    virtual bool Initialize(EditorApplication* app) = 0;
    virtual void Shutdown() = 0;
    virtual void Update(float deltaTime) = 0;
    virtual void OnGUI() = 0;

    // Optional — event hooks (default no-op)
    virtual void OnSceneLoad(const std::string& scenePath) {}
    virtual void OnSceneSave(const std::string& scenePath) {}
    virtual void OnEntitySelected(uint32_t entityID) {}
    virtual void OnMenuBar() {}
};
```

### Lifecycle

1. **`Initialize(app)`** — Called once when the plugin is loaded. Use `app` to access editor subsystems. Return `false` to abort loading.
2. **`Update(deltaTime)`** — Called every frame. Perform non-UI logic here.
3. **`OnGUI()`** — Called every frame during ImGui rendering. Draw your UI here.
4. **`Shutdown()`** — Called when the plugin is unloaded or the editor exits.

### Event Hooks

The plugin manager broadcasts editor events to all loaded plugins:

| Hook | When fired |
|------|-----------|
| `OnSceneLoad(path)` | After a scene file is loaded |
| `OnSceneSave(path)` | After a scene file is saved |
| `OnEntitySelected(id)` | When the user selects an entity in the hierarchy |
| `OnMenuBar()` | During main menu bar rendering (add custom menu items) |

---

## Creating a Built-In Plugin

### Step 1: Implement the interface

```cpp
// MyToolPlugin.h
#pragma once
#include "Core/IEditorPlugin.h"

class MyToolPlugin : public IEditorPlugin {
public:
    const char* GetName() const override { return "My Tool"; }
    const char* GetVersion() const override { return "1.0.0"; }

    bool Initialize(EditorApplication* app) override {
        m_app = app;
        return true;
    }

    void Shutdown() override {}

    void Update(float deltaTime) override {
        // Non-UI logic
    }

    void OnGUI() override {
        if (ImGui::Begin("My Tool Window")) {
            ImGui::Text("Hello from my plugin!");
        }
        ImGui::End();
    }

private:
    EditorApplication* m_app = nullptr;
};
```

### Step 2: Register with the plugin manager

In `EditorApplication` initialization:

```cpp
m_pluginManager.RegisterPlugin<MyToolPlugin>();
```

---

## Creating a Native Shared-Library Plugin

Use the SDK helper `spark_add_plugin(...)` and implement the stable C entry
point. The generated sibling `.sparkplugin.json` records the ABI version read
directly from `Spark/PluginABI.h`, binary name, identity, and SHA-256. The editor
verifies that metadata before mapping the native image.

```cpp
#include <Spark/PluginABI.h>

namespace {
SparkPluginResult Create(const SparkPluginHostAPI*, SparkPluginInstance* out) {
    if (!out) return SPARK_PLUGIN_ERROR_INVALID_ARGUMENT;
    *out = 1;
    return SPARK_PLUGIN_OK;
}
void Destroy(SparkPluginInstance) {}
SparkPluginResult Tick(SparkPluginInstance, double) { return SPARK_PLUGIN_OK; }

const SparkPluginAPI api = {
    sizeof(SparkPluginAPI), SPARK_PLUGIN_ABI_MAJOR, SPARK_PLUGIN_ABI_MINOR, 0,
    SPARK_PLUGIN_CAP_EDITOR_EXTENSION | SPARK_PLUGIN_CAP_TICK,
    &Create, &Destroy, nullptr, nullptr, &Tick,
    nullptr, nullptr, nullptr, nullptr, {}
};

const SparkPluginDescriptor descriptor = {
    sizeof(SparkPluginDescriptor), SPARK_PLUGIN_ABI_MAGIC,
    SPARK_PLUGIN_ABI_MAJOR, SPARK_PLUGIN_ABI_MINOR, 1, 0,
    "org.example.my-tool", "My Tool", "Example", "1.0.0", &api, {}
};
}

SPARK_DECLARE_PLUGIN_ENTRY_POINT() {
    if (host_abi_major != SPARK_PLUGIN_ABI_MAJOR ||
        host_abi_minor < SPARK_PLUGIN_ABI_MINOR) return nullptr;
    return &descriptor;
}
```

```cmake
find_package(SparkEngine REQUIRED)
spark_add_plugin(MyTool
    ID "org.example.my-tool"
    VERSION "1.0.0"
    TYPE "editor-extension"
    SOURCES MyTool.cpp)
```

Load at runtime:

```cpp
m_pluginManager.LoadPlugin("plugins/MyDLLPlugin.dll");
```

Unload by descriptor name or stable ID:

```cpp
m_pluginManager.UnloadPlugin("My Tool");
```

The current stable ABI supports lifecycle, task/resource host services,
per-frame ticks, and transactional hot reload. ImGui/panel hooks remain a
built-in C++ surface until an append-only C UI service table is standardized.

---

## Plugin Manager API

```cpp
class EditorPluginManager {
public:
    // Registration
    template<typename T> bool RegisterPlugin();
    bool LoadPlugin(const std::string& path);
    bool UnloadPlugin(const std::string& name);

    // Query
    IEditorPlugin* GetPlugin(const std::string& name) const;
    const SparkPluginDescriptor* GetDynamicPluginDescriptor(const std::string& nameOrId) const;
    size_t GetPluginCount() const;
    std::vector<std::string> GetPluginNames() const;

    // Lifecycle (called by EditorApplication)
    bool InitializeAll(EditorApplication* app);
    void ShutdownAll();
    void UpdateAll(float deltaTime);
    void RenderAll();

    // Event broadcasting
    void NotifySceneLoad(const std::string& scenePath);
    void NotifySceneSave(const std::string& scenePath);
    void NotifyEntitySelected(uint32_t entityID);
    void RenderMenuBarItems();

    // Panel registration (plugins can add panels)
    void RegisterPanel(std::unique_ptr<EditorPanel> panel);
};
```

---

## Adding Custom Editor Panels

Plugins can register new editor panels that integrate with the panel system:

### Step 1: Inherit EditorPanel

```cpp
#include "Core/EditorPanel.h"

class MyPanel : public EditorPanel {
public:
    MyPanel() : EditorPanel("My Panel", "my_panel_id") {}

    bool Initialize() override { return true; }
    void Update(float dt) override {}

    void Render() override {
        if (!IsVisible()) return;
        if (BeginPanel()) {
            ImGui::Text("Panel content here");
        }
        EndPanel();
    }

    void Shutdown() override {}
};
```

### Step 2: Register from your plugin

```cpp
bool MyPlugin::Initialize(EditorApplication* app) {
    auto panel = std::make_unique<MyPanel>();
    app->GetPluginManager().RegisterPanel(std::move(panel));
    return true;
}
```

---

## Adding Custom Asset Processors

Plugins can extend the asset pipeline with custom format importers:

```cpp
#include "AssetPipeline/AdvancedAssetPipeline.h"

class MyFormatProcessor : public AssetProcessor {
public:
    std::string GetName() const override { return "My Format"; }

    std::vector<std::string> GetSupportedExtensions() const override {
        return {".myformat"};
    }

    AssetType GetAssetType() const override { return AssetType::Mesh; }

    bool Process(AssetMetadata& metadata,
                 const AssetImportSettings& settings,
                 std::function<void(float)> progress) override {
        // Parse .myformat file and populate metadata
        return true;
    }

    bool GenerateThumbnail(const AssetMetadata& metadata, int size) override {
        return false;  // No thumbnail support
    }

    bool Validate(const AssetMetadata& metadata) override {
        return true;
    }
};

// Register
pipeline.RegisterProcessor(std::make_unique<MyFormatProcessor>());
```

---

## Adding Menu Items

Use the `OnMenuBar()` hook to add items to the editor's main menu:

```cpp
void MyPlugin::OnMenuBar() {
    if (ImGui::BeginMenu("My Plugin")) {
        if (ImGui::MenuItem("Open Tool Window"))
            m_showWindow = true;
        if (ImGui::MenuItem("Run Analysis"))
            RunAnalysis();
        ImGui::EndMenu();
    }
}
```

---

## Best Practices

1. **Keep plugins self-contained.** Don't modify engine internals — use the provided APIs.
2. **Handle initialization failure gracefully.** Return `false` from `Initialize()` if dependencies are missing.
3. **Clean up in `Shutdown()`.** Release all resources, unregister callbacks.
4. **Use ImGui for UI.** All editor UI uses Dear ImGui — use it for consistency.
5. **Version your plugins.** The `GetVersion()` method helps track compatibility.
6. **Don't block in `Update()`.** Long operations should be async or spread across frames.

See [SparkEditor](../gameplay-tools/SparkEditor.md) for the full editor architecture.
