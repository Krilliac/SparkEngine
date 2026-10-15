/**
 * @file EditorPreferences.h
 * @brief Per-user editor UI preferences (palette, appearance, UI scale, density, accessibility
 *        toggles, last layout, recent scenes) and their editor-preferences.ini reader and writer.
 *
 * The file is untrusted input (users edit it; it roams with profiles), so the reader is bounded:
 * the file is read through ReadRegularFileBounded (a regular file of at most
 * kMaxEditorPreferencesBytes, decided on the opened handle), an optional UTF-8 byte-order mark is
 * skipped, a `version=1` line is required, only whitelisted `key=value` lines are read (unknown
 * keys and malformed lines are ignored), finite numbers are clamped to their ranges, strings carry
 * no control characters and are of bounded length, and at most kMaxEditorRecentScenes recent
 * scenes are kept. A file the reader rejects is never overwritten:
 * LoadEditorPreferencesForSession moves it aside first, or tells the caller not to save. The
 * parser is ImGui-free and lives in its own translation unit so the SEC-120 libFuzzer target
 * links the shipped code; EditorPreferencesPath() lives in EditorPreferencesPath.cpp because it
 * needs ProjectManager.
 *
 * Thread affinity: any thread; no shared state. Load/Save touch only the files they are given.
 * Ownership: inputs are borrowed; outputs are written in place only on success.
 */

#pragma once

#include "SparkUI/Enums.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace SparkEditor
{
    /// @brief Largest preferences file the reader accepts.
    inline constexpr std::size_t kMaxEditorPreferencesBytes = 64 * 1024;
    /// @brief Most recent scenes remembered (newest first).
    inline constexpr std::size_t kMaxEditorRecentScenes = 10;
    /// @brief Longest palette name stored.
    inline constexpr std::size_t kMaxEditorPaletteNameBytes = 64;
    /// @brief Longest path (layout or scene) stored.
    inline constexpr std::size_t kMaxEditorPreferencePathBytes = 1024;
    /// @brief Smallest non-automatic user UI scale.
    inline constexpr float kMinEditorUiScale = 0.75f;
    /// @brief Largest user UI scale.
    inline constexpr float kMaxEditorUiScale = 2.0f;
    /// @brief User UI scale step (12.5 %).
    inline constexpr float kEditorUiScaleStep = 0.125f;

    /// @brief Editor UI preferences. Defaults are what a first run uses.
    struct EditorPreferences
    {
        std::string palette = "Ember Studio";
        std::string appearance = "dark"; ///< "dark", "light", "hc" or "system".
        float uiScale = 0.0f;            ///< 0 = Auto (DPI only); else 0.75 .. 2.0 in 12.5 % steps.
        Spark::Ui::Density density = Spark::Ui::Density::Standard;
        bool alwaysShowFocus = false;
        bool reduceMotion = false;
        bool showStartPage = true;
        bool showStatusBar = true;
        std::string lastLayout;
        std::vector<std::string> recentScenes; ///< Newest first, at most kMaxEditorRecentScenes.

        friend bool operator==(const EditorPreferences&, const EditorPreferences&) = default;
    };

    /// @brief Parse preferences text.
    /// @param text File contents.
    /// @param out Receives the parsed preferences (starting from defaults) only on success.
    /// @param error Optional; receives the reason on failure.
    /// @return False when the text is larger than kMaxEditorPreferencesBytes or has no
    ///         `version=1` line; value-level problems only drop the offending line.
    bool ParseEditorPreferences(std::string_view text, EditorPreferences& out, std::string* error);

    /// @brief Render @p prefs in the format ParseEditorPreferences reads.
    /// @details Values that cannot be stored (control characters, over-long strings, surplus
    ///          recent scenes) are left out, so parsing the result always succeeds.
    std::string WriteEditorPreferences(const EditorPreferences& prefs);

    /// @brief Read and parse @p file. @p out is unchanged on failure.
    /// @details A missing path, a directory, FIFO or device, and a file larger than
    ///          kMaxEditorPreferencesBytes all fail without blocking or reading past the limit.
    bool LoadEditorPreferences(const std::filesystem::path& file, EditorPreferences& out, std::string* error);

    /// @brief What LoadEditorPreferencesForSession found at the preferences path.
    enum class EditorPreferencesLoadStatus : std::uint8_t
    {
        Loaded,     ///< The file was read and parsed.
        Missing,    ///< No file yet (first run); saving creates one.
        MovedAside, ///< The reader rejected the file; it was renamed to backupPath, so saving cannot destroy it.
        Unreadable  ///< The path could not be read, or a rejected file could not be moved aside; do not save.
    };

    /// @brief Outcome of LoadEditorPreferencesForSession.
    struct EditorPreferencesLoadResult
    {
        EditorPreferencesLoadStatus status = EditorPreferencesLoadStatus::Missing;
        std::filesystem::path backupPath; ///< Where a rejected file now lives (MovedAside only).
        std::string error;                ///< Why the file was rejected or unreadable; empty otherwise.

        /// @brief False when saving would overwrite a file the editor could not read.
        [[nodiscard]] bool CanSave() const noexcept { return status != EditorPreferencesLoadStatus::Unreadable; }
    };

    /// @brief Load @p file for an editor session without ever losing a file the reader rejects.
    /// @details @p out is reset to defaults, then filled from the file when it loads. A file the
    ///          parser rejects (a newer version, no version line, over 64 KiB) is renamed to the
    ///          first free name of `<file>.bak`, `<file>.bak2` .. `<file>.bak9`, so the next save
    ///          writes a fresh file beside it. A path that cannot be read (a directory, FIFO,
    ///          device or I/O failure), or a rejected file that cannot be renamed, reports
    ///          Unreadable: the caller must not save over it this session.
    EditorPreferencesLoadResult LoadEditorPreferencesForSession(const std::filesystem::path& file,
                                                                EditorPreferences& out);

    /// @brief Write @p prefs to @p file atomically: the text goes to `<file>.tmp`, which then
    ///        replaces @p file, so a crash never leaves a truncated preferences file.
    bool SaveEditorPreferences(const std::filesystem::path& file, const EditorPreferences& prefs, std::string* error);

    /// @brief Add @p scene to the front of @p prefs.recentScenes (removing an older copy) and
    ///        drop entries beyond kMaxEditorRecentScenes.
    void PushRecentScene(EditorPreferences& prefs, std::string_view scene);

    /// @brief Per-user preferences file: ProjectManager::GetEditorDataDirectory()/editor-preferences.ini.
    std::filesystem::path EditorPreferencesPath();
} // namespace SparkEditor
