/**
 * @file UISystem.h
 * @brief Runtime UI/Widget system for in-game HUD and menus
 * @author Spark Engine Team
 * @date 2025
 *
 * @details
 * Provides a reusable, engine-level UI framework for in-game HUD elements,
 * menus, and overlays. Unlike the editor's Dear ImGui panels, this system
 * is designed for runtime game UI with proper layout, anchoring, and input.
 *
 * ## Widget hierarchy
 * ```
 * UICanvas (root, one per viewport)
 *   +-- UIPanel "HUD"
 *   |    +-- UILabel "Health: 100"
 *   |    +-- UIProgressBar (health bar)
 *   |    +-- UIImageWidget (crosshair)
 *   +-- UIPanel "PauseMenu"
 *        +-- UILabel "PAUSED"
 *        +-- UIButton "Resume"
 *        +-- UIButton "Quit"
 * ```
 *
 * ## Usage
 * @code
 *   UISystem ui;
 *   ui.Initialize(1920, 1080);
 *
 *   auto* hud = ui.GetCanvas().CreatePanel("HUD");
 *   hud->SetAnchor(Anchor::TopLeft);
 *
 *   auto* healthLabel = hud->CreateLabel("health_text", "Health: 100");
 *   healthLabel->SetPosition(20, 20);
 *   healthLabel->SetFontSize(24);
 *
 *   auto* healthBar = hud->CreateProgressBar("health_bar");
 *   healthBar->SetPosition(20, 50);
 *   healthBar->SetSize(200, 20);
 *   healthBar->SetValue(1.0f);
 *
 *   auto* fireBtn = hud->CreateButton("fire", "Fire!");
 *   fireBtn->OnClick([](){ });
 *
 *   // Per frame:
 *   ui.Update(deltaTime);
 *   ui.Render();
 * @endcode
 */

#pragma once

// Phase R: activated Tier 2 graphics orphan — per-widget compositor
// stack with pooled render-target metadata. Pure CPU, runs on every
// platform. Owned by UISystem so the widget layer can push / pop
// composition levels for nested effects (blur, mask, opacity).
#include "../../Graphics/UICompositor.h"

#include <algorithm>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <cstdint>

namespace Spark::UI
{

    // =============================================================================
    // Anchor & Layout
    // =============================================================================

    /**
 * @brief Screen anchor points for UI widget positioning.
 */
    enum class Anchor
    {
        TopLeft,
        TopCenter,
        TopRight,
        MiddleLeft,
        Center,
        MiddleRight,
        BottomLeft,
        BottomCenter,
        BottomRight,
        Stretch ///< Fill the entire parent area
    };

    /**
 * @brief Layout direction for automatic child arrangement.
 */
    enum class LayoutDirection
    {
        None,       ///< Manual positioning
        Horizontal, ///< Left-to-right
        Vertical    ///< Top-to-bottom
    };

    /**
 * @brief Color for UI elements (RGBA, 0.0 - 1.0).
 */
    struct UIColor
    {
        float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;

        static UIColor White() { return {1, 1, 1, 1}; }
        static UIColor Black() { return {0, 0, 0, 1}; }
        static UIColor Red() { return {1, 0, 0, 1}; }
        static UIColor Green() { return {0, 1, 0, 1}; }
        static UIColor Blue() { return {0, 0, 1, 1}; }
        static UIColor Yellow() { return {1, 1, 0, 1}; }
        static UIColor Transparent() { return {0, 0, 0, 0}; }
    };

    // =============================================================================
    // UIWidget (base class)
    // =============================================================================

    /**
 * @brief Base class for all UI widgets.
 */
    class UIWidget
    {
      public:
        UIWidget(const std::string& name);
        virtual ~UIWidget() = default;

        /** @brief Get the widget name. */
        const std::string& GetName() const { return m_name; }

        /** @brief Set world-space position relative to parent. */
        void SetPosition(float x, float y)
        {
            m_x = x;
            m_y = y;
        }

        /** @brief Set widget size. */
        void SetSize(float width, float height)
        {
            m_width = width;
            m_height = height;
        }

        /** @brief Set anchor point. */
        void SetAnchor(Anchor anchor) { m_anchor = anchor; }

        /** @brief Set visibility. */
        void SetVisible(bool visible) { m_visible = visible; }

        /** @brief Check visibility. */
        bool IsVisible() const { return m_visible; }

        /** @brief Set opacity (0.0 = transparent, 1.0 = opaque). */
        void SetOpacity(float opacity) { m_opacity = opacity; }

        /** @brief Get position X. */
        float GetX() const { return m_x; }
        /** @brief Get position Y. */
        float GetY() const { return m_y; }
        /** @brief Get width. */
        float GetWidth() const { return m_width; }
        /** @brief Get height. */
        float GetHeight() const { return m_height; }

        /** @brief Update the widget (called per frame). */
        virtual void Update(float deltaTime);

        /** @brief Render the widget. */
        virtual void Render() const;

        /** @brief Handle mouse click at (x, y). Returns true if consumed. */
        virtual bool HandleClick(float x, float y);

      protected:
        std::string m_name;
        float m_x = 0.0f, m_y = 0.0f;
        float m_width = 100.0f, m_height = 30.0f;
        Anchor m_anchor = Anchor::TopLeft;
        bool m_visible = true;
        float m_opacity = 1.0f;
    };

    // =============================================================================
    // UILabel
    // =============================================================================

    /**
 * @brief Text label widget.
 */
    class UILabel : public UIWidget
    {
      public:
        UILabel(const std::string& name, const std::string& text);

        void SetText(const std::string& text) { m_text = text; }
        const std::string& GetText() const { return m_text; }

        void SetFontSize(int size) { m_fontSize = size; }
        void SetColor(const UIColor& color) { m_color = color; }

        void Render() const override;

      private:
        std::string m_text;
        int m_fontSize = 16;
        UIColor m_color = UIColor::White();
    };

    // =============================================================================
    // UIButton
    // =============================================================================

    /**
 * @brief Clickable button widget.
 */
    class UIButton : public UIWidget
    {
      public:
        UIButton(const std::string& name, const std::string& label);

        void SetLabel(const std::string& label) { m_label = label; }
        const std::string& GetLabel() const { return m_label; }

        void OnClick(std::function<void()> callback) { m_onClick = std::move(callback); }

        void SetNormalColor(const UIColor& color) { m_normalColor = color; }
        void SetHoverColor(const UIColor& color) { m_hoverColor = color; }
        void SetPressedColor(const UIColor& color) { m_pressedColor = color; }

        bool HandleClick(float x, float y) override;
        void Render() const override;

      private:
        std::string m_label;
        std::function<void()> m_onClick;
        UIColor m_normalColor{0.3f, 0.3f, 0.3f, 0.9f};
        UIColor m_hoverColor{0.4f, 0.4f, 0.4f, 0.9f};
        UIColor m_pressedColor{0.2f, 0.2f, 0.2f, 0.9f};
        bool m_hovered = false;
        bool m_pressed = false;
    };

    // =============================================================================
    // UIProgressBar
    // =============================================================================

    /**
 * @brief Progress bar widget (e.g. health bar, loading bar).
 */
    class UIProgressBar : public UIWidget
    {
      public:
        UIProgressBar(const std::string& name);

        void SetValue(float value) { m_value = std::clamp(value, 0.0f, 1.0f); }
        float GetValue() const { return m_value; }

        void SetFillColor(const UIColor& color) { m_fillColor = color; }
        void SetBackgroundColor(const UIColor& color) { m_bgColor = color; }

        void Render() const override;

      private:
        float m_value = 1.0f;
        UIColor m_fillColor = UIColor::Green();
        UIColor m_bgColor{0.2f, 0.2f, 0.2f, 0.8f};
    };

    // =============================================================================
    // UIImageWidget
    // =============================================================================

    /**
 * @brief Image display widget.
 */
    class UIImageWidget : public UIWidget
    {
      public:
        UIImageWidget(const std::string& name, const std::string& texturePath = "");

        void SetTexturePath(const std::string& path) { m_texturePath = path; }
        const std::string& GetTexturePath() const { return m_texturePath; }

        void SetTint(const UIColor& color) { m_tint = color; }

        void Render() const override;

      private:
        std::string m_texturePath;
        UIColor m_tint = UIColor::White();
    };

    // =============================================================================
    // UIPanel
    // =============================================================================

    /**
 * @brief Container widget that holds child widgets with layout.
 */
    class UIPanel : public UIWidget
    {
      public:
        UIPanel(const std::string& name);

        /** @brief Set the layout direction for automatic child arrangement. */
        void SetLayout(LayoutDirection layout) { m_layout = layout; }

        /** @brief Set spacing between children in auto-layout mode. */
        void SetSpacing(float spacing) { m_spacing = spacing; }

        /** @brief Set padding inside the panel. */
        void SetPadding(float padding) { m_padding = padding; }

        /** @brief Set background color. */
        void SetBackgroundColor(const UIColor& color) { m_bgColor = color; }

        // --- Child creation ---

        UILabel* CreateLabel(const std::string& name, const std::string& text);
        UIButton* CreateButton(const std::string& name, const std::string& label);
        UIProgressBar* CreateProgressBar(const std::string& name);
        UIImageWidget* CreateImage(const std::string& name, const std::string& texturePath = "");
        UIPanel* CreatePanel(const std::string& name);

        /** @brief Find a child widget by name (recursive). */
        UIWidget* FindWidget(const std::string& name);

        /** @brief Remove a child widget by name. */
        void RemoveWidget(const std::string& name);

        void Update(float deltaTime) override;
        void Render() const override;
        bool HandleClick(float x, float y) override;

      private:
        std::vector<std::unique_ptr<UIWidget>> m_children;
        LayoutDirection m_layout = LayoutDirection::None;
        float m_spacing = 5.0f;
        float m_padding = 10.0f;
        UIColor m_bgColor = UIColor::Transparent();
    };

    // =============================================================================
    // UICanvas
    // =============================================================================

    /**
 * @brief Root UI canvas -- one per viewport.
 *
 * The canvas manages all top-level panels and handles coordinate mapping
 * from screen space to UI space.
 */
    class UICanvas
    {
      public:
        UICanvas();
        ~UICanvas() = default;

        /**
     * @brief Initialize with screen resolution.
     * @param width  Screen width in pixels.
     * @param height Screen height in pixels.
     */
        void Initialize(int width, int height);

        /**
     * @brief Create a top-level panel.
     * @param name Panel name.
     * @return Pointer to the created panel.
     */
        UIPanel* CreatePanel(const std::string& name);

        /** @brief Find a widget by name across all panels. */
        UIWidget* FindWidget(const std::string& name);

        /** @brief Remove a panel by name. */
        void RemovePanel(const std::string& name);

        /** @brief Update all panels. */
        void Update(float deltaTime);

        /** @brief Render all visible panels. */
        void Render() const;

        /** @brief Handle a click event. */
        bool HandleClick(float x, float y);

        /** @brief Get screen width. */
        int GetWidth() const { return m_width; }

        /** @brief Get screen height. */
        int GetHeight() const { return m_height; }

        /** @brief Resize the canvas. */
        void Resize(int width, int height);

      private:
        std::vector<std::unique_ptr<UIPanel>> m_panels;
        int m_width = 1920;
        int m_height = 1080;
    };

    // =============================================================================
    // UISystem
    // =============================================================================

    /**
 * \@class UISystem
 * @brief Top-level UI management system.
 *
 * Owns the UICanvas and provides convenience methods for common HUD operations.
 */
    class UISystem
    {
      public:
        UISystem();
        ~UISystem() = default;

        /**
     * @brief Initialize the UI system.
     * @param screenWidth  Screen width.
     * @param screenHeight Screen height.
     */
        void Initialize(int screenWidth, int screenHeight);

        /** @brief Get the root canvas. */
        UICanvas& GetCanvas() { return m_canvas; }
        const UICanvas& GetCanvas() const { return m_canvas; }

        /** @brief Update all UI elements. */
        void Update(float deltaTime);

        /** @brief Render all UI elements. */
        void Render();

        /** @brief Handle screen resize. */
        void OnResize(int width, int height);

        /** @brief Handle mouse click. Returns true if UI consumed the click. */
        bool HandleClick(float x, float y);

        /** @brief Show or hide all UI. */
        void SetVisible(bool visible) { m_visible = visible; }
        bool IsVisible() const { return m_visible; }

        // --- Phase R: UICompositor accessor ---

        /**
         * @brief Get the per-widget composition stack (Phase R activation).
         *
         * The `UICompositor` is lifecycle-owned by the UI system. It
         * manages a pool of intermediate render-target metadata slots
         * and a `kMaxStackDepth = 8` composition stack so widgets can
         * push a `CompositeRequest` (opacity, blur, mask, etc.),
         * render to an intermediate target, and pop back to the parent.
         * Simple widgets (full opacity, no effects) skip the push /
         * pop via `NeedsComposition()` and render straight to the
         * parent RT.
         *
         * The compositor is initialised from `UISystem::Initialize`
         * with the viewport size, ticked via `BeginFrame()` from
         * `UISystem::Render()` (which reclaims pool entries unused
         * for 10+ frames), and re-initialised on `OnResize` so the
         * pool resets at the new screen size.
         */
        Spark::Graphics::UICompositor& GetCompositor() { return m_compositor; }
        const Spark::Graphics::UICompositor& GetCompositor() const { return m_compositor; }

        // --- Console integration ---

        /** @brief Get UI system status (console integration). */
        std::string Console_GetStatus() const;

      private:
        UICanvas m_canvas;
        // Phase R: per-widget compositor stack. Pure CPU bookkeeping
        // — the header documents that "GPU resources would be
        // ComPtr<ID3D11Texture2D>, etc. Kept abstract for cross-
        // platform compatibility", so the member lives outside any
        // Windows guard and runs on every platform.
        Spark::Graphics::UICompositor m_compositor;
        bool m_visible = true;
    };

} // namespace Spark::UI
