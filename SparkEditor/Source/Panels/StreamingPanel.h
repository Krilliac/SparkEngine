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
     * Shows loaded/unloaded areas, streaming distances, origin rebase status,
     * and provides tools for defining area boundaries and transition zones.
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
        struct AreaInfo
        {
            char name[64] = {};
            float position[3] = {};
            float extents[3] = {100.0f, 100.0f, 100.0f};
            bool loaded = false;
            bool visible = true;
            int entityCount = 0;
        };

        void RenderAreaList();
        void RenderStreamingSettings();
        void RenderOriginRebaseInfo();

        std::vector<AreaInfo> m_areas;
        int m_selectedArea = -1;
        float m_loadDistance = 500.0f;
        float m_unloadDistance = 600.0f;
        float m_originRebaseThreshold = 5000.0f;
        float m_playerPosition[3] = {};
        int m_loadedAreaCount = 0;
    };

} // namespace SparkEditor
