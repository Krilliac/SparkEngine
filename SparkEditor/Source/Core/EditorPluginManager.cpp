/// @file EditorPluginManager.cpp
/// @brief Implementation of the editor plugin manager.

#include "EditorPluginManager.h"
#include "EditorPluginDiscovery.h"
#include "EditorPanel.h"
#include "Core/DynamicPluginHost.h"
#include "Core/FaultIsolation.h"
#include "Utils/SparkConsole.h"
#include "Utils/Validate.h"

#include <algorithm>
#include <chrono>
#include <cstddef>

namespace SparkEditor
{

    namespace
    {
        void LogDynamicPluginMessage(SparkPluginLogLevel level, const char* category, const char* message)
        {
            const std::string text =
                "[" + std::string(category ? category : "Plugin") + "] " + std::string(message ? message : "");
            auto& console = Spark::SimpleConsole::GetInstance();
            if (level >= SPARK_PLUGIN_LOG_ERROR)
                console.LogError(text);
            else if (level >= SPARK_PLUGIN_LOG_WARNING)
                console.LogWarning(text);
            else
                console.LogInfo(text);
        }
    } // namespace

    EditorPluginManager::~EditorPluginManager()
    {
        ShutdownAll();
    }

    // ----- Registration -----
    bool EditorPluginManager::RegisterPluginInstance(EditorPluginPtr plugin)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        if (!plugin)
        {
            Spark::SimpleConsole::GetInstance().LogError("EditorPluginManager: Cannot register null plugin");
            return false;
        }

        const std::string name = plugin->GetName();
        if (FindPlugin(name) != m_plugins.end())
        {
            Spark::SimpleConsole::GetInstance().LogWarning("EditorPluginManager: Plugin '" + name +
                                                           "' is already registered");
            return false;
        }

        PluginEntry entry;
        entry.plugin = std::move(plugin);
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

        if (path.contains(".."))
        {
            Spark::SimpleConsole::GetInstance().LogError("Plugin path rejected — contains '..' traversal: " + path);
            return false;
        }

        Spark::SimpleConsole::GetInstance().LogInfo("EditorPluginManager: Loading plugin from '" + path + "'...");

        auto host = std::make_unique<Spark::DynamicPluginHost>();
        host->SetLogSink(&LogDynamicPluginMessage);

        std::string error;
        if (!host->Load(path, &error))
        {
            Spark::SimpleConsole::GetInstance().LogError("EditorPluginManager: Failed to load '" + path +
                                                         "': " + error);
            return false;
        }

        const SparkPluginDescriptor* descriptor = host->Descriptor();
        if (!descriptor || !descriptor->api || (descriptor->api->capabilities & SPARK_PLUGIN_CAP_EDITOR_EXTENSION) == 0)
        {
            Spark::SimpleConsole::GetInstance().LogError("EditorPluginManager: Plugin '" + path +
                                                         "' does not advertise editor-extension capability");
            host->Unload();
            return false;
        }

        const std::string id = descriptor->id;
        const std::string name = descriptor->name;
        if (FindPlugin(name) != m_plugins.end() || FindDynamicPlugin(name) != m_dynamicPlugins.end() ||
            FindDynamicPlugin(id) != m_dynamicPlugins.end())
        {
            Spark::SimpleConsole::GetInstance().LogWarning("EditorPluginManager: Plugin '" + name +
                                                           "' is already registered");
            host->Unload();
            return false;
        }

        if (m_lifecycleInitialized && !host->Start(&error))
        {
            Spark::SimpleConsole::GetInstance().LogError("EditorPluginManager: Failed to start plugin '" + name +
                                                         "': " + error);
            host->Unload();
            return false;
        }

        DynamicPluginEntry entry;
        entry.host = std::move(host);
        entry.id = id;
        entry.name = name;
        entry.version = descriptor->version ? descriptor->version : "";
        m_dynamicPlugins.push_back(std::move(entry));
        Spark::SimpleConsole::GetInstance().LogInfo("EditorPluginManager: Registered stable-ABI plugin '" + name +
                                                    "' v" + m_dynamicPlugins.back().version);
        return true;
    }

    bool EditorPluginManager::LoadPluginsFromProjectDirectory(const std::string& projectPath,
                                                              const std::string& directory, size_t* loadedCount)
    {
        if (loadedCount)
            *loadedCount = 0;

        std::vector<std::filesystem::path> candidates;
        std::string error;
        if (!PluginDiscovery::Discover(projectPath, directory, candidates, error))
        {
            Spark::SimpleConsole::GetInstance().LogError("EditorPluginManager: " + error);
            return false;
        }

        std::vector<std::string> loadedNames;
        loadedNames.reserve(candidates.size());
        for (const std::filesystem::path& candidate : candidates)
        {
            if (!LoadPlugin(LaunchContext::PathToUtf8(candidate)))
            {
                size_t retainedCount = 0;
                for (auto it = loadedNames.rbegin(); it != loadedNames.rend(); ++it)
                {
                    if (!UnloadPlugin(*it))
                        ++retainedCount;
                }
                if (loadedCount)
                    *loadedCount = retainedCount;
                if (retainedCount != 0)
                    Spark::SimpleConsole::GetInstance().LogError(
                        "EditorPluginManager: directory load rollback retained " + std::to_string(retainedCount) +
                        " plugin(s) in fail-closed ownership");
                return false;
            }

            loadedNames.push_back(m_dynamicPlugins.back().name);
        }

        if (loadedCount)
            *loadedCount = loadedNames.size();
        return true;
    }

    bool EditorPluginManager::UnloadPlugin(const std::string& name)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !name.empty(), false);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Unloading plugin '%s'", name.c_str());

        auto dynamic = FindDynamicPlugin(name);
        if (dynamic != m_dynamicPlugins.end())
        {
            std::string error;
            if (!dynamic->host->Unload(std::chrono::seconds(5), &error))
            {
                Spark::SimpleConsole::GetInstance().LogError("EditorPluginManager: Failed to unload plugin '" +
                                                             dynamic->name + "': " + error);
                return false;
            }
            const std::string unloadedName = dynamic->name;
            m_dynamicPlugins.erase(dynamic);
            Spark::SimpleConsole::GetInstance().LogInfo("EditorPluginManager: Unloaded plugin '" + unloadedName + "'");
            return true;
        }

        auto it = FindPlugin(name);
        if (it == m_plugins.end())
        {
            Spark::SimpleConsole::GetInstance().LogWarning("EditorPluginManager: Plugin '" + name + "' not found");
            return false;
        }

        if (it->isInitialized)
        {
            Spark::SimpleConsole::GetInstance().LogInfo("EditorPluginManager: Shutting down plugin '" + name + "'...");
            it->plugin->Shutdown();
            it->isInitialized = false;
        }

        ReleasePanelsForPlugin(name);

        it->plugin.reset();
        m_plugins.erase(it);

        Spark::SimpleConsole::GetInstance().LogInfo("EditorPluginManager: Unloaded plugin '" + name + "'");
        return true;
    }

    // ----- Lookup -----

    IEditorPlugin* EditorPluginManager::GetPlugin(const std::string& name) const
    {
        auto it = FindPlugin(name);
        return it != m_plugins.end() ? it->plugin.get() : nullptr;
    }

    const SparkPluginDescriptor* EditorPluginManager::GetDynamicPluginDescriptor(const std::string& nameOrId) const
    {
        const auto it = FindDynamicPlugin(nameOrId);
        return it == m_dynamicPlugins.end() ? nullptr : it->host->Descriptor();
    }

    size_t EditorPluginManager::GetPluginCount() const
    {
        return m_plugins.size() + m_dynamicPlugins.size();
    }

    std::vector<std::string> EditorPluginManager::GetPluginNames() const
    {
        std::vector<std::string> names;
        names.reserve(GetPluginCount());
        for (const auto& entry : m_plugins)
            names.emplace_back(entry.plugin->GetName());
        for (const auto& entry : m_dynamicPlugins)
            names.push_back(entry.name);
        return names;
    }

    // ----- Lifecycle -----

    bool EditorPluginManager::InitializeAll(EditorApplication* app)
    {
        bool allSucceeded = true;

        for (auto& entry : m_plugins)
        {
            if (entry.isInitialized)
                continue;

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

        for (auto& entry : m_dynamicPlugins)
        {
            if (entry.host->GetState() == Spark::DynamicPluginHost::State::Started)
                continue;

            std::string error;
            if (!entry.host->Start(&error))
            {
                Spark::SimpleConsole::GetInstance().LogError("EditorPluginManager: Plugin '" + entry.name +
                                                             "' failed to start: " + error);
                allSucceeded = false;
            }
        }

        m_lifecycleInitialized = true;

        return allSucceeded;
    }

    void EditorPluginManager::ShutdownAll()
    {
        for (auto it = m_dynamicPlugins.rbegin(); it != m_dynamicPlugins.rend(); ++it)
        {
            std::string error;
            if (!it->host->Unload(std::chrono::seconds(5), &error))
                Spark::SimpleConsole::GetInstance().LogError("EditorPluginManager: Failed to unload plugin '" +
                                                             it->name + "': " + error);
        }
        m_dynamicPlugins.clear();

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

        for (auto it = m_registeredPanels.rbegin(); it != m_registeredPanels.rend(); ++it)
            (*it)->Shutdown();
        m_registeredPanels.clear();
        m_registeredPanelOwners.clear();

        for (auto it = m_plugins.rbegin(); it != m_plugins.rend(); ++it)
            it->plugin.reset();

        m_plugins.clear();
        m_lifecycleInitialized = false;
    }

    void EditorPluginManager::UpdateAll(float deltaTime)
    {
        for (auto& entry : m_plugins)
        {
            if (entry.isInitialized)
                SPARK_GUARDED_UPDATE("EditorPlugin:Update", "Editor", { entry.plugin->Update(deltaTime); });
        }

        for (auto& entry : m_dynamicPlugins)
        {
            const SparkPluginDescriptor* descriptor = entry.host->Descriptor();
            if (!descriptor || !descriptor->api || (descriptor->api->capabilities & SPARK_PLUGIN_CAP_TICK) == 0)
                continue;

            const SparkPluginResult result = entry.host->Tick(deltaTime);
            if (result != SPARK_PLUGIN_OK)
                Spark::SimpleConsole::GetInstance().LogError("EditorPluginManager: Plugin '" + entry.name +
                                                             "' tick failed with result " + std::to_string(result));
        }
    }

    void EditorPluginManager::RenderAll()
    {
        for (auto& entry : m_plugins)
        {
            if (entry.isInitialized)
                SPARK_GUARDED_UPDATE("EditorPlugin:Render", "Editor", { entry.plugin->OnGUI(); });
        }
    }

    // ----- Event hooks -----

    void EditorPluginManager::NotifySceneLoad(const std::string& scenePath)
    {
        for (auto& entry : m_plugins)
        {
            if (entry.isInitialized)
                SPARK_GUARDED_UPDATE("EditorPlugin:SceneLoad", "Editor", { entry.plugin->OnSceneLoad(scenePath); });
        }
    }

    void EditorPluginManager::NotifySceneSave(const std::string& scenePath)
    {
        for (auto& entry : m_plugins)
        {
            if (entry.isInitialized)
                SPARK_GUARDED_UPDATE("EditorPlugin:SceneSave", "Editor", { entry.plugin->OnSceneSave(scenePath); });
        }
    }

    void EditorPluginManager::NotifyEntitySelected(uint32_t entityID)
    {
        for (auto& entry : m_plugins)
        {
            if (entry.isInitialized)
                SPARK_GUARDED_UPDATE("EditorPlugin:EntitySelect", "Editor",
                                     { entry.plugin->OnEntitySelected(entityID); });
        }
    }

    void EditorPluginManager::RenderMenuBarItems()
    {
        for (auto& entry : m_plugins)
        {
            if (entry.isInitialized)
                SPARK_GUARDED_UPDATE("EditorPlugin:MenuBar", "Editor", { entry.plugin->OnMenuBar(); });
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

        if (m_plugins.empty() && m_dynamicPlugins.empty())
        {
            console.LogInfo("No plugins loaded.");
            return;
        }

        console.LogInfo("Loaded plugins (" + std::to_string(GetPluginCount()) + "):");
        for (const auto& entry : m_plugins)
        {
            const std::string status = entry.isInitialized ? "initialized" : "registered";
            console.LogInfo("  - " + std::string(entry.plugin->GetName()) + " v" +
                            std::string(entry.plugin->GetVersion()) + " [built-in, " + status + "]");
        }
        for (const auto& entry : m_dynamicPlugins)
        {
            const std::string status =
                entry.host->GetState() == Spark::DynamicPluginHost::State::Started ? "initialized" : "registered";
            console.LogInfo("  - " + entry.name + " v" + entry.version + " [stable C ABI, " + status + "]");
        }
    }

    // ----- Private helpers -----

    std::vector<PluginEntry>::iterator EditorPluginManager::FindPlugin(const std::string& name)
    {
        return std::find_if(m_plugins.begin(), m_plugins.end(), [&name](const PluginEntry& e)
                            { return e.plugin && std::string(e.plugin->GetName()) == name; });
    }

    std::vector<PluginEntry>::const_iterator EditorPluginManager::FindPlugin(const std::string& name) const
    {
        return std::find_if(m_plugins.cbegin(), m_plugins.cend(), [&name](const PluginEntry& e)
                            { return e.plugin && std::string(e.plugin->GetName()) == name; });
    }

    std::vector<DynamicPluginEntry>::iterator EditorPluginManager::FindDynamicPlugin(const std::string& nameOrId)
    {
        return std::find_if(m_dynamicPlugins.begin(), m_dynamicPlugins.end(),
                            [&nameOrId](const DynamicPluginEntry& entry)
                            { return entry.name == nameOrId || entry.id == nameOrId; });
    }

    std::vector<DynamicPluginEntry>::const_iterator EditorPluginManager::FindDynamicPlugin(
        const std::string& nameOrId) const
    {
        return std::find_if(m_dynamicPlugins.cbegin(), m_dynamicPlugins.cend(),
                            [&nameOrId](const DynamicPluginEntry& entry)
                            { return entry.name == nameOrId || entry.id == nameOrId; });
    }

} // namespace SparkEditor
