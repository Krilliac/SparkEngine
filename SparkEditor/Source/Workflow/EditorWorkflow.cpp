/**
 * @file EditorWorkflow.cpp
 * @brief Workflow execution and registry implementation
 */

#include "EditorWorkflow.h"

#include <algorithm>
#include <set>

namespace SparkEditor
{

    // ========================================================================
    // EditorWorkflow
    // ========================================================================

    EditorWorkflow::EditorWorkflow(std::string name, std::string description, std::string category)
        : m_name(std::move(name)), m_description(std::move(description)), m_category(std::move(category))
    {
    }

    void EditorWorkflow::AddStep(WorkflowStep step)
    {
        m_steps.push_back(std::move(step));
    }

    WorkflowResult EditorWorkflow::Execute(WorkflowContext& ctx) const
    {
        WorkflowResult result;
        result.totalSteps = static_cast<uint32_t>(m_steps.size());

        ctx.Log("[Workflow] Starting: " + m_name);

        for (uint32_t i = 0; i < result.totalSteps; ++i)
        {
            if (ctx.cancelled)
            {
                result.status = WorkflowStatus::Cancelled;
                ctx.Log("[Workflow] Cancelled at step " + std::to_string(i + 1));
                break;
            }

            const auto& step = m_steps[i];

            // Check precondition
            if (step.canExecute && !step.canExecute())
            {
                ctx.Log("[Skip] " + step.name + " (precondition not met)");
                result.stepsCompleted++;
                continue;
            }

            ctx.Log("[Step " + std::to_string(i + 1) + "/" + std::to_string(result.totalSteps) + "] " + step.name);

            bool ok = step.execute(ctx);
            if (!ok)
            {
                result.status = WorkflowStatus::Failed;
                result.failedStepName = step.name;
                ctx.Log("[FAILED] " + step.name);
                break;
            }

            result.stepsCompleted++;
        }

        if (result.status == WorkflowStatus::Idle)
            result.status = WorkflowStatus::Completed;

        result.log = ctx.log;
        ctx.Log("[Workflow] Finished: " + m_name + " (" + std::to_string(result.stepsCompleted) + "/" +
                std::to_string(result.totalSteps) + " steps)");

        return result;
    }

    // ========================================================================
    // WorkflowRegistry
    // ========================================================================

    WorkflowRegistry& WorkflowRegistry::Instance()
    {
        static WorkflowRegistry s_instance;
        return s_instance;
    }

    void WorkflowRegistry::Register(EditorWorkflow workflow)
    {
        m_workflows.push_back(std::move(workflow));
    }

    const EditorWorkflow* WorkflowRegistry::Find(const std::string& name) const
    {
        for (const auto& wf : m_workflows)
        {
            if (wf.GetName() == name)
                return &wf;
        }
        return nullptr;
    }

    std::vector<const EditorWorkflow*> WorkflowRegistry::GetByCategory(const std::string& category) const
    {
        std::vector<const EditorWorkflow*> result;
        for (const auto& wf : m_workflows)
        {
            if (wf.GetCategory() == category)
                result.push_back(&wf);
        }
        return result;
    }

    std::vector<std::string> WorkflowRegistry::GetCategories() const
    {
        std::set<std::string> cats;
        for (const auto& wf : m_workflows)
            cats.insert(wf.GetCategory());
        return {cats.begin(), cats.end()};
    }

} // namespace SparkEditor
