/**
 * @file EngineSettings.cpp
 * @brief Implementation of centralized engine settings
 */

#include "EngineSettings.h"
#include "Utils/SparkConsole.h"
#include <filesystem>
#include <sstream>

#ifdef SPARK_PLATFORM_WINDOWS
#include <Windows.h>
#endif

// =============================================================================
// Singleton
// =============================================================================
EngineSettings& EngineSettings::GetInstance() {
    static EngineSettings instance;
    return instance;
}

// =============================================================================
// Find settings path relative to executable
// =============================================================================
std::string EngineSettings::FindSettingsPath() const {
#ifdef SPARK_PLATFORM_WINDOWS
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto dir = std::filesystem::path(exePath).parent_path();
#else
    auto dir = std::filesystem::current_path();
#endif
    // Check Resources/Config/ first, then fall back to exe directory
    auto configDir = dir / "Resources" / "Config";
    if (std::filesystem::exists(configDir / "settings.ini"))
        return (configDir / "settings.ini").string();

    // Check one level up (common in development builds)
    auto parentConfig = dir.parent_path() / "Resources" / "Config";
    if (std::filesystem::exists(parentConfig / "settings.ini"))
        return (parentConfig / "settings.ini").string();

    // Default: create in Resources/Config
    std::filesystem::create_directories(configDir);
    return (configDir / "settings.ini").string();
}

// =============================================================================
// Load
// =============================================================================
bool EngineSettings::Load(const std::string& path) {
    m_filePath = path.empty() ? FindSettingsPath() : path;

    if (m_config.Load(m_filePath)) {
        ReadFromConfig();
        return true;
    }

    // File doesn't exist or failed to parse - use defaults
    PopulateDefaults();
    ReadFromConfig();

    // Save defaults so the file exists for next time
    Save();
    return true;
}

// =============================================================================
// Save
// =============================================================================
bool EngineSettings::Save() const {
    WriteToConfig();
    return m_config.Save(m_filePath);
}

bool EngineSettings::SaveAs(const std::string& path) const {
    WriteToConfig();
    return m_config.Save(path);
}

// =============================================================================
// Reset
// =============================================================================
void EngineSettings::ResetToDefaults() {
    m_graphics = GraphicsSettings{};
    m_audio = AudioSettings{};
    m_controls = ControlsSettings{};
    m_game = GameSettings{};
    PopulateDefaults();
}

// =============================================================================
// Read struct fields from ConfigParser
// =============================================================================
void EngineSettings::ReadFromConfig() {
    // Graphics
    m_graphics.windowWidth   = m_config.GetInt("Graphics", "WindowWidth", 1280);
    m_graphics.windowHeight  = m_config.GetInt("Graphics", "WindowHeight", 720);
    m_graphics.fullscreen    = m_config.GetBool("Graphics", "Fullscreen", false);
    m_graphics.vsync         = m_config.GetBool("Graphics", "VSync", true);
    m_graphics.antiAliasing  = m_config.GetInt("Graphics", "AntiAliasing", 4);
    m_graphics.shadowQuality = m_config.GetInt("Graphics", "ShadowQuality", 2);
    m_graphics.renderScale   = m_config.GetFloat("Graphics", "RenderScale", 1.0f);
    m_graphics.hdr           = m_config.GetBool("Graphics", "HDR", false);

    // Audio
    m_audio.masterVolume     = m_config.GetFloat("Audio", "MasterVolume", 1.0f);
    m_audio.sfxVolume        = m_config.GetFloat("Audio", "SFXVolume", 0.8f);
    m_audio.musicVolume      = m_config.GetFloat("Audio", "MusicVolume", 0.6f);
    m_audio.voiceVolume      = m_config.GetFloat("Audio", "VoiceVolume", 1.0f);
    m_audio.muteOnFocusLoss  = m_config.GetBool("Audio", "MuteOnFocusLoss", true);

    // Controls
    m_controls.mouseSensitivity  = m_config.GetFloat("Controls", "MouseSensitivity", 1.0f);
    m_controls.invertMouseY      = m_config.GetBool("Controls", "InvertMouse", false);
    m_controls.mouseDeadZone     = m_config.GetFloat("Controls", "MouseDeadZone", 0.0f);
    m_controls.rawMouseInput     = m_config.GetBool("Controls", "RawMouseInput", false);
    m_controls.mouseAcceleration = m_config.GetBool("Controls", "MouseAcceleration", false);

    // Game
    m_game.difficulty    = m_config.GetString("Game", "Difficulty", "Normal");
    m_game.showFPS       = m_config.GetBool("Game", "ShowFPS", true);
    m_game.showDebugInfo = m_config.GetBool("Game", "ShowDebugInfo", false);
    m_game.fieldOfView   = m_config.GetFloat("Game", "FieldOfView", 90.0f);
}

// =============================================================================
// Write struct fields to ConfigParser
// =============================================================================
void EngineSettings::WriteToConfig() const {
    // Graphics
    m_config.SetInt("Graphics", "WindowWidth", m_graphics.windowWidth);
    m_config.SetInt("Graphics", "WindowHeight", m_graphics.windowHeight);
    m_config.SetBool("Graphics", "Fullscreen", m_graphics.fullscreen);
    m_config.SetBool("Graphics", "VSync", m_graphics.vsync);
    m_config.SetInt("Graphics", "AntiAliasing", m_graphics.antiAliasing);
    m_config.SetInt("Graphics", "ShadowQuality", m_graphics.shadowQuality);
    m_config.SetFloat("Graphics", "RenderScale", m_graphics.renderScale);
    m_config.SetBool("Graphics", "HDR", m_graphics.hdr);

    // Audio
    m_config.SetFloat("Audio", "MasterVolume", m_audio.masterVolume);
    m_config.SetFloat("Audio", "SFXVolume", m_audio.sfxVolume);
    m_config.SetFloat("Audio", "MusicVolume", m_audio.musicVolume);
    m_config.SetFloat("Audio", "VoiceVolume", m_audio.voiceVolume);
    m_config.SetBool("Audio", "MuteOnFocusLoss", m_audio.muteOnFocusLoss);

    // Controls
    m_config.SetFloat("Controls", "MouseSensitivity", m_controls.mouseSensitivity);
    m_config.SetBool("Controls", "InvertMouse", m_controls.invertMouseY);
    m_config.SetFloat("Controls", "MouseDeadZone", m_controls.mouseDeadZone);
    m_config.SetBool("Controls", "RawMouseInput", m_controls.rawMouseInput);
    m_config.SetBool("Controls", "MouseAcceleration", m_controls.mouseAcceleration);

    // Game
    m_config.SetString("Game", "Difficulty", m_game.difficulty);
    m_config.SetBool("Game", "ShowFPS", m_game.showFPS);
    m_config.SetBool("Game", "ShowDebugInfo", m_game.showDebugInfo);
    m_config.SetFloat("Game", "FieldOfView", m_game.fieldOfView);
}

// =============================================================================
// Populate defaults into the ConfigParser
// =============================================================================
void EngineSettings::PopulateDefaults() {
    m_config.Clear();

    // Write current struct values (which have their defaults) into the parser
    WriteToConfig();
}

// =============================================================================
// Generic key access
// =============================================================================
std::string EngineSettings::GetValue(const std::string& section, const std::string& key) const {
    // Sync struct -> config first
    WriteToConfig();
    return m_config.GetString(section, key, "");
}

bool EngineSettings::SetValue(const std::string& section, const std::string& key, const std::string& value) {
    if (!m_config.HasSection(section) && section != "Graphics" && section != "Audio"
        && section != "Controls" && section != "Game") {
        return false;
    }

    m_config.SetString(section, key, value);
    ReadFromConfig(); // Sync back to structs
    return true;
}

std::vector<std::string> EngineSettings::GetSections() const {
    return {"Graphics", "Audio", "Controls", "Game"};
}

std::vector<std::string> EngineSettings::GetKeys(const std::string& section) const {
    WriteToConfig();
    return m_config.GetKeys(section);
}

// =============================================================================
// Console commands
// =============================================================================
void EngineSettings::RegisterConsoleCommands() {
    auto& console = Spark::SimpleConsole::GetInstance();

    console.RegisterCommand("settings_get", [](const std::vector<std::string>& args) -> std::string {
        if (args.size() < 2) return "Usage: settings_get <section> <key>";
        auto& settings = EngineSettings::GetInstance();
        std::string val = settings.GetValue(args[0], args[1]);
        if (val.empty()) return "Key not found: " + args[0] + "." + args[1];
        return args[0] + "." + args[1] + " = " + val;
    }, "Get a settings value", "Settings");

    console.RegisterCommand("settings_set", [](const std::vector<std::string>& args) -> std::string {
        if (args.size() < 3) return "Usage: settings_set <section> <key> <value>";
        auto& settings = EngineSettings::GetInstance();
        if (settings.SetValue(args[0], args[1], args[2])) {
            return "Set " + args[0] + "." + args[1] + " = " + args[2];
        }
        return "Failed to set " + args[0] + "." + args[1];
    }, "Set a settings value", "Settings");

    console.RegisterCommand("settings_save", [](const std::vector<std::string>&) -> std::string {
        auto& settings = EngineSettings::GetInstance();
        if (settings.Save()) {
            return "Settings saved to " + settings.GetFilePath();
        }
        return "Failed to save settings";
    }, "Save settings to disk", "Settings");

    console.RegisterCommand("settings_reload", [](const std::vector<std::string>&) -> std::string {
        auto& settings = EngineSettings::GetInstance();
        if (settings.Load(settings.GetFilePath())) {
            return "Settings reloaded from " + settings.GetFilePath();
        }
        return "Failed to reload settings";
    }, "Reload settings from disk", "Settings");

    console.RegisterCommand("settings_reset", [](const std::vector<std::string>&) -> std::string {
        auto& settings = EngineSettings::GetInstance();
        settings.ResetToDefaults();
        return "Settings reset to defaults (use settings_save to persist)";
    }, "Reset all settings to defaults", "Settings");

    console.RegisterCommand("settings_list", [](const std::vector<std::string>& args) -> std::string {
        auto& settings = EngineSettings::GetInstance();
        std::stringstream ss;

        std::vector<std::string> sections;
        if (!args.empty()) {
            sections.push_back(args[0]);
        } else {
            sections = settings.GetSections();
        }

        for (const auto& section : sections) {
            ss << "[" << section << "]\n";
            auto keys = settings.GetKeys(section);
            for (const auto& key : keys) {
                ss << "  " << key << " = " << settings.GetValue(section, key) << "\n";
            }
        }
        return ss.str();
    }, "List all settings (or settings in a section)", "Settings");
}
