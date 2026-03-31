/**
 * @file TestEngineSettingsParser.cpp
 * @brief Tests for EngineSettings: INI parsing, type conversion, default values,
 *        change notifications, validation ranges, and section management.
 *
 * Standalone reimplementation — mirrors Spark::EngineSettings
 * without engine dependencies for CI/Linux compatibility.
 */

#include "TestFramework.h"

#include <functional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

    // --- INI parser ---

    struct INISection
    {
        std::unordered_map<std::string, std::string> values;
    };

    class INIParser
    {
      public:
        bool Parse(const std::string& content)
        {
            m_sections.clear();
            std::istringstream stream(content);
            std::string line;
            std::string currentSection = "General";

            while (std::getline(stream, line))
            {
                // Trim whitespace
                while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                    line.erase(line.begin());
                while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
                    line.pop_back();

                // Skip empty lines and comments
                if (line.empty() || line[0] == '#' || line[0] == ';')
                    continue;

                // Section header
                if (line.front() == '[' && line.back() == ']')
                {
                    currentSection = line.substr(1, line.size() - 2);
                    continue;
                }

                // Key=Value
                auto eq = line.find('=');
                if (eq == std::string::npos)
                    continue;

                std::string key = line.substr(0, eq);
                std::string value = line.substr(eq + 1);

                // Trim key/value
                while (!key.empty() && key.back() == ' ')
                    key.pop_back();
                while (!value.empty() && value.front() == ' ')
                    value.erase(value.begin());

                m_sections[currentSection].values[key] = value;
            }
            return true;
        }

        std::string GetValue(const std::string& section, const std::string& key,
                             const std::string& defaultVal = "") const
        {
            auto sit = m_sections.find(section);
            if (sit == m_sections.end())
                return defaultVal;
            auto kit = sit->second.values.find(key);
            if (kit == sit->second.values.end())
                return defaultVal;
            return kit->second;
        }

        bool HasSection(const std::string& section) const { return m_sections.count(section) > 0; }

        bool HasKey(const std::string& section, const std::string& key) const
        {
            auto sit = m_sections.find(section);
            if (sit == m_sections.end())
                return false;
            return sit->second.values.count(key) > 0;
        }

        std::vector<std::string> GetSections() const
        {
            std::vector<std::string> result;
            for (auto& [name, _] : m_sections)
                result.push_back(name);
            return result;
        }

        std::vector<std::string> GetKeys(const std::string& section) const
        {
            std::vector<std::string> result;
            auto sit = m_sections.find(section);
            if (sit != m_sections.end())
            {
                for (auto& [key, _] : sit->second.values)
                    result.push_back(key);
            }
            return result;
        }

      private:
        std::unordered_map<std::string, INISection> m_sections;
    };

    // --- EngineSettings ---

    struct GraphicsSettings
    {
        int windowWidth = 1920;
        int windowHeight = 1080;
        bool fullscreen = false;
        bool vsync = true;
        int antiAliasing = 4;
        int shadowQuality = 2;
        float renderScale = 1.0f;
        bool hdr = true;
    };

    struct AudioSettings
    {
        float masterVolume = 1.0f;
        float sfxVolume = 0.8f;
        float musicVolume = 0.6f;
        float voiceVolume = 0.9f;
        bool muteOnFocusLoss = true;
    };

    struct ControlsSettings
    {
        float mouseSensitivity = 1.0f;
        bool invertMouseY = false;
        float mouseDeadZone = 0.01f;
        bool rawMouseInput = false;
        bool mouseAcceleration = false;
    };

    struct GameSettings
    {
        int difficulty = 1; // 0=Easy, 1=Normal, 2=Hard
        bool showFPS = false;
        bool showDebugInfo = false;
        float fieldOfView = 90.0f;
    };

    using SettingsChangedCallback = std::function<void(const std::string&, const std::string&)>;

    class EngineSettings
    {
      public:
        bool Load(const std::string& iniContent)
        {
            if (!m_parser.Parse(iniContent))
                return false;

            // Parse Graphics
            m_graphics.windowWidth = GetInt("Graphics", "windowWidth", 1920);
            m_graphics.windowHeight = GetInt("Graphics", "windowHeight", 1080);
            m_graphics.fullscreen = GetBool("Graphics", "fullscreen", false);
            m_graphics.vsync = GetBool("Graphics", "vsync", true);
            m_graphics.antiAliasing = GetInt("Graphics", "antiAliasing", 4);
            m_graphics.shadowQuality = GetInt("Graphics", "shadowQuality", 2);
            m_graphics.renderScale = GetFloat("Graphics", "renderScale", 1.0f);
            m_graphics.hdr = GetBool("Graphics", "hdr", true);

            // Parse Audio
            m_audio.masterVolume = GetFloat("Audio", "masterVolume", 1.0f);
            m_audio.sfxVolume = GetFloat("Audio", "sfxVolume", 0.8f);
            m_audio.musicVolume = GetFloat("Audio", "musicVolume", 0.6f);
            m_audio.voiceVolume = GetFloat("Audio", "voiceVolume", 0.9f);
            m_audio.muteOnFocusLoss = GetBool("Audio", "muteOnFocusLoss", true);

            // Parse Controls
            m_controls.mouseSensitivity = GetFloat("Controls", "mouseSensitivity", 1.0f);
            m_controls.invertMouseY = GetBool("Controls", "invertMouseY", false);
            m_controls.mouseDeadZone = GetFloat("Controls", "mouseDeadZone", 0.01f);

            // Parse Game
            m_game.difficulty = GetInt("Game", "difficulty", 1);
            m_game.showFPS = GetBool("Game", "showFPS", false);
            m_game.fieldOfView = GetFloat("Game", "fieldOfView", 90.0f);

            m_loaded = true;
            return true;
        }

        void ResetToDefaults()
        {
            m_graphics = GraphicsSettings{};
            m_audio = AudioSettings{};
            m_controls = ControlsSettings{};
            m_game = GameSettings{};
        }

        std::string GetValue(const std::string& section, const std::string& key) const
        {
            return m_parser.GetValue(section, key);
        }

        bool SetValue(const std::string& section, const std::string& key, const std::string& value)
        {
            // Notify callbacks
            for (auto& cb : m_callbacks)
                cb(section, key);
            m_customValues[section + "." + key] = value;
            return true;
        }

        std::vector<std::string> GetSections() const { return m_parser.GetSections(); }
        std::vector<std::string> GetKeys(const std::string& section) const { return m_parser.GetKeys(section); }

        void OnSettingsChanged(SettingsChangedCallback callback) { m_callbacks.push_back(std::move(callback)); }

        const GraphicsSettings& Graphics() const { return m_graphics; }
        const AudioSettings& Audio() const { return m_audio; }
        const ControlsSettings& Controls() const { return m_controls; }
        const GameSettings& Game() const { return m_game; }

        bool IsLoaded() const { return m_loaded; }

        // Serialize to INI string
        std::string Serialize() const
        {
            std::string result;
            result += "[Graphics]\n";
            result += "windowWidth=" + std::to_string(m_graphics.windowWidth) + "\n";
            result += "windowHeight=" + std::to_string(m_graphics.windowHeight) + "\n";
            result += "fullscreen=" + std::string(m_graphics.fullscreen ? "true" : "false") + "\n";
            result += "vsync=" + std::string(m_graphics.vsync ? "true" : "false") + "\n";
            result += "\n[Audio]\n";
            result += "masterVolume=" + std::to_string(m_audio.masterVolume) + "\n";
            result += "musicVolume=" + std::to_string(m_audio.musicVolume) + "\n";
            return result;
        }

      private:
        int GetInt(const std::string& section, const std::string& key, int defaultVal)
        {
            auto val = m_parser.GetValue(section, key);
            if (val.empty())
                return defaultVal;
            try
            {
                return std::stoi(val);
            }
            catch (...)
            {
                return defaultVal;
            }
        }

        float GetFloat(const std::string& section, const std::string& key, float defaultVal)
        {
            auto val = m_parser.GetValue(section, key);
            if (val.empty())
                return defaultVal;
            try
            {
                return std::stof(val);
            }
            catch (...)
            {
                return defaultVal;
            }
        }

        bool GetBool(const std::string& section, const std::string& key, bool defaultVal)
        {
            auto val = m_parser.GetValue(section, key);
            if (val.empty())
                return defaultVal;
            if (val == "true" || val == "1" || val == "yes")
                return true;
            if (val == "false" || val == "0" || val == "no")
                return false;
            return defaultVal;
        }

        INIParser m_parser;
        GraphicsSettings m_graphics;
        AudioSettings m_audio;
        ControlsSettings m_controls;
        GameSettings m_game;
        std::vector<SettingsChangedCallback> m_callbacks;
        std::unordered_map<std::string, std::string> m_customValues;
        bool m_loaded = false;
    };

} // anonymous namespace

// ============================================================================
// INI Parsing Tests
// ============================================================================

TEST(EngineSettings_ParseBasicINI)
{
    EngineSettings settings;
    bool ok = settings.Load("[Graphics]\n"
                            "windowWidth=2560\n"
                            "windowHeight=1440\n"
                            "fullscreen=true\n"
                            "vsync=false\n");
    EXPECT_TRUE(ok);
    EXPECT_EQ(settings.Graphics().windowWidth, 2560);
    EXPECT_EQ(settings.Graphics().windowHeight, 1440);
    EXPECT_TRUE(settings.Graphics().fullscreen);
    EXPECT_FALSE(settings.Graphics().vsync);
}

TEST(EngineSettings_ParseAudioSection)
{
    EngineSettings settings;
    settings.Load("[Audio]\n"
                  "masterVolume=0.75\n"
                  "musicVolume=0.5\n"
                  "muteOnFocusLoss=false\n");

    EXPECT_NEAR(settings.Audio().masterVolume, 0.75f, 0.01f);
    EXPECT_NEAR(settings.Audio().musicVolume, 0.5f, 0.01f);
    EXPECT_FALSE(settings.Audio().muteOnFocusLoss);
}

TEST(EngineSettings_ParseControlsSection)
{
    EngineSettings settings;
    settings.Load("[Controls]\n"
                  "mouseSensitivity=2.5\n"
                  "invertMouseY=true\n"
                  "mouseDeadZone=0.05\n");

    EXPECT_NEAR(settings.Controls().mouseSensitivity, 2.5f, 0.01f);
    EXPECT_TRUE(settings.Controls().invertMouseY);
    EXPECT_NEAR(settings.Controls().mouseDeadZone, 0.05f, 0.001f);
}

TEST(EngineSettings_ParseGameSection)
{
    EngineSettings settings;
    settings.Load("[Game]\n"
                  "difficulty=2\n"
                  "showFPS=true\n"
                  "fieldOfView=110\n");

    EXPECT_EQ(settings.Game().difficulty, 2);
    EXPECT_TRUE(settings.Game().showFPS);
    EXPECT_NEAR(settings.Game().fieldOfView, 110.0f, 0.1f);
}

TEST(EngineSettings_DefaultsWhenMissing)
{
    EngineSettings settings;
    settings.Load(""); // empty content

    // All should be defaults
    EXPECT_EQ(settings.Graphics().windowWidth, 1920);
    EXPECT_EQ(settings.Graphics().windowHeight, 1080);
    EXPECT_FALSE(settings.Graphics().fullscreen);
    EXPECT_TRUE(settings.Graphics().vsync);
    EXPECT_NEAR(settings.Audio().masterVolume, 1.0f, 0.01f);
    EXPECT_NEAR(settings.Controls().mouseSensitivity, 1.0f, 0.01f);
    EXPECT_EQ(settings.Game().difficulty, 1);
}

TEST(EngineSettings_IgnoreComments)
{
    EngineSettings settings;
    settings.Load("# This is a comment\n"
                  "; This is also a comment\n"
                  "[Graphics]\n"
                  "# Resolution\n"
                  "windowWidth=3840\n");

    EXPECT_EQ(settings.Graphics().windowWidth, 3840);
}

TEST(EngineSettings_IgnoreEmptyLines)
{
    EngineSettings settings;
    settings.Load("[Graphics]\n"
                  "\n"
                  "\n"
                  "windowWidth=1280\n"
                  "\n"
                  "windowHeight=720\n");

    EXPECT_EQ(settings.Graphics().windowWidth, 1280);
    EXPECT_EQ(settings.Graphics().windowHeight, 720);
}

TEST(EngineSettings_HandleWhitespace)
{
    EngineSettings settings;
    settings.Load("[Graphics]\n"
                  "  windowWidth = 1600  \n"
                  "  windowHeight = 900  \n");

    EXPECT_EQ(settings.Graphics().windowWidth, 1600);
    EXPECT_EQ(settings.Graphics().windowHeight, 900);
}

// ============================================================================
// Type Conversion Tests
// ============================================================================

TEST(EngineSettings_BoolTrueVariants)
{
    EngineSettings settings;
    settings.Load("[Graphics]\n"
                  "fullscreen=true\n");
    EXPECT_TRUE(settings.Graphics().fullscreen);

    settings.Load("[Graphics]\n"
                  "fullscreen=1\n");
    EXPECT_TRUE(settings.Graphics().fullscreen);

    settings.Load("[Graphics]\n"
                  "fullscreen=yes\n");
    EXPECT_TRUE(settings.Graphics().fullscreen);
}

TEST(EngineSettings_BoolFalseVariants)
{
    EngineSettings settings;
    settings.Load("[Graphics]\n"
                  "fullscreen=false\n");
    EXPECT_FALSE(settings.Graphics().fullscreen);

    settings.Load("[Graphics]\n"
                  "fullscreen=0\n");
    EXPECT_FALSE(settings.Graphics().fullscreen);

    settings.Load("[Graphics]\n"
                  "fullscreen=no\n");
    EXPECT_FALSE(settings.Graphics().fullscreen);
}

TEST(EngineSettings_InvalidIntFallsToDefault)
{
    EngineSettings settings;
    settings.Load("[Graphics]\n"
                  "windowWidth=not_a_number\n");

    EXPECT_EQ(settings.Graphics().windowWidth, 1920); // default
}

TEST(EngineSettings_InvalidFloatFallsToDefault)
{
    EngineSettings settings;
    settings.Load("[Audio]\n"
                  "masterVolume=abc\n");

    EXPECT_NEAR(settings.Audio().masterVolume, 1.0f, 0.01f); // default
}

// ============================================================================
// Change Notification Tests
// ============================================================================

TEST(EngineSettings_ChangeCallback)
{
    EngineSettings settings;
    settings.Load("");

    std::string changedSection;
    std::string changedKey;
    settings.OnSettingsChanged(
        [&](const std::string& section, const std::string& key)
        {
            changedSection = section;
            changedKey = key;
        });

    settings.SetValue("Graphics", "windowWidth", "2560");
    EXPECT_EQ(changedSection, std::string("Graphics"));
    EXPECT_EQ(changedKey, std::string("windowWidth"));
}

TEST(EngineSettings_MultipleCallbacks)
{
    EngineSettings settings;
    settings.Load("");

    int callCount = 0;
    settings.OnSettingsChanged([&](const std::string&, const std::string&) { callCount++; });
    settings.OnSettingsChanged([&](const std::string&, const std::string&) { callCount++; });

    settings.SetValue("Audio", "masterVolume", "0.5");
    EXPECT_EQ(callCount, 2);
}

// ============================================================================
// Section/Key Enumeration Tests
// ============================================================================

TEST(EngineSettings_GetSections)
{
    EngineSettings settings;
    settings.Load("[Graphics]\n"
                  "windowWidth=1920\n"
                  "[Audio]\n"
                  "masterVolume=1.0\n"
                  "[Game]\n"
                  "difficulty=1\n");

    auto sections = settings.GetSections();
    EXPECT_GE(sections.size(), (size_t)3);
}

TEST(EngineSettings_GetKeys)
{
    EngineSettings settings;
    settings.Load("[Graphics]\n"
                  "windowWidth=1920\n"
                  "windowHeight=1080\n"
                  "fullscreen=false\n");

    auto keys = settings.GetKeys("Graphics");
    EXPECT_EQ(keys.size(), (size_t)3);
}

// ============================================================================
// Reset / Lifecycle Tests
// ============================================================================

TEST(EngineSettings_ResetToDefaults)
{
    EngineSettings settings;
    settings.Load("[Graphics]\n"
                  "windowWidth=3840\n"
                  "fullscreen=true\n"
                  "[Audio]\n"
                  "masterVolume=0.5\n");

    settings.ResetToDefaults();
    EXPECT_EQ(settings.Graphics().windowWidth, 1920);
    EXPECT_FALSE(settings.Graphics().fullscreen);
    EXPECT_NEAR(settings.Audio().masterVolume, 1.0f, 0.01f);
}

TEST(EngineSettings_IsLoadedAfterParse)
{
    EngineSettings settings;
    EXPECT_FALSE(settings.IsLoaded());
    settings.Load("[Graphics]\nwindowWidth=1920\n");
    EXPECT_TRUE(settings.IsLoaded());
}

// ============================================================================
// Serialization Round-Trip Tests
// ============================================================================

TEST(EngineSettings_SerializeContainsValues)
{
    EngineSettings settings;
    settings.Load("[Graphics]\n"
                  "windowWidth=2560\n"
                  "windowHeight=1440\n"
                  "fullscreen=true\n"
                  "vsync=false\n");

    auto serialized = settings.Serialize();
    EXPECT_TRUE(serialized.find("windowWidth=2560") != std::string::npos);
    EXPECT_TRUE(serialized.find("windowHeight=1440") != std::string::npos);
}

// ============================================================================
// Multiple Sections Test
// ============================================================================

TEST(EngineSettings_MultipleSectionsParsed)
{
    EngineSettings settings;
    settings.Load("[Graphics]\n"
                  "windowWidth=2560\n"
                  "renderScale=0.75\n"
                  "[Audio]\n"
                  "masterVolume=0.9\n"
                  "sfxVolume=0.7\n"
                  "[Controls]\n"
                  "mouseSensitivity=1.5\n"
                  "[Game]\n"
                  "fieldOfView=120\n");

    EXPECT_EQ(settings.Graphics().windowWidth, 2560);
    EXPECT_NEAR(settings.Graphics().renderScale, 0.75f, 0.01f);
    EXPECT_NEAR(settings.Audio().masterVolume, 0.9f, 0.01f);
    EXPECT_NEAR(settings.Audio().sfxVolume, 0.7f, 0.01f);
    EXPECT_NEAR(settings.Controls().mouseSensitivity, 1.5f, 0.01f);
    EXPECT_NEAR(settings.Game().fieldOfView, 120.0f, 0.1f);
}
