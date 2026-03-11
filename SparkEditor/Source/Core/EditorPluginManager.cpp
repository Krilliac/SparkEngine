/**
 * @file EditorPluginManager.cpp
 * @brief Implementation of the editor plugin manager (R7.1)
 * @author Spark Engine Team
 * @date 2025
 */

#include "EditorPluginManager.h"
#include "EditorPanel.h"
#include "../Utils/SparkConsole.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifdef SPARK_PLATFORM_WINDOWS
#include <Windows.h>
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
        entry.destroyFn = destroyFn;
        entry.isInitialized = false;

        Spark::SimpleConsole::GetInstance().LogInfo("EditorPluginManager: Registered plugin '" + name + "' v" +
                                                    entry.plugin->GetVersion());
        m_plugins.push_back(std::move(entry));
        return true;
    }

    bool EditorPluginManager::LoadPlugin(const std::string& path)
    {
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

        // Wrap in unique_ptr with custom deleter awareness — the destroy function
        // is stored in PluginEntry and called during unload rather than via unique_ptr deleter.
        auto plugin = std::unique_ptr<IEditorPlugin>(rawPlugin);
        return RegisterPluginInstance(std::move(plugin), /*isFromDLL=*/true, handle, destroyFn);
    }

    bool EditorPluginManager::UnloadPlugin(const std::string& name)
    {
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

            if (entry.plugin->Initialize(app))
            {
                entry.isInitialized = true;
                Spark::SimpleConsole::GetInstance().LogSuccess("EditorPluginManager: Plugin '" + name +
                                                               "' initialized successfully");
            }
            else
            {
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
                entry.plugin->Update(deltaTime);
            }
        }
    }

    void EditorPluginManager::RenderAll()
    {
        for (auto& entry : m_plugins)
        {
            if (entry.isInitialized)
            {
                entry.plugin->OnGUI();
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
                entry.plugin->OnSceneLoad(scenePath);
            }
        }
    }

    void EditorPluginManager::NotifySceneSave(const std::string& scenePath)
    {
        for (auto& entry : m_plugins)
        {
            if (entry.isInitialized)
            {
                entry.plugin->OnSceneSave(scenePath);
            }
        }
    }

    void EditorPluginManager::NotifyEntitySelected(uint32_t entityID)
    {
        for (auto& entry : m_plugins)
        {
            if (entry.isInitialized)
            {
                entry.plugin->OnEntitySelected(entityID);
            }
        }
    }

    void EditorPluginManager::RenderMenuBarItems()
    {
        for (auto& entry : m_plugins)
        {
            if (entry.isInitialized)
            {
                entry.plugin->OnMenuBar();
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
    }

    const std::vector<std::unique_ptr<EditorPanel>>& EditorPluginManager::GetRegisteredPanels() const
    {
        return m_registeredPanels;
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
