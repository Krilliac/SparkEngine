/**
 * @file TestEditorPreferences.cpp
 * @brief editor-preferences.ini reader and writer: round trip, whitelisting, bounds, number
 *        validation, recent-scene cap and atomic save.
 *
 * SparkEditor/Source/Core/EditorPreferences.cpp is ImGui-free and compiled into SparkTests
 * directly (the EditorThemeDocument.cpp pattern); the SEC-120 fuzz target links the same file.
 */

#include "TestFramework.h"

#include "Core/EditorPreferences.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>

using SparkEditor::EditorPreferences;

namespace
{
    namespace fs = std::filesystem;

    class ScopedTempDirectory
    {
      public:
        ScopedTempDirectory()
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            m_path = fs::temp_directory_path() / ("spark-editor-prefs-" + std::to_string(stamp));
            fs::create_directories(m_path);
        }

        ~ScopedTempDirectory()
        {
            std::error_code ec;
            fs::remove_all(m_path, ec);
        }

        ScopedTempDirectory(const ScopedTempDirectory&) = delete;
        ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;

        [[nodiscard]] const fs::path& Path() const noexcept { return m_path; }

      private:
        fs::path m_path;
    };

    EditorPreferences Customized()
    {
        EditorPreferences prefs;
        prefs.palette = "Cobalt Forge";
        prefs.appearance = "hc";
        prefs.uiScale = 1.375f;
        prefs.density = Spark::Ui::Density::Compact;
        prefs.alwaysShowFocus = true;
        prefs.reduceMotion = true;
        prefs.showStartPage = false;
        prefs.showStatusBar = false;
        prefs.lastLayout = "Layouts/Animation Studio.ini";
        prefs.recentScenes = {"Scenes/level2.scene", "Scenes/level1.scene"};
        return prefs;
    }

    std::string ReadFile(const fs::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }
} // namespace

TEST(EditorPreferences_RoundTripPreservesEveryField)
{
    const EditorPreferences original = Customized();
    const std::string text = SparkEditor::WriteEditorPreferences(original);
    EXPECT_STR_CONTAINS(text, "version=1\n");
    EXPECT_STR_CONTAINS(text, "uiScale=1.375\n");

    EditorPreferences parsed;
    std::string error;
    ASSERT_TRUE(SparkEditor::ParseEditorPreferences(text, parsed, &error));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(parsed == original);
    EXPECT_EQ(SparkEditor::WriteEditorPreferences(parsed), text);

    // Defaults survive a round trip too.
    EditorPreferences defaults;
    ASSERT_TRUE(SparkEditor::ParseEditorPreferences(SparkEditor::WriteEditorPreferences(EditorPreferences{}), defaults,
                                                    nullptr));
    EXPECT_TRUE(defaults == EditorPreferences{});
}

TEST(EditorPreferences_UnknownKeysAndMalformedLinesAreIgnored)
{
    const std::string text = "# comment\r\n"
                             "version=1\r\n"
                             "telemetryEndpoint=https://example.invalid\r\n"
                             "this line has no separator\r\n"
                             "appearance=sepia\r\n"
                             "density=roomy\r\n"
                             "showStatusBar=maybe\r\n"
                             "  palette = Graphite Signal  \r\n"
                             "reduceMotion=1\r\n";
    EditorPreferences parsed;
    ASSERT_TRUE(SparkEditor::ParseEditorPreferences(text, parsed, nullptr));
    EXPECT_EQ(parsed.palette, std::string("Graphite Signal"));
    EXPECT_TRUE(parsed.reduceMotion);
    // Invalid values keep the defaults.
    EXPECT_EQ(parsed.appearance, std::string("dark"));
    EXPECT_TRUE(parsed.density == Spark::Ui::Density::Standard);
    EXPECT_TRUE(parsed.showStatusBar);
}

TEST(EditorPreferences_RequiresSupportedVersion)
{
    EditorPreferences parsed = Customized();
    std::string error;
    EXPECT_FALSE(SparkEditor::ParseEditorPreferences("palette=Cobalt Forge\n", parsed, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(SparkEditor::ParseEditorPreferences("version=2\npalette=Cobalt Forge\n", parsed, &error));
    // A rejected document leaves the output untouched.
    EXPECT_TRUE(parsed == Customized());
}

TEST(EditorPreferences_OversizeRejected)
{
    std::string text = "version=1\npalette=Cobalt Forge\n";
    text.append(SparkEditor::kMaxEditorPreferencesBytes, '#');
    EditorPreferences parsed;
    std::string error;
    EXPECT_FALSE(SparkEditor::ParseEditorPreferences(text, parsed, &error));
    EXPECT_STR_CONTAINS(error, "64 KiB");
    EXPECT_TRUE(parsed == EditorPreferences{});

    // A file that size is rejected by Load as well, without reading it all.
    ScopedTempDirectory directory;
    const fs::path file = directory.Path() / "editor-preferences.ini";
    {
        std::ofstream stream(file, std::ios::binary);
        stream << text;
    }
    EXPECT_FALSE(SparkEditor::LoadEditorPreferences(file, parsed, &error));
    EXPECT_TRUE(parsed == EditorPreferences{});

    // Exactly at the limit is accepted.
    std::string limit = "version=1\n";
    limit.append(SparkEditor::kMaxEditorPreferencesBytes - limit.size(), '\n');
    EXPECT_TRUE(SparkEditor::ParseEditorPreferences(limit, parsed, nullptr));
}

TEST(EditorPreferences_NonFiniteAndMalformedScaleRejected)
{
    for (const char* value : {"nan", "NaN", "inf", "-inf", "1e999", "1e3", "0x1p3", "1.5.2", "", "+", "."})
    {
        EditorPreferences parsed;
        ASSERT_TRUE(
            SparkEditor::ParseEditorPreferences(std::string("version=1\nuiScale=") + value + "\n", parsed, nullptr));
        EXPECT_EQ(parsed.uiScale, 0.0f);
    }

    // Finite values are clamped to 0.75 .. 2.0 and snapped to 12.5 % steps; 0 stays Auto.
    const auto scaleOf = [](const char* value)
    {
        EditorPreferences parsed;
        SparkEditor::ParseEditorPreferences(std::string("version=1\nuiScale=") + value + "\n", parsed, nullptr);
        return parsed.uiScale;
    };
    EXPECT_EQ(scaleOf("0"), 0.0f);
    EXPECT_EQ(scaleOf("1.3"), 1.25f);
    EXPECT_EQ(scaleOf("9"), 2.0f);
    EXPECT_EQ(scaleOf("0.1"), 0.75f);
    EXPECT_EQ(scaleOf("-4"), 0.75f);
    EXPECT_EQ(scaleOf("1.125"), 1.125f);

    // A non-finite value set in code is never written.
    EditorPreferences prefs;
    prefs.uiScale = std::numeric_limits<float>::infinity();
    EXPECT_STR_CONTAINS(SparkEditor::WriteEditorPreferences(prefs), "uiScale=0.000\n");
}

TEST(EditorPreferences_RecentScenesCappedAtTen)
{
    std::string text = "version=1\n";
    for (int i = 0; i < 25; ++i)
    {
        text += "recentScene=Scenes/scene" + std::to_string(i) + ".scene\n";
    }
    text += "recentScene=Scenes/scene0.scene\n";
    EditorPreferences parsed;
    ASSERT_TRUE(SparkEditor::ParseEditorPreferences(text, parsed, nullptr));
    ASSERT_EQ(parsed.recentScenes.size(), SparkEditor::kMaxEditorRecentScenes);
    EXPECT_EQ(parsed.recentScenes.front(), std::string("Scenes/scene0.scene"));
    EXPECT_EQ(parsed.recentScenes.back(), std::string("Scenes/scene9.scene"));

    // PushRecentScene moves a reopened scene to the front and keeps the cap.
    SparkEditor::PushRecentScene(parsed, "Scenes/scene5.scene");
    EXPECT_EQ(parsed.recentScenes.front(), std::string("Scenes/scene5.scene"));
    EXPECT_EQ(parsed.recentScenes.size(), SparkEditor::kMaxEditorRecentScenes);
    SparkEditor::PushRecentScene(parsed, "Scenes/new.scene");
    EXPECT_EQ(parsed.recentScenes.front(), std::string("Scenes/new.scene"));
    EXPECT_EQ(parsed.recentScenes.size(), SparkEditor::kMaxEditorRecentScenes);

    // The writer never emits more than the cap, even for a list built in code.
    EditorPreferences many;
    for (int i = 0; i < 12; ++i)
    {
        many.recentScenes.push_back("Scenes/extra" + std::to_string(i) + ".scene");
    }
    EditorPreferences reread;
    ASSERT_TRUE(SparkEditor::ParseEditorPreferences(SparkEditor::WriteEditorPreferences(many), reread, nullptr));
    EXPECT_EQ(reread.recentScenes.size(), SparkEditor::kMaxEditorRecentScenes);
}

TEST(EditorPreferences_StringsWithControlCharactersAreNotStored)
{
    EditorPreferences prefs;
    prefs.palette = "Ember\nStudio";
    prefs.lastLayout = std::string("Layouts/a\0b.ini", 15);
    prefs.recentScenes = {"Scenes/ok.scene", "Scenes/bad\r.scene"};
    const std::string text = SparkEditor::WriteEditorPreferences(prefs);
    EditorPreferences parsed;
    ASSERT_TRUE(SparkEditor::ParseEditorPreferences(text, parsed, nullptr));
    EXPECT_EQ(parsed.palette, std::string("Ember Studio"));
    EXPECT_TRUE(parsed.lastLayout.empty());
    ASSERT_EQ(parsed.recentScenes.size(), std::size_t{1});
    EXPECT_EQ(parsed.recentScenes.front(), std::string("Scenes/ok.scene"));
}

TEST(EditorPreferences_AtomicSaveReplacesFileAndLeavesNoTemporary)
{
    ScopedTempDirectory directory;
    const fs::path file = directory.Path() / "nested" / "editor-preferences.ini";
    fs::path temporary = file;
    temporary += ".tmp";

    std::string error;
    ASSERT_TRUE(SparkEditor::SaveEditorPreferences(file, EditorPreferences{}, &error));
    ASSERT_TRUE(SparkEditor::SaveEditorPreferences(file, Customized(), &error));
    EXPECT_FALSE(fs::exists(temporary));
    EXPECT_EQ(ReadFile(file), SparkEditor::WriteEditorPreferences(Customized()));

    EditorPreferences loaded;
    ASSERT_TRUE(SparkEditor::LoadEditorPreferences(file, loaded, &error));
    EXPECT_TRUE(loaded == Customized());

    // When the temporary cannot be created (a directory squats on its name) the save fails and
    // the previous file is untouched.
    fs::create_directories(temporary);
    EditorPreferences other;
    other.palette = "Graphite Signal";
    EXPECT_FALSE(SparkEditor::SaveEditorPreferences(file, other, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(ReadFile(file), SparkEditor::WriteEditorPreferences(Customized()));
}

TEST(EditorPreferences_LoadMissingFileFails)
{
    ScopedTempDirectory directory;
    EditorPreferences prefs = Customized();
    std::string error;
    EXPECT_FALSE(SparkEditor::LoadEditorPreferences(directory.Path() / "absent.ini", prefs, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(prefs == Customized());
}

TEST(EditorPreferences_PathIsInsideEditorDataDirectory)
{
    const fs::path path = SparkEditor::EditorPreferencesPath();
    EXPECT_EQ(path.filename().string(), std::string("editor-preferences.ini"));
    EXPECT_TRUE(path.has_parent_path());
}
