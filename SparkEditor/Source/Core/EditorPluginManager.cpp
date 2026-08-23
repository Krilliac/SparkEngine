/**
 * @file EditorPluginManager.cpp
 * @brief Implementation of the editor plugin manager (R7.1)
 * @author Spark Engine Team
 * @date 2025
 */

#include "EditorPluginManager.h"
#include "EditorPanel.h"
#include "Core/FaultIsolation.h"
#include "Utils/SparkConsole.h"
#include "Utils/Validate.h"

#include <cstddef>

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

    bool EditorPluginManager::RegisterPluginInstance(EditorPluginPtr plugin, bool isFromDLL, void* libraryHandle)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        if (!plugin)
        {
            Spark::SimpleConsole::GetInstance().LogError("EditorPluginManager: Cannot register null plugin");
            return false;
        }

        const std::string name = plugin->GetName();

        // Check for duplicate names
        if (FindPlugin(name) != m_plugins.end())
        {
            Spark::SimpleConsole::GetInstance().LogWarning("EditorPluginManager: Plugin '" + name +
                                                           "' is already registered");
            return false;
        }

        PluginEntry entry;
        entry.plugin = std::move(plugin);
        entry.isFromDLL = isFromDLL;
        entry.libraryHandle = libraryHandle;
        entry.isInitialized = false;

        Spark::SimpleConsole::GetInstance().LogInfo("EditorPluginManager: Registered plugin '" + name + "' v" +
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
            Spark::SimpleConsole::GetInstance().LogError("Plugin path rejected — contains '..' traversal: " + path);
            return false;
        }

        Spark::SimpleConsole::GetInstance().LogInfo("EditorPluginManager: Loading plugin from '" + path + "'...");

        void* handle = nullptr;
        CreateEditorPluginFn createFn = nullptr;
        DestroyEditorPluginFn destroyFn = nullptr;

#ifdef _WIN32
        handle = LoadLibraryA(path.c_str());
        if (!handle)
        {
            DWORD err = GetLastError();
            Spark::SimpleConsole::GetInstance().LogError("EditorPluginManager: Failed to load '" + path + "' (error " +
                                                         std::to_string(err) + ")");
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
            Spark::SimpleConsole::GetInstance().LogError(std::string("EditorPluginManager: Failed to load '") + path +
                                                         "': " + (err ? err : "unknown error"));
            return false;
        }

        createFn = reinterpret_cast<CreateEditorPluginFn>(dlsym(handle, "CreateEditorPlugin"));
        destroyFn = reinterpret_cast<DestroyEditorPluginFn>(dlsym(handle, "DestroyEditorPlugin"));
#endif

        if (!createFn || !destroyFn)
        {
            Spark::SimpleConsole::GetInstance().LogError(
                "EditorPluginManager: Plugin '" + path +
                "' missing required exports (CreateEditorPlugin/DestroyEditorPlugin)");
            UnloadLibrary(handle);
            return false;
        }

        IEditorPlugin* rawPlugin = createFn();
        if (!rawPlugin)
        {
            Spark::SimpleConsole::GetInstance().LogError(
                "EditorPluginManager: CreateEditorPlugin() returned null for '" + path + "'");
            UnloadLibrary(handle);
            return false;
        }

        EditorPluginPtr plugin(rawPlugin, PluginDeleter{destroyFn});
        try
        {
            if (RegisterPluginInstance(std::move(plugin), /*isFromDLL=*/true, handle))
                return true;
        }
        catch (...)
        {
            UnloadLibrary(handle);
            throw;
        }

        // Registration failure destroys the instance through the DLL export
        // before the library is released (including duplicate-name failures).
        UnloadLibrary(handle);
        return false;
    }

    bool EditorPluginManager::UnloadPlugin(const std::string& name)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !name.empty(), false);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Unloading plugin '%s'", name.c_str());
        auto it = FindPlugin(name);
        if (it == m_plugins.end())
        {
            Spark::SimpleConsole::GetInstance().LogWarning("EditorPluginManager: Plugin '" + name + "' not found");
            return false;
        }

        // Shutdown if initialized
        if (it->isInitialized)
        {
            Spark::SimpleConsole::GetInstance().LogInfo("EditorPluginManager: Shutting down plugin '" + name + "'...");
            it->plugin->Shutdown();
            it->isInitialized = false;
        }

        // Panel vtables and destructors may live in the plugin DLL. Release
        // them before destroying the plugin instance or unloading the library.
        ReleasePanelsForPlugin(name);

        void* handle = it->libraryHandle;
        it->plugin.reset();
        m_plugins.erase(it);

        // Unload the library after erasing the entry
        if (handle)
        {
            UnloadLibrary(handle);
        }

        Spark::SimpleConsole::GetInstance().LogInfo("EditorPluginManager: Unloaded plugin '" + name + "'");
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
            Spark::SimpleConsole::GetInstance().LogInfo("EditorPluginManager: Initializing plugin '" + name + "'...");

            struct RegistrationScope
            {
                std::string& activePlugin;
                ~RegistrationScope() { activePlugin.clear(); }
            } registrationScope{m_registeringPlugin};
            m_registeringPlugin = name;

            if (entry.plugin->Initialize(app))
            {
                entry.isInitialized = true;
                Spark::SimpleConsole::GetInstance().LogSuccess("EditorPluginManager: Plugin '" + name +
                                                               "' initialized successfully");
            }
            else
            {
                ReleasePanelsForPlugin(name);
                Spark::SimpleConsole::GetInstance().LogError("EditorPluginManager: Plugin '" + name +
                                                             "' failed to initialize");
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
                Spark::SimpleConsole::GetInstance().LogInfo("EditorPluginManager: Shutting down plugin '" + name +
                                                            "'...");
                it->plugin->Shutdown();
                it->isInitialized = false;
            }
        }

        // Plugin-provided panel implementations must be gone before any DLL
        // that supplied their virtual methods is unloaded.
        for (auto it = m_registeredPanels.rbegin(); it != m_registeredPanels.rend(); ++it)
            (*it)->Shutdown();
        m_registeredPanels.clear();
        m_registeredPanelOwners.clear();

        // Destroy plugin objects while their DLL exports remain callable.
        for (auto it = m_plugins.rbegin(); it != m_plugins.rend(); ++it)
            it->plugin.reset();

        // Unload all DLL handles only after every DLL-owned object is gone.
        for (auto& entry : m_plugins)
        {
            if (entry.libraryHandle)
            {
                UnloadLibrary(entry.libraryHandle);
                entry.libraryHandle = nullptr;
            }
        }

        m_plugins.clear();
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
            Spark::SimpleConsole::GetInstance().LogWarning("EditorPluginManager: Cannot register null panel");
            return;
        }

        Spark::SimpleConsole::GetInstance().LogInfo("EditorPluginManager: Registered panel '" + panel->GetName() + "'");
        m_registeredPanels.push_back(std::move(panel));
        m_registeredPanelOwners.push_back(m_registeringPlugin);
    }

    const std::vector<std::unique_ptr<EditorPanel>>& EditorPluginManager::GetRegisteredPanels() const
    {
        return m_registeredPanels;
    }

    void EditorPluginManager::ReleasePanelsForPlugin(const std::string& pluginName)
    {
        for (size_t i = m_registeredPanels.size(); i-- > 0;)
        {
            if (m_registeredPanelOwners[i] != pluginName)
                continue;

            m_registeredPanels[i]->Shutdown();
            m_registeredPanels.erase(m_registeredPanels.begin() + static_cast<std::ptrdiff_t>(i));
            m_registeredPanelOwners.erase(m_registeredPanelOwners.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    // ----- Console integration -----

    void EditorPluginManager::Console_ListPlugins() const
    {
        auto& console = Spark::SimpleConsole::GetInstance();

        if (m_plugins.empty())
        {
            console.LogInfo("No plugins loaded.");
            return;
        }

        console.LogInfo("Loaded plugins (" + std::to_string(m_plugins.size()) + "):");
        for (const auto& entry : m_plugins)
        {
            const std::string status = entry.isInitialized ? "initialized" : "registered";
            const std::string source = entry.isFromDLL ? "DLL" : "built-in";
            console.LogInfo("  - " + std::string(entry.plugin->GetName()) + " v" +
                            std::string(entry.plugin->GetVersion()) + " [" + source + ", " + status + "]");
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
