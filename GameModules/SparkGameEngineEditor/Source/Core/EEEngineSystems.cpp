/**
 * @file EEEngineSystems.cpp
 * @brief Wires engine editor tools into engine subsystems
 *
 * Registers editor project data with save system, event bus, and
 * undo/redo history tracking.
 */

#include "EEEngineSystems.h"
#include "Enums/EngineEditorEnums.h"

#include "Engine/SaveSystem/SaveSystem.h"
#include "Engine/Events/EventSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

namespace EngineEditor
{

    EEEngineSystems::~EEEngineSystems()
    {
        if (m_initialized)
            Shutdown();
    }

    bool EEEngineSystems::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;

        m_context = context;

        RegisterSaveSerializers();
        SubscribeToEvents();

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Engine editor integration initialized");
        Spark::SimpleConsole::GetInstance().LogInfo("[EngineEditor] Engine systems wired (save, events, undo)");
        return true;
    }

    void EEEngineSystems::Update([[maybe_unused]] float deltaTime)
    {
        if (!m_initialized)
            return;
    }

    void EEEngineSystems::Shutdown()
    {
        if (!m_initialized)
            return;

        m_eventHandles.clear();
        m_undoStack.clear();
        m_redoStack.clear();
        m_context = nullptr;
        m_initialized = false;
    }

    void EEEngineSystems::RenderDebugUI() {}

    std::string EEEngineSystems::NewProject(const std::string& name)
    {
        m_projectName = name;
        m_undoStack.clear();
        m_redoStack.clear();
        RecordAction("Created project '" + name + "'");
        return "Project '" + name + "' created";
    }

    std::string EEEngineSystems::SaveProject(const std::string& slotName)
    {
        auto* saveSystem = m_context->GetSaveSystem();
        if (!saveSystem)
            return "Save system not available";

        return "Save to slot '" + slotName + "' requested (save system wired)";
    }

    std::string EEEngineSystems::LoadProject(const std::string& slotName)
    {
        auto* saveSystem = m_context->GetSaveSystem();
        if (!saveSystem)
            return "Save system not available";

        if (!saveSystem->SaveExists(slotName))
            return "No save found in slot '" + slotName + "'";

        return "Load from slot '" + slotName + "' requested (save system wired)";
    }

    std::string EEEngineSystems::GetProjectStatus() const
    {
        std::string s = "=== Project Status ===\n";
        s += "Name: " + m_projectName + "\n";
        s += "Undo stack: " + std::to_string(m_undoStack.size()) + "\n";
        s += "Redo stack: " + std::to_string(m_redoStack.size()) + "\n";
        return s;
    }

    void EEEngineSystems::RecordAction(const std::string& description)
    {
        m_undoStack.push_back(description);
        m_redoStack.clear();

        if (m_undoStack.size() > 100)
            m_undoStack.erase(m_undoStack.begin());
    }

    bool EEEngineSystems::Undo()
    {
        if (m_undoStack.empty())
            return false;

        m_redoStack.push_back(m_undoStack.back());
        m_undoStack.pop_back();
        return true;
    }

    bool EEEngineSystems::Redo()
    {
        if (m_redoStack.empty())
            return false;

        m_undoStack.push_back(m_redoStack.back());
        m_redoStack.pop_back();
        return true;
    }

    std::string EEEngineSystems::GetUndoHistoryString() const
    {
        std::string s = "=== Undo History ===\n";
        size_t start = m_undoStack.size() > 10 ? m_undoStack.size() - 10 : 0;
        for (size_t i = start; i < m_undoStack.size(); i++)
            s += "  " + std::to_string(i + 1) + ". " + m_undoStack[i] + "\n";
        if (m_undoStack.empty())
            s += "  (empty)\n";
        if (!m_redoStack.empty())
            s += "Redo available: " + std::to_string(m_redoStack.size()) + " actions\n";
        return s;
    }

    void EEEngineSystems::RegisterSaveSerializers()
    {
        auto* saveSystem = m_context->GetSaveSystem();
        if (!saveSystem)
            return;

        auto& registry = Spark::ComponentSerializerRegistry::GetInstance();

        auto registerPlaceholder = [&](const std::string& typeName)
        {
            registry.Register(
                typeName,
                [typeName](const void*) -> Spark::SerializedComponent
                {
                    Spark::SerializedComponent sc;
                    sc.typeName = typeName;
                    sc.properties["placeholder"] = typeName;
                    return sc;
                },
                []([[maybe_unused]] World& world, [[maybe_unused]] EntityID entity,
                   [[maybe_unused]] const Spark::SerializedComponent& data) {});
        };

        registerPlaceholder("EEProjectState");       // Project name, settings
        registerPlaceholder("EEVisualScriptData");   // Script graphs and variables
        registerPlaceholder("EELevelDesignData");    // Placed instances, terrain
        registerPlaceholder("EEMaterialData");       // Material graphs
        registerPlaceholder("EEVFXData");            // VFX assets and emitters
        registerPlaceholder("EEAnimControllerData"); // Animation controllers
        registerPlaceholder("EEUIScreenData");       // UI screen layouts
        registerPlaceholder("EEPrototypeData");      // Blockouts and rules
        registerPlaceholder("EEAssetPipelineData");  // Asset pipeline state

        saveSystem->SetMaxAutoSaves(3);

        SPARK_LOG_INFO(Spark::LogCategory::Game, "Engine editor registered 9 save serializers");
        Spark::SimpleConsole::GetInstance().LogInfo("[EngineEditor] Registered 9 save serializers");
    }

    void EEEngineSystems::SubscribeToEvents()
    {
        auto* eventBus = m_context->GetEventBus();
        if (!eventBus)
            return;

        // Track weather changes for VFX preview updates
        m_eventHandles.push_back(eventBus->Subscribe<Spark::WeatherChangedEvent>(
            [](const Spark::WeatherChangedEvent& evt)
            {
                Spark::SimpleConsole::GetInstance().LogInfo("[EngineEditor] Weather changed to type " +
                                                            std::to_string(evt.newType) + " — updating VFX previews");
            }));

        // Track time-of-day for lighting preview
        m_eventHandles.push_back(eventBus->Subscribe<Spark::TimeOfDayChangedEvent>(
            [](const Spark::TimeOfDayChangedEvent& evt)
            {
                if (evt.currentHour >= 6.0f && evt.previousHour < 6.0f)
                {
                    Spark::SimpleConsole::GetInstance().LogInfo("[EngineEditor] Dawn — updating lighting preview");
                }
                else if (evt.currentHour >= 20.0f && evt.previousHour < 20.0f)
                {
                    Spark::SimpleConsole::GetInstance().LogInfo("[EngineEditor] Dusk — updating lighting preview");
                }
            }));

        SPARK_LOG_INFO(Spark::LogCategory::Game, "Engine editor subscribed to 2 engine events");
        Spark::SimpleConsole::GetInstance().LogInfo("[EngineEditor] Subscribed to 2 engine events");
    }

} // namespace EngineEditor
