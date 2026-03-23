/**
 * @file CoroutineDebugPanel.h
 * @brief Coroutine scheduler debug and visualization panel
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Panel for monitoring active coroutines and async tasks
     *
     * Shows running coroutines with their names, elapsed time, status,
     * and current wait state. Supports filtering by status and canceling
     * individual or all coroutines.
     */
    class CoroutineDebugPanel : public EditorPanel
    {
      public:
        CoroutineDebugPanel();
        ~CoroutineDebugPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        enum class CoroutineStatus
        {
            Running,
            Suspended,
            Completed,
            Failed
        };

        struct CoroutineInfo
        {
            char name[128] = {};
            float elapsedTime = 0.0f;
            char waitState[64] = {};
            CoroutineStatus status = CoroutineStatus::Running;
        };

        void RenderCoroutineStats();
        void RenderFilterBar();
        void RenderActiveCoroutines();

        static const char* GetStatusName(CoroutineStatus status);
        static ImVec4 GetStatusColor(CoroutineStatus status);

        std::vector<CoroutineInfo> m_coroutines;
        int m_totalStarted = 0;
        int m_totalCompleted = 0;
        int m_totalFailed = 0;
        int m_statusFilter = 0; // 0=All, 1=Running, 2=Suspended
        float m_creationsPerSecond = 0.0f;
    };

} // namespace SparkEditor
