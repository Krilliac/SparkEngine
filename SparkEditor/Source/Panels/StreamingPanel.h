/**
 * @file StreamingPanel.h
 * @brief Area streaming and LOD management panel
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Panel for configuring seamless area streaming and LOD
     *
     * Shows loaded/unloaded areas with status indicators, streaming distances,
     * memory budget tracking, origin rebase status, and a transition event log.
     */
    class StreamingPanel : public EditorPanel
    {
      public:
        StreamingPanel();
        ~StreamingPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        enum class AreaStatus
        {
            Unloaded,
            Loading,
            Loaded,
            Unloading
        };

        struct AreaInfo
        {
            char name[64] = {};
            float position[3] = {};
            float extents[3] = {100.0f, 100.0f, 100.0f};
            AreaStatus status = AreaStatus::Unloaded;
            bool visible = true;
            int entityCount = 0;
            float memorySizeMB = 0.0f;
        };

        struct StreamEventLog
        {
            float timestamp = 0.0f;
            char areaName[64] = {};
            char action[32] = {};
        };

        void RenderAreaList();
        void RenderStreamingSettings();
        void RenderOriginRebaseInfo();
        void RenderMemoryBudget();
        void RenderEventLog();

        static const char* GetStatusName(AreaStatus status);
        static ImVec4 GetStatusColor(AreaStatus status);

        std::vector<AreaInfo> m_areas;
        std::vector<StreamEventLog> m_streamEvents;
        int m_selectedArea = -1;
        float m_loadDistance = 500.0f;
        float m_unloadDistance = 600.0f;
        float m_originRebaseThreshold = 5000.0f;
        float m_playerPosition[3] = {};
        float m_originOffset[3] = {};
        int m_loadedAreaCount = 0;

        // Memory budget
        float m_memoryBudgetMB = 512.0f;
        float m_memoryUsedMB = 0.0f;
    };

} // namespace SparkEditor
