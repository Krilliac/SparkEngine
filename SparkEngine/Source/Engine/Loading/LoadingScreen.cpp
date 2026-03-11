/**
 * @file LoadingScreen.cpp
 * @brief Implementation of the loading screen and progress tracking system
 */

#include "LoadingScreen.h"

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
        std::lock_guard<std::mutex> lock(m_mutex);
        LoadingTask task;
        task.name = name;
        task.weight = weight;
        task.execute = std::move(execute);
        m_tasks.push_back(std::move(task));
    }

    void LoadingScreen::Execute()
    {
        m_state = LoadingState::Loading;

        // Calculate total weight
        float totalWeight = 0.0f;
        for (const auto& task : m_tasks)
        {
            totalWeight += task.weight;
        }
        if (totalWeight <= 0.0f)
        {
            totalWeight = 1.0f;
        }

        float completedWeight = 0.0f;

        for (size_t i = 0; i < m_tasks.size(); ++i)
        {
            if (m_cancelled)
            {
                m_state = LoadingState::Cancelled;
                return;
            }

            m_currentTaskIndex = i;
            auto& task = m_tasks[i];

            // Execute the task
            task.success = task.execute ? task.execute() : true;
            task.completed = true;

            if (!task.success)
            {
                m_state = LoadingState::Failed;
                for (const auto& callback : m_completeCallbacks)
                {
                    callback(false);
                }
                return;
            }

            completedWeight += task.weight;
            float progress = completedWeight / totalWeight;
            m_progress.store(progress);

            // Fire progress callbacks
            for (const auto& callback : m_progressCallbacks)
            {
                callback(progress, task.name);
            }
        }

        m_state = LoadingState::Completed;
        m_progress.store(1.0f);

        for (const auto& callback : m_completeCallbacks)
        {
            callback(true);
        }
    }

    void LoadingScreen::Cancel()
    {
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

    void LoadingScreen::OnProgress(std::function<void(float, const std::string&)> callback)
    {
        m_progressCallbacks.push_back(std::move(callback));
    }

    void LoadingScreen::OnComplete(std::function<void(bool)> callback)
    {
        m_completeCallbacks.push_back(std::move(callback));
    }

    std::string LoadingScreen::Console_GetStatus() const
    {
        std::ostringstream oss;
        oss << "=== Loading Screen ===\n";
        oss << "Session: " << m_sessionName << "\n";
        const char* stateNames[] = {"Idle", "Loading", "Completed", "Failed", "Cancelled"};
        oss << "State: " << stateNames[static_cast<int>(m_state)] << "\n";
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
