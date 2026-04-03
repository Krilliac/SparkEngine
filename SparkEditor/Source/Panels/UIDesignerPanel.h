/**
 * @file UIDesignerPanel.h
 * @brief WYSIWYG UI layout editor panel
 * @author Spark Engine Team
 * @date 2026
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "../UIDesigner/UIDesignerSystem.h"

#include <memory>

namespace SparkEditor
{

    /**
     * @brief Editor panel for visual UI screen design
     *
     * Provides ImGui UI for the UIDesignerSystem: screen management,
     * widget tree, property inspector, canvas preview, and preset creation.
     */
    class UIDesignerPanel : public EditorPanel
    {
      public:
        UIDesignerPanel();
        ~UIDesignerPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

        std::string GetTypeName() const override { return "UIDesignerPanel"; }

      private:
        void RenderScreenList();
        void RenderWidgetTree();
        void RenderWidgetInspector();
        void RenderCanvasPreview();
        void RenderPresetButtons();

        std::unique_ptr<UIDesignerSystem> m_system;
        uint32_t m_selectedScreenId{0};
        uint32_t m_selectedWidgetId{0};
    };

} // namespace SparkEditor
