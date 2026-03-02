/**
 * @file EditorPanel.h
 * @brief Base class for all editor UI panels
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file defines the base class that all editor panels inherit from,
 * providing a consistent interface for panel management and rendering.
 */

#pragma once

#include <string>
#include <functional>

namespace SparkEditor {

/**
 * @brief Base class for all editor panels
 * 
 * Provides a consistent interface for panel management, including
 * initialization, updating, rendering, and event handling.
 * 
 * All editor panels (Hierarchy, Inspector, Asset Browser, etc.)
 * inherit from this base class.
 */
class EditorPanel {
public:
    /**
     * @brief Constructor
     * @param name Display name of the panel
     * @param id Unique identifier for the panel
     */
    EditorPanel(const std::string& name, const std::string& id);

    /**
     * @brief Virtual destructor
     */
    virtual ~EditorPanel() = default;

    /**
     * @brief Initialize the panel
     * @return true if initialization succeeded, false otherwise
     */
    virtual bool Initialize() = 0;

    /**
     * @brief Update panel for the current frame
     * @param deltaTime Time elapsed since last frame in seconds
     */
    virtual void Update(float deltaTime) = 0;

    /**
     * @brief Render the panel UI
     */
    virtual void Render() = 0;

    /**
     * @brief Shutdown the panel
     */
    virtual void Shutdown() {}

    /**
     * @brief Handle panel-specific events
     * @param eventType Type of event
     * @param eventData Event data (implementation-specific)
     * @return true if event was handled, false otherwise
     */
    virtual bool HandleEvent(const std::string& eventType, void* eventData) { return false; }

    /**
     * @brief Get panel display name
     * @return Display name of the panel
     */
    const std::string& GetName() const { return m_name; }

    /**
     * @brief Get panel unique ID
     * @return Unique identifier of the panel
     */
    const std::string& GetID() const { return m_id; }

    /**
     * @brief Check if panel is visible
     * @return true if panel is visible, false otherwise
     */
    bool IsVisible() const { return m_isVisible; }

    /**
     * @brief Set panel visibility
     * @param visible Whether the panel should be visible
     */
    void SetVisible(bool visible) { m_isVisible = visible; }

    /**
     * @brief Check if panel is focused
     * @return true if panel has focus, false otherwise
     */
    bool IsFocused() const { return m_isFocused; }

    /**
     * @brief Set panel focus state
     * @param focused Whether the panel should be focused
     */
    void SetFocused(bool focused) { m_isFocused = focused; }

    /**
     * @brief Check if panel can be closed
     * @return true if panel can be closed by user, false otherwise
     */
    bool IsClosable() const { return m_isClosable; }

    /**
     * @brief Set whether panel can be closed
     * @param closable Whether the panel can be closed by user
     */
    void SetClosable(bool closable) { m_isClosable = closable; }

    /**
     * @brief Check if panel has unsaved changes
     */
    virtual bool IsModified() const { return m_isModified; }

    /**
     * @brief Set modified state
     */
    void SetModified(bool modified) { m_isModified = modified; }

    /**
     * @brief Get panel type name
     */
    virtual std::string GetTypeName() const { return "EditorPanel"; }

    /**
     * @brief Check if panel can be reset
     */
    virtual bool CanReset() const { return false; }

    /**
     * @brief Save panel state to string
     */
    virtual std::string SaveState() const { return "{}"; }

    /**
     * @brief Load panel state from string
     */
    virtual bool LoadState(const std::string& state) { return true; }

    /**
     * @brief Check if panel is visible in view menu
     */
    bool IsVisibleInMenu() const { return m_visibleInMenu; }

    /**
     * @brief Set visibility in view menu
     */
    void SetVisibleInMenu(bool visible) { m_visibleInMenu = visible; }

    /**
     * @brief Get panel size in pixels
     * @param width Output parameter for panel width
     * @param height Output parameter for panel height
     */
    void GetSize(float& width, float& height) const {
        width = m_width;
        height = m_height;
    }

    /**
     * @brief Set panel size in pixels
     * @param width Panel width
     * @param height Panel height
     */
    void SetSize(float width, float height) {
        m_width = width;
        m_height = height;
    }

    /**
     * @brief Get panel position in pixels
     * @param x Output parameter for panel X position
     * @param y Output parameter for panel Y position
     */
    void GetPosition(float& x, float& y) const {
        x = m_posX;
        y = m_posY;
    }

    /**
     * @brief Set panel position in pixels
     * @param x Panel X position
     * @param y Panel Y position
     */
    void SetPosition(float x, float y) {
        m_posX = x;
        m_posY = y;
    }

    /**
     * @brief Register a callback for when panel state changes
     * @param callback Function to call when panel state changes
     */
    void RegisterStateChangeCallback(std::function<void(EditorPanel*)> callback) {
        m_stateChangeCallback = callback;
    }

protected:
    /**
     * @brief Begin panel rendering (sets up ImGui window)
     * @return true if panel should render content, false if collapsed/hidden
     */
    bool BeginPanel();

    /**
     * @brief End panel rendering (finishes ImGui window)
     */
    void EndPanel();

    /**
     * @brief Notify that panel state has changed
     */
    void NotifyStateChange();

    /**
     * @brief Set panel title (can include icons, status text, etc.)
     * @param title New panel title
     */
    void SetTitle(const std::string& title) { m_title = title; }

    /**
     * @brief Get current panel title
     * @return Current panel title
     */
    const std::string& GetTitle() const { return m_title; }

protected:
    std::string m_name;                                 ///< Panel display name
    std::string m_id;                                   ///< Panel unique ID
    std::string m_title;                                ///< Panel title (can include status)
    
    bool m_isVisible = true;                            ///< Panel visibility state
    bool m_isFocused = false;                           ///< Panel focus state
    bool m_isClosable = true;                           ///< Whether panel can be closed
    bool m_isInitialized = false;                       ///< Initialization state
    
    float m_width = 300.0f;                             ///< Panel width in pixels
    float m_height = 400.0f;                            ///< Panel height in pixels
    float m_posX = 0.0f;                                ///< Panel X position
    float m_posY = 0.0f;                                ///< Panel Y position
    
    bool m_isModified = false;                          ///< Panel modification state
    bool m_visibleInMenu = true;                        ///< Visibility in view menu

    std::string m_icon;                                 ///< FontAwesome icon for panel header

    std::function<void(EditorPanel*)> m_stateChangeCallback; ///< State change callback

public:
    /** @brief Set icon for the panel title bar */
    void SetIcon(const std::string& icon) { m_icon = icon; }

    /** @brief Get icon string */
    const std::string& GetIcon() const { return m_icon; }
};

} // namespace SparkEditor