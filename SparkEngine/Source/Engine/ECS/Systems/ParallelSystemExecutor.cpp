/**
 * @file ParallelSystemExecutor.cpp
 * @brief Stage-based parallel ECS system execution implementation
 *
 * Provides the StageBasedExecutor which groups systems by ECS stage
 * (Physics, Animation, AI, Audio, Lifecycle, Render) and runs systems
 * within each stage concurrently on the JobSystem thread pool.
 * Stages execute sequentially to preserve the required ordering.
 */

#include "ParallelSystemExecutor.h"
#include "../../../Utils/SparkConsole.h"

#include <algorithm>
#include <format>

namespace Spark::ECS
{

    // ============================================================================
    // Registration
    // ============================================================================

    void StageBasedExecutor::RegisterSystem(const std::string& name, SystemStage stage, std::function<void(float)> fn)
    {
        SystemEntry entry;
        entry.name = name;
        entry.stage = stage;
        entry.updateFn = std::move(fn);
        m_systems.push_back(std::move(entry));
    }

    // ============================================================================
    // Execution
    // ============================================================================

    void StageBasedExecutor::ExecuteAll(float dt)
    {
        auto& jobs = Spark::JobSystem::Get();
        const bool jobsAvailable = jobs.IsInitialized() && jobs.GetWorkerCount() > 0;

        // Process each stage in the fixed execution order
        for (uint8_t stageIdx = 0; stageIdx < static_cast<uint8_t>(SystemStage::Count); ++stageIdx)
        {
            auto stage = static_cast<SystemStage>(stageIdx);

            // Gather systems belonging to this stage
            std::vector<SystemEntry*> stageSystems;
            for (auto& sys : m_systems)
            {
                if (sys.stage == stage)
                {
                    stageSystems.push_back(&sys);
                }
            }

            if (stageSystems.empty())
            {
                continue;
            }

            // Single system in stage: run inline, no thread dispatch overhead
            if (stageSystems.size() == 1)
            {
                stageSystems[0]->updateFn(dt);
                continue;
            }

            // Multiple systems: run in parallel if the JobSystem is available
            if (jobsAvailable)
            {
                std::vector<std::future<void>> futures;
                futures.reserve(stageSystems.size());

                for (auto* sys : stageSystems)
                {
                    futures.push_back(jobs.Submit([sys, dt]() { sys->updateFn(dt); }));
                }

                // Wait for all systems in this stage to complete before
                // advancing to the next stage
                for (auto& f : futures)
                {
                    f.get();
                }
            }
            else
            {
                // Fallback: run sequentially when no worker threads are available
                for (auto* sys : stageSystems)
                {
                    sys->updateFn(dt);
                }
            }
        }
    }

    // ============================================================================
    // Lifecycle
    // ============================================================================

    void StageBasedExecutor::Shutdown()
    {
        m_systems.clear();
    }

    // ============================================================================
    // Diagnostics
    // ============================================================================

    static const char* StageToString(SystemStage stage)
    {
        switch (stage)
        {
        case SystemStage::Physics:
            return "Physics";
        case SystemStage::Animation:
            return "Animation";
        case SystemStage::AI:
            return "AI";
        case SystemStage::Audio:
            return "Audio";
        case SystemStage::Lifecycle:
            return "Lifecycle";
        case SystemStage::Render:
            return "Render";
        default:
            return "Unknown";
        }
    }

    std::string StageBasedExecutor::Console_GetStatus() const
    {
        std::string result = "Stage-Based Parallel Executor:\n";

        for (uint8_t stageIdx = 0; stageIdx < static_cast<uint8_t>(SystemStage::Count); ++stageIdx)
        {
            auto stage = static_cast<SystemStage>(stageIdx);
            std::string stageStr = std::format("  [{}]", StageToString(stage));

            bool hasSystems = false;
            for (const auto& sys : m_systems)
            {
                if (sys.stage == stage)
                {
                    if (!hasSystems)
                    {
                        result += stageStr;
                        hasSystems = true;
                    }
                    result += " " + sys.name;
                }
            }

            if (hasSystems)
            {
                result += "\n";
            }
        }

        result += std::format("  Total systems: {}\n", m_systems.size());
        return result;
    }

} // namespace Spark::ECS
