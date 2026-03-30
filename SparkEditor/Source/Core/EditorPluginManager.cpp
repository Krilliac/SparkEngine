/**
 * @file EditorPluginManager.cpp
 * @brief Implementation of the editor plugin manager (R7.1)
 * @author Spark Engine Team
 * @date 2025
 */

#include "EditorPluginManager.h"
#include "EditorPanel.h"
#include "Core/FaultIsolation.h"
#include "Utils/Validate.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#endif // SPARK_PLATFORM_WINDOWS
#else
#include <dlfcn.h>
#endif

namespace SparkEditor
{

    EditorPluginManager::~EditorPluginManager()
    {
        ShutdownAll();
    }

    // ----- Registration -----

    bool EditorPluginManager::RegisterPluginInstance(std::unique_ptr<IEditorPlugin> plugin, bool isFromDLL,
                                                     void* libraryHandle, DestroyEditorPluginFn destroyFn)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        if (!plugin)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "EditorPluginManager: Cannot register null plugin");
            return false;
        }

        const std::string name = plugin->GetName();

        // Check for duplicate names
        if (FindPlugin(name) != m_plugins.end())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "EditorPluginManager: Plugin '%s' is already registered",
                           name.c_str());
            return false;
        }

        PluginEntry entry;
        entry.plugin = std::move(plugin);
        entry.isFromDLL = isFromDLL;
        entry.libraryHandle = libraryHandle;
        entry.destroyFn = destroyFn;
        entry.isInitialized = false;

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "EditorPluginManager: Registered plugin '%s' v%s", name.c_str(),
                       entry.plugin->GetVersion());
        m_plugins.push_back(std::move(entry));
        return true;
    }

    bool EditorPluginManager::LoadPlugin(const std::string& path)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !path.empty(), false);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Loading plugin from '%s'", path.c_str());

        // Security: reject path traversal sequences
        if (path.contains(".."))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "Plugin path rejected — contains '..' traversal: %s",
                            path.c_str());
            return false;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "EditorPluginManager: Loading plugin from '%s'...", path.c_str());

        void* handle = nullptr;
        CreateEditorPluginFn createFn = nullptr;
        DestroyEditorPluginFn destroyFn = nullptr;

#ifdef _WIN32
        handle = LoadLibraryA(path.c_str());
        if (!handle)
        {
            DWORD err = GetLastError();
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "EditorPluginManager: Failed to load '%s' (error %lu)",
                            path.c_str(), err);
            return false;
        }

        createFn =
            reinterpret_cast<CreateEditorPluginFn>(GetProcAddress(static_cast<HMODULE>(handle), "CreateEditorPlugin"));
        destroyFn = reinterpret_cast<DestroyEditorPluginFn>(
            GetProcAddress(static_cast<HMODULE>(handle), "DestroyEditorPlugin"));
#else
        handle = dlopen(path.c_str(), RTLD_NOW);
        if (!handle)
        {
            const char* err = dlerror();
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "EditorPluginManager: Failed to load '%s': %s", path.c_str(),
                            (err ? err : "unknown error"));
            return false;
        }

        createFn = reinterpret_cast<CreateEditorPluginFn>(dlsym(handle, "CreateEditorPlugin"));
        destroyFn = reinterpret_cast<DestroyEditorPluginFn>(dlsym(handle, "DestroyEditorPlugin"));
#endif

        if (!createFn || !destroyFn)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor,
                            "EditorPluginManager: Plugin '%s' missing required exports "
                            "(CreateEditorPlugin/DestroyEditorPlugin)",
                            path.c_str());
            UnloadLibrary(handle);
            return false;
        }

        IEditorPlugin* rawPlugin = createFn();
        if (!rawPlugin)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor,
                            "EditorPluginManager: CreateEditorPlugin() returned null for '%s'", path.c_str());
            UnloadLibrary(handle);
            return false;
        }

        // Wrap in unique_ptr with custom deleter awareness — the destroy function
        // is stored in PluginEntry and called during unload rather than via unique_ptr deleter.
        auto plugin = std::unique_ptr<IEditorPlugin>(rawPlugin);
        return RegisterPluginInstance(std::move(plugin), /*isFromDLL=*/true, handle, destroyFn);
    }

    bool EditorPluginManager::UnloadPlugin(const std::string& name)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !name.empty(), false);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Unloading plugin '%s'", name.c_str());
        auto it = FindPlugin(name);
        if (it == m_plugins.end())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "EditorPluginManager: Plugin '%s' not found", name.c_str());
            return false;
        }

        // Shutdown if initialized
        if (it->isInitialized)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "EditorPluginManager: Shutting down plugin '%s'...",
                           name.c_str());
            it->plugin->Shutdown();
            it->isInitialized = false;
        }

        // For DLL plugins, call the destroy function before releasing the unique_ptr
        if (it->isFromDLL && it->destroyFn)
        {
            it->destroyFn(it->plugin.release());
        }

        void* handle = it->libraryHandle;
        m_plugins.erase(it);

        // Unload the library after erasing the entry
        if (handle)
        {
            UnloadLibrary(handle);
        }

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "EditorPluginManager: Unloaded plugin '%s'", name.c_str());
        return true;
    }

    // ----- Lookup -----

    IEditorPlugin* EditorPluginManager::GetPlugin(const std::string& name) const
    {
        auto it = FindPlugin(name);
        if (it != m_plugins.end())
        {
            return it->plugin.get();
        }
        return nullptr;
    }

    size_t EditorPluginManager::GetPluginCount() const
    {
        return m_plugins.size();
    }

    std::vector<std::string> EditorPluginManager::GetPluginNames() const
    {
        std::vector<std::string> names;
        names.reserve(m_plugins.size());
        for (const auto& entry : m_plugins)
        {
            names.emplace_back(entry.plugin->GetName());
        }
        return names;
    }

    // ----- Lifecycle -----

    bool EditorPluginManager::InitializeAll(EditorApplication* app)
    {
        bool allSucceeded = true;

        for (auto& entry : m_plugins)
        {
            if (entry.isInitialized)
            {
                continue;
            }

            const std::string name = entry.plugin->GetName();
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "EditorPluginManager: Initializing plugin '%s'...",
                           name.c_str());

            if (entry.plugin->Initialize(app))
            {
                entry.isInitialized = true;
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "EditorPluginManager: Plugin '%s' initialized successfully",
                               name.c_str());
            }
            else
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Editor, "EditorPluginManager: Plugin '%s' failed to initialize",
                                name.c_str());
                allSucceeded = false;
            }
        }

        return allSucceeded;
    }

    void EditorPluginManager::ShutdownAll()
    {
        // Shutdown in reverse order
        for (auto it = m_plugins.rbegin(); it != m_plugins.rend(); ++it)
        {
            if (it->isInitialized)
            {
                const std::string name = it->plugin->GetName();
                SPARK_LOG_INFO(Spark::LogCategory::Editor, "EditorPluginManager: Shutting down plugin '%s'...",
                               name.c_str());
                it->plugin->Shutdown();
                it->isInitialized = false;
            }

            // For DLL plugins, call destroy before releasing
            if (it->isFromDLL && it->destroyFn)
            {
                it->destroyFn(it->plugin.release());
            }
        }

        // Unload all DLL handles after destroying plugin objects
        for (auto& entry : m_plugins)
        {
            if (entry.libraryHandle)
            {
                UnloadLibrary(entry.libraryHandle);
                entry.libraryHandle = nullptr;
            }
        }

        m_plugins.clear();

        // Shutdown and release plugin-registered panels
        for (auto& panel : m_registeredPanels)
        {
            panel->Shutdown();
        }
        m_registeredPanels.clear();
    }

    void EditorPluginManager::UpdateAll(float deltaTime)
    {
        for (auto& entry : m_plugins)
        {
            if (entry.isInitialized)
            {
                SPARK_GUARDED_UPDATE("EditorPlugin:Update", "Editor", { entry.plugin->Update(deltaTime); });
            }
        }
    }

    void EditorPluginManager::RenderAll()
    {
        for (auto& entry : m_plugins)
        {
            if (entry.isInitialized)
            {
                SPARK_GUARDED_UPDATE("EditorPlugin:Render", "Editor", { entry.plugin->OnGUI(); });
            }
        }
    }

    // ----- Event hooks -----

    void EditorPluginManager::NotifySceneLoad(const std::string& scenePath)
    {
        for (auto& entry : m_plugins)
        {
            if (entry.isInitialized)
            {
                SPARK_GUARDED_UPDATE("EditorPlugin:SceneLoad", "Editor", { entry.plugin->OnSceneLoad(scenePath); });
            }
        }
    }

    void EditorPluginManager::NotifySceneSave(const std::string& scenePath)
    {
        for (auto& entry : m_plugins)
        {
            if (entry.isInitialized)
            {
                SPARK_GUARDED_UPDATE("EditorPlugin:SceneSave", "Editor", { entry.plugin->OnSceneSave(scenePath); });
            }
        }
    }

    void EditorPluginManager::NotifyEntitySelected(uint32_t entityID)
    {
        for (auto& entry : m_plugins)
        {
            if (entry.isInitialized)
            {
                SPARK_GUARDED_UPDATE("EditorPlugin:EntitySelect", "Editor",
                                     { entry.plugin->OnEntitySelected(entityID); });
            }
        }
    }

    void EditorPluginManager::RenderMenuBarItems()
    {
        for (auto& entry : m_plugins)
        {
            if (entry.isInitialized)
            {
                SPARK_GUARDED_UPDATE("EditorPlugin:MenuBar", "Editor", { entry.plugin->OnMenuBar(); });
            }
        }
    }

    // ----- Panel registration -----

    void EditorPluginManager::RegisterPanel(std::unique_ptr<EditorPanel> panel)
    {
        if (!panel)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "EditorPluginManager: Cannot register null panel");
            return;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "EditorPluginManager: Registered panel '%s'",
                       panel->GetName().c_str());
        m_registeredPanels.push_back(std::move(panel));
    }

    const std::vector<std::unique_ptr<EditorPanel>>& EditorPluginManager::GetRegisteredPanels() const
    {
        return m_registeredPanels;
    }

    // ----- Console integration -----

    void EditorPluginManager::Console_ListPlugins() const
    {
        if (m_plugins.empty())
        {
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "No plugins loaded.");
            return;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Loaded plugins (%zu):", m_plugins.size());
        for (const auto& entry : m_plugins)
        {
            const char* status = entry.isInitialized ? "initialized" : "registered";
            const char* source = entry.isFromDLL ? "DLL" : "built-in";
            SPARK_LOG_INFO(Spark::LogCategory::Editor, "  - %s v%s [%s, %s]", entry.plugin->GetName(),
                           entry.plugin->GetVersion(), source, status);
        }
    }

    // ----- Private helpers -----

    std::vector<PluginEntry>::iterator EditorPluginManager::FindPlugin(const std::string& name)
    {
        for (auto it = m_plugins.begin(); it != m_plugins.end(); ++it)
        {
            if (it->plugin && std::string(it->plugin->GetName()) == name)
            {
                return it;
            }
        }
        return m_plugins.end();
    }

    std::vector<PluginEntry>::const_iterator EditorPluginManager::FindPlugin(const std::string& name) const
    {
        for (auto it = m_plugins.cbegin(); it != m_plugins.cend(); ++it)
        {
            if (it->plugin && std::string(it->plugin->GetName()) == name)
            {
                return it;
            }
        }
        return m_plugins.cend();
    }

    void EditorPluginManager::UnloadLibrary(void* handle)
    {
        if (!handle)
        {
            return;
        }

#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(handle));
#else
        dlclose(handle);
#endif
    }

} // namespace SparkEditor
