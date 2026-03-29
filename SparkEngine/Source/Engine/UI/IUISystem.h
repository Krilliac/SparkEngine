/**
 * @file IUISystem.h
 * @brief Abstract interface for the runtime UI system
 * @author Spark Engine Team
 * @date 2026
 *
 * Defines the interface for in-game UI management. This abstraction decouples
 * engine code from the concrete UISystem implementation, enabling access
 * through EngineContext and testability with mock UI.
 */

#pragma once

#include <string>

namespace Spark::UI
{

    class UICanvas;

    /**
     * @brief Abstract interface for the runtime UI system.
     *
     * Provides initialization, update, render, and input handling for
     * in-game HUD and menu UI. The concrete UISystem implements this.
     */
    class IUISystem
    {
      public:
        virtual ~IUISystem() = default;

        // ================================================================
        // Lifecycle
        // ================================================================

        /// @brief Initialize the UI system with screen dimensions.
        virtual void Initialize(int screenWidth, int screenHeight) = 0;

        /// @brief Update all UI elements (call once per frame).
        virtual void Update(float deltaTime) = 0;

        /// @brief Render all UI elements.
        virtual void Render() = 0;

        /// @brief Handle screen resize.
        virtual void OnResize(int width, int height) = 0;

        // ================================================================
        // Input
        // ================================================================

        /// @brief Handle mouse click. Returns true if UI consumed the click.
        virtual bool HandleClick(float x, float y) = 0;

        // ================================================================
        // Visibility
        // ================================================================

        /// @brief Show or hide all UI.
        virtual void SetVisible(bool visible) = 0;

        /// @brief Check if UI is visible.
        virtual bool IsVisible() const = 0;
    };

} // namespace Spark::UI
