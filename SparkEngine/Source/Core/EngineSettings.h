/**
 * @file EngineSettings.h
 * @brief Centralized engine settings loaded from INI configuration
 * @author Spark Engine Team
 * @date 2025
 *
 * Wraps ConfigParser to provide typed accessors for all engine settings,
 * default value population, and console integration for live tweaking.
 */

#pragma once

#include "Utils/ConfigParser.h"
#include <string>

class EngineSettings
{
  public:
    // =========================================================================
    // Settings structures
    // =========================================================================

    struct GraphicsSettings
    {
        int windowWidth = 1280;
        int windowHeight = 720;
        bool fullscreen = false;
        bool vsync = true;
        int antiAliasing = 4;     // MSAA sample count (1, 2, 4, 8)
        int shadowQuality = 2;    // 0=Off, 1=Low, 2=Medium, 3=High
        float renderScale = 1.0f; // Internal resolution scale
        bool hdr = false;
    };

    struct AudioSettings
    {
        float masterVolume = 1.0f;
        float sfxVolume = 0.8f;
        float musicVolume = 0.6f;
        float voiceVolume = 1.0f;
        bool muteOnFocusLoss = true;
    };

    struct ControlsSettings
    {
        float mouseSensitivity = 1.0f;
        bool invertMouseY = false;
        float mouseDeadZone = 0.0f;
        bool rawMouseInput = false;
        bool mouseAcceleration = false;
    };

    struct GameSettings
    {
        std::string difficulty = "Normal";
        bool showFPS = true;
        bool showDebugInfo = false;
        float fieldOfView = 90.0f;
    };

    // =========================================================================
    // Singleton access
    // =========================================================================

    static EngineSettings& GetInstance();

    // =========================================================================
    // Lifecycle
    // =========================================================================

    /// Load settings from file. Creates defaults if file doesn't exist.
    bool Load(const std::string& path = "");

    /// Save current settings to file.
    bool Save() const;

    /// Save to a specific path.
    bool SaveAs(const std::string& path) const;

    /// Reset all settings to defaults.
    void ResetToDefaults();

    /// Get the path settings were loaded from.
    const std::string& GetFilePath() const { return m_filePath; }

    // =========================================================================
    // Typed accessors
    // =========================================================================

    GraphicsSettings& Graphics() { return m_graphics; }
    const GraphicsSettings& Graphics() const { return m_graphics; }

    AudioSettings& Audio() { return m_audio; }
    const AudioSettings& Audio() const { return m_audio; }

    ControlsSettings& Controls() { return m_controls; }
    const ControlsSettings& Controls() const { return m_controls; }

    GameSettings& Game() { return m_game; }
    const GameSettings& Game() const { return m_game; }

    // =========================================================================
    // Generic key access (for console commands)
    // =========================================================================

    /// Get a setting value as string: "Graphics.WindowWidth" -> "1280"
    std::string GetValue(const std::string& section, const std::string& key) const;

    /// Set a setting value from string: "Graphics", "WindowWidth", "1920"
    bool SetValue(const std::string& section, const std::string& key, const std::string& value);

    /// Get all section names.
    std::vector<std::string> GetSections() const;

    /// Get all keys in a section.
    std::vector<std::string> GetKeys(const std::string& section) const;

    // =========================================================================
    // Console command registration
    // =========================================================================

    /// Register settings-related console commands with SparkConsole.
    void RegisterConsoleCommands();

  private:
    EngineSettings() = default;

    /// Read struct fields from the ConfigParser.
    void ReadFromConfig();

    /// Write struct fields to the ConfigParser.
    void WriteToConfig() const;

    /// Populate the ConfigParser with sensible defaults.
    void PopulateDefaults();

    /// Find settings.ini relative to executable.
    std::string FindSettingsPath() const;

    // Data
    mutable Spark::ConfigParser m_config;
    std::string m_filePath;

    GraphicsSettings m_graphics;
    AudioSettings m_audio;
    ControlsSettings m_controls;
    GameSettings m_game;
};
