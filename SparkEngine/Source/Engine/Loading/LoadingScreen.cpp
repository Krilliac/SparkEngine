/**
 * @file LoadingScreen.cpp
 * @brief Implementation of the loading screen and progress tracking system
 */

#include "LoadingScreen.h"
#include "../../Utils/Validate.h"

#include <sstream>
#include <random>

namespace Spark
{

    LoadingScreen::LoadingScreen() = default;

    void LoadingScreen::AddLoadingTip(const std::string& tip)
    {
        m_loadingTips.push_back(tip);
    }

    void LoadingScreen::BeginLoading(const std::string& name)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Core);
        SPARK_LOG_INFO(Spark::LogCategory::Core, "Beginning loading session: '%s'", name.c_str());
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sessionName = name;
        m_tasks.clear();
        m_progress.store(0.0f);
        m_state = LoadingState::Idle;
        m_currentTaskIndex = 0;
        m_elapsedTime = 0.0f;
        m_cancelled = false;
    }

    void LoadingScreen::AddTask(const std::string& name, float weight, std::function<bool()> execute)
    {
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Core, name);
        SPARK_WARN_IF(Spark::LogCategory::Core, weight <= 0.0f, "Loading task added with non-positive weight");
        std::lock_guard<std::mutex> lock(m_mutex);
        LoadingTask task;
        task.name = name;
        task.weight = weight;
        task.execute = std::move(execute);
        m_tasks.push_back(std::move(task));
    }

    void LoadingScreen::Execute()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Core);

        // Publish the state transition and read the task list under the lock so
        // it stays consistent with the concurrent readers (GetCurrentTaskName /
        // Console_GetStatus) and with a Cancel() on another thread. The task
        // callbacks themselves run OUTSIDE the lock — they may be slow or
        // re-enter the loading screen.
        float totalWeight = 0.0f;
        size_t taskCount = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            SPARK_WARN_IF(Spark::LogCategory::Core, m_tasks.empty(), "Execute called with no loading tasks");
            m_state = LoadingState::Loading;
            taskCount = m_tasks.size();
            for (const auto& task : m_tasks)
            {
                totalWeight += task.weight;
            }
        }
        if (totalWeight <= 0.0f)
        {
            totalWeight = 1.0f;
        }

        float completedWeight = 0.0f;

        for (size_t i = 0; i < taskCount; ++i)
        {
            std::function<bool()> exec;
            std::string taskName;
            float taskWeight = 0.0f;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                // Poll cancellation under the lock so a concurrent Cancel() is
                // observed; a plain-bool read could be cached/hoisted otherwise.
                if (m_cancelled)
                {
                    m_state = LoadingState::Cancelled;
                    return;
                }
                m_currentTaskIndex = i;
                exec = m_tasks[i].execute;
                taskName = m_tasks[i].name;
                taskWeight = m_tasks[i].weight;
            }

            // Execute the task without holding the lock.
            bool success = exec ? exec() : true;

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_tasks[i].success = success;
                m_tasks[i].completed = true;
            }

            if (!success)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Core, "LoadingScreen task '%s' failed", taskName.c_str());
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_state = LoadingState::Failed;
                }
                m_completeCallbacks.Broadcast(false);
                return;
            }

            SPARK_LOG_DEBUG(Spark::LogCategory::Core, "LoadingScreen task '%s' completed", taskName.c_str());

            completedWeight += taskWeight;
            float progress = completedWeight / totalWeight;
            m_progress.store(progress);

            // Fire progress callbacks
            m_progressCallbacks.Broadcast(progress, taskName);
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_state = LoadingState::Completed;
        }
        m_progress.store(1.0f);

        m_completeCallbacks.Broadcast(true);
    }

    void LoadingScreen::Cancel()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Core, "LoadingScreen::Cancel requested for session '%s'",
                       m_sessionName.c_str());
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cancelled = true;
    }

    std::string LoadingScreen::GetCurrentTaskName() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_currentTaskIndex < m_tasks.size())
        {
            return m_tasks[m_currentTaskIndex].name;
        }
        return "";
    }

    std::string LoadingScreen::GetCurrentTip() const
    {
        if (m_loadingTips.empty())
        {
            return "";
        }
        static std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<size_t> dist(0, m_loadingTips.size() - 1);
        return m_loadingTips[dist(rng)];
    }

    Spark::Delegate<float, const std::string&>::HandlerID LoadingScreen::OnProgress(
        std::function<void(float, const std::string&)> callback)
    {
        return (m_progressCallbacks += std::move(callback));
    }

    Spark::Delegate<bool>::HandlerID LoadingScreen::OnComplete(std::function<void(bool)> callback)
    {
        return (m_completeCallbacks += std::move(callback));
    }

    std::string LoadingScreen::Console_GetStatus() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::ostringstream oss;
        oss << "=== Loading Screen ===\n";
        oss << "Session: " << m_sessionName << "\n";
        const char* stateNames[] = {"Idle", "Loading", "Completed", "Failed", "Cancelled"};
        auto stateIdx = static_cast<size_t>(m_state);
        oss << "State: " << (stateIdx < std::size(stateNames) ? stateNames[stateIdx] : "Unknown") << "\n";
        oss << "Progress: " << static_cast<int>(m_progress.load() * 100) << "%\n";
        oss << "Tasks: " << m_tasks.size() << "\n";
        for (size_t i = 0; i < m_tasks.size(); ++i)
        {
            oss << "  " << (i < m_currentTaskIndex ? "[DONE]" : (i == m_currentTaskIndex ? "[....]" : "[    ]")) << " "
                << m_tasks[i].name << " (weight=" << m_tasks[i].weight << ")\n";
        }
        oss << "Tips loaded: " << m_loadingTips.size() << "\n";
        return oss.str();
    }

} // namespace Spark
