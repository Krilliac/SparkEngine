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
     * Shows running coroutines with their names, elapsed time,
     * and current wait state. Allows stopping individual coroutines.
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
        struct CoroutineInfo
        {
            char name[128] = {};
            float elapsedTime = 0.0f;
            char waitState[64] = {};
            bool running = true;
        };

        void RenderActiveCoroutines();
        void RenderCoroutineStats();

        std::vector<CoroutineInfo> m_coroutines;
        int m_totalStarted = 0;
        int m_totalCompleted = 0;
    };

} // namespace SparkEditor
