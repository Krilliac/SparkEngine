/**
 * @file DestructionEditorPanel.h
 * @brief Destruction system editor panel for fracture pattern authoring
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Panel for authoring fracture patterns and managing destructible objects
     *
     * Allows creating/editing fracture patterns (pieces, mass, lifetime),
     * configuring per-entity destructible properties, and monitoring active debris.
     */
    class DestructionEditorPanel : public EditorPanel
    {
      public:
        DestructionEditorPanel();
        ~DestructionEditorPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        struct FracturePieceInfo
        {
            char name[64] = {};
            char meshName[128] = {};
            float localOffset[3] = {};
            float mass = 1.0f;
            float lifetime = 5.0f;
            float scatterForce = 10.0f;
        };

        struct PatternInfo
        {
            char name[64] = {};
            char destructionSound[128] = {};
            char particleEffect[128] = {};
            std::vector<FracturePieceInfo> pieces;
        };

        void RenderPatternList();
        void RenderPatternEditor();
        void RenderDebrisMonitor();

        std::vector<PatternInfo> m_patterns;
        int m_selectedPattern = -1;
        int m_selectedPiece = -1;
        int m_activeDebrisCount = 0;
        int m_maxDebris = 256;
        float m_debrisLifetimeMultiplier = 1.0f;
    };

} // namespace SparkEditor
