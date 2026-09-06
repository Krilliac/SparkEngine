/**
 * @file EditorWorkflow.h
 * @brief Lightweight editor workflow automation system
 *
 * Provides a sequential step-based workflow executor that the editor can
 * use to chain common operations (build, cook, validate, export, etc.).
 * Workflows are registered in a global registry and executed with a
 * shared context for inter-step communication.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace SparkEditor
{

    class EditorUI; // forward

    // ========================================================================
    // Context — shared mutable state passed between workflow steps
    // ========================================================================

    struct WorkflowContext
    {
        std::unordered_map<std::string, std::string> variables; ///< Key-value store for inter-step data.
        std::vector<std::string> log;                           ///< Accumulated log messages.
        EditorUI* editorUI = nullptr;                           ///< Non-owning access to editor systems.
        bool cancelled = false;                                 ///< Set to true to abort remaining steps.

        /// Convenience: append a log line.
        void Log(const std::string& message) { log.push_back(message); }
    };

    // ========================================================================
    // Step — a single action inside a workflow
    // ========================================================================

    struct WorkflowStep
    {
        std::string name;        ///< Short human-readable name ("Save Scene").
        std::string description; ///< Longer explanation shown on hover.

        /// Execute the step. Returns true on success, false on failure.
        std::function<bool(WorkflowContext&)> execute;

        /// Optional precondition. If provided and returns false, the step is skipped with a warning.
        std::function<bool()> canExecute;
    };

    // ========================================================================
    // Result — returned after a workflow completes
    // ========================================================================

    enum class WorkflowStatus
    {
        Idle,
        Running,
        Completed,
        Failed,
        Cancelled
    };

    struct WorkflowResult
    {
        WorkflowStatus status = WorkflowStatus::Idle;
        uint32_t stepsCompleted = 0;  ///< How many steps ran successfully.
        uint32_t totalSteps = 0;      ///< Total steps in the workflow.
        std::string failedStepName;   ///< Name of the step that failed (if any).
        std::vector<std::string> log; ///< Accumulated log from the context.
    };

    // ========================================================================
    // EditorWorkflow — an ordered list of steps
    // ========================================================================

    class EditorWorkflow
    {
      public:
        EditorWorkflow() = default;
        EditorWorkflow(std::string name, std::string description, std::string category);

        /// @brief Append a step to the workflow.
        void AddStep(WorkflowStep step);

        /// @brief Execute all steps sequentially. Stops on failure or cancellation.
        WorkflowResult Execute(WorkflowContext& ctx) const;

        const std::string& GetName() const { return m_name; }
        const std::string& GetDescription() const { return m_description; }
        const std::string& GetCategory() const { return m_category; }
        uint32_t GetStepCount() const { return static_cast<uint32_t>(m_steps.size()); }
        const std::vector<WorkflowStep>& GetSteps() const { return m_steps; }

        /// @brief True when the workflow performs destructive work (deleting directories).
        /// The Workflow panel requires an explicit confirmation before running these.
        bool RequiresConfirmation() const { return m_requiresConfirmation; }
        void SetRequiresConfirmation(bool requiresConfirmation) { m_requiresConfirmation = requiresConfirmation; }

      private:
        std::string m_name;
        std::string m_description;
        std::string m_category; ///< "Build", "Scene", "Asset", "Custom"
        std::vector<WorkflowStep> m_steps;
        bool m_requiresConfirmation = false;
    };

    // ========================================================================
    // WorkflowRegistry — singleton holding all registered workflows
    // ========================================================================

    class WorkflowRegistry
    {
      public:
        static WorkflowRegistry& Instance();

        void Register(EditorWorkflow workflow);

        const EditorWorkflow* Find(const std::string& name) const;

        /// @brief Get all workflows in a given category.
        std::vector<const EditorWorkflow*> GetByCategory(const std::string& category) const;

        /// @brief Get sorted list of unique category names.
        std::vector<std::string> GetCategories() const;

        /// @brief Get all registered workflows.
        const std::vector<EditorWorkflow>& GetAll() const { return m_workflows; }

      private:
        WorkflowRegistry() = default;
        std::vector<EditorWorkflow> m_workflows;
    };

} // namespace SparkEditor
