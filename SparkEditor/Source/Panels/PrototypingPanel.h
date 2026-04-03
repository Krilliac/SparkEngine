/**
 * @file PrototypingPanel.h
 * @brief Editor panel for rapid prototyping tools
 * @author Spark Engine Team
 * @date 2026
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "../Prototyping/PrototypingSystem.h"

#include <memory>

namespace SparkEditor
{

    /**
     * @brief Editor panel for blockout placement, game templates, and gameplay rules
     *
     * Provides ImGui UI for the PrototypingSystem: shape palette for gray-boxing,
     * template selector, gameplay rule editor, and play-test controls.
     */
    class PrototypingPanel : public EditorPanel
    {
      public:
        PrototypingPanel();
        ~PrototypingPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

        std::string GetTypeName() const override { return "PrototypingPanel"; }

      private:
        void RenderBlockoutSection();
        void RenderTemplateSection();
        void RenderRulesSection();
        void RenderPlayTestSection();

        std::unique_ptr<PrototypingSystem> m_system;
        BlockoutShape m_selectedShape{BlockoutShape::Cube};
        int m_selectedTemplateIdx{0};
    };

} // namespace SparkEditor
