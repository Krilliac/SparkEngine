/**
 * @file EEEngineSystems.h
 * @brief Wires engine editor tools into engine subsystems
 * @author Spark Engine Team
 * @date 2026
 *
 * EEEngineSystems registers editor tool data with engine infrastructure:
 * save serializers for editor projects, event bus subscriptions for editor
 * state changes, and undo/redo history tracking.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Utils/EventBus.h"

#include <string>
#include <vector>

namespace EngineEditor
{

    /**
     * @brief Bridges engine editor tools with engine subsystems
     *
     * Constructed and owned by SparkGameEngineEditorModule. On Initialize() it
     * registers editor project data with save and event systems.
     */
    class EEEngineSystems
    {
      public:
        EEEngineSystems() = default;
        ~EEEngineSystems();

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();
        void RenderDebugUI();

        // Project management
        std::string NewProject(const std::string& name);
        std::string SaveProject(const std::string& slotName);
        std::string LoadProject(const std::string& slotName);
        std::string GetProjectStatus() const;

        // Undo/Redo
        void RecordAction(const std::string& description);
        bool Undo();
        bool Redo();
        size_t GetUndoStackSize() const { return m_undoStack.size(); }
        size_t GetRedoStackSize() const { return m_redoStack.size(); }
        std::string GetUndoHistoryString() const;

      private:
        void RegisterSaveSerializers();
        void SubscribeToEvents();

        Spark::IEngineContext* m_context = nullptr;
        bool m_initialized = false;
        std::string m_projectName = "Untitled";
        std::vector<std::string> m_undoStack;
        std::vector<std::string> m_redoStack;
        std::vector<Spark::SubscriptionHandle> m_eventHandles;
    };

} // namespace EngineEditor
