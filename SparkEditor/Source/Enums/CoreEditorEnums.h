/**
 * @file CoreEditorEnums.h
 * @brief Core enumerations for the editor application
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file contains fundamental enumerations used throughout the editor,
 * including dock positions, panel types, and core editor states.
 */

#pragma once

namespace SparkEditor
{

    /**
 * @brief Dock position enumeration for panel docking
 */
    enum class DockPosition
    {
        Left,    ///< Dock to left side
        Right,   ///< Dock to right side
        Top,     ///< Dock to top
        Bottom,  ///< Dock to bottom
        Center,  ///< Dock to center
        Tab,     ///< Add as tab
        Floating ///< Floating window
    };

    /**
 * @brief Editor panel types
 */
    enum class PanelType
    {
        HIERARCHY = 0,         ///< Scene hierarchy panel
        INSPECTOR = 1,         ///< Property inspector panel
        ASSET_BROWSER = 2,     ///< Asset browser panel
        CONSOLE = 3,           ///< Console panel
        VIEWPORT = 4,          ///< Scene viewport panel
        PROFILER = 5,          ///< Performance profiler panel
        VERSION_CONTROL = 6,   ///< Version control panel
        BUILD_SYSTEM = 7,      ///< Build system panel
        LEVEL_STREAMING = 8,   ///< Level streaming panel
        MATERIAL_EDITOR = 9,   ///< Material editor panel
        ANIMATION_EDITOR = 10, ///< Animation editor panel
        TERRAIN_EDITOR = 11,   ///< Terrain editor panel
        AUDIO_MIXER = 12,      ///< Audio mixer panel
        CUSTOM = 100           ///< Custom panel type
    };

    /**
 * @brief Editor application states
 */
    enum class EditorState
    {
        INITIALIZING = 0, ///< Editor is initializing
        READY = 1,        ///< Editor is ready for use
        PLAYING = 2,      ///< Game is playing in editor
        PAUSED = 3,       ///< Game is paused
        BUILDING = 4,     ///< Building project
        SHUTTING_DOWN = 5 ///< Editor is shutting down
    };

    /**
 * @brief Editor theme types
 */
    enum class EditorTheme
    {
        DARK = 0,          ///< Dark theme
        LIGHT = 1,         ///< Light theme
        CLASSIC = 2,       ///< Classic theme
        HIGH_CONTRAST = 3, ///< High contrast theme
        CUSTOM = 4         ///< Custom theme
    };

    /**
 * @brief Editor layout modes
 */
    enum class LayoutMode
    {
        DEFAULT = 0,    ///< Default layout
        MINIMAL = 1,    ///< Minimal layout
        DEBUG = 2,      ///< Debug layout
        ARTIST = 3,     ///< Artist-friendly layout
        PROGRAMMER = 4, ///< Programmer layout
        CUSTOM = 5      ///< Custom layout
    };

    /**
 * @brief Input capture modes
 */
    enum class InputCaptureMode
    {
        NONE = 0,          ///< No input capture
        VIEWPORT_ONLY = 1, ///< Capture input only in viewport
        ALWAYS = 2,        ///< Always capture input
        PLAY_MODE_ONLY = 3 ///< Capture input only in play mode
    };

    /**
 * @brief Selection modes
 */
    enum class SelectionMode
    {
        SINGLE = 0,     ///< Single object selection
        MULTIPLE = 1,   ///< Multiple object selection
        ADDITIVE = 2,   ///< Additive selection (Ctrl+click)
        SUBTRACTIVE = 3 ///< Subtractive selection (Shift+click)
    };

} // namespace SparkEditor