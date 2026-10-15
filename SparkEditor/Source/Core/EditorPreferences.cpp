/**
 * @file EditorPreferences.cpp
 * @brief Bounded reader and writer for editor-preferences.ini.
 *
 * Numbers are read and written without the C locale (no strtod/printf), so a host that calls
 * setlocale cannot change the format. Files are read through the editor's bounded regular-file
 * reader (Utils/EditorFileRead.h), which decides on the opened handle and never blocks on a FIFO.
 */

#include "EditorPreferences.h"

#include "Utils/EditorFileRead.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace SparkEditor
{
    namespace
    {
        constexpr std::string_view kVersionKey = "version";
        constexpr std::string_view kSupportedVersion = "1";
        constexpr std::size_t kMaxNumberChars = 16;
        /// Written by editors such as Notepad ("UTF-8 with BOM") and PowerShell 5 Set-Content -Encoding UTF8.
        constexpr std::string_view kUtf8ByteOrderMark = "\xEF\xBB\xBF";
        /// Backup names tried for a rejected file: <file>.bak, then <file>.bak2 .. <file>.bak9.
        constexpr int kMaxPreferenceBackups = 9;

        void SetError(std::string* error, std::string_view message)
        {
            if (error != nullptr)
            {
                error->assign(message);
            }
        }

        bool IsTrimmed(char c) noexcept
        {
            return c == ' ' || c == '\t' || c == '\r';
        }

        std::string_view Trim(std::string_view text) noexcept
        {
            while (!text.empty() && IsTrimmed(text.front()))
            {
                text.remove_prefix(1);
            }
            while (!text.empty() && IsTrimmed(text.back()))
            {
                text.remove_suffix(1);
            }
            return text;
        }

        /// A string the format can carry: no control characters, no surrounding blanks, bounded.
        bool IsStorableText(std::string_view text, std::size_t maxBytes) noexcept
        {
            if (text.size() > maxBytes || Trim(text).size() != text.size())
            {
                return false;
            }
            return std::none_of(text.begin(), text.end(),
                                [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; });
        }

        std::optional<bool> ParseBool(std::string_view value) noexcept
        {
            if (value == "true" || value == "1")
            {
                return true;
            }
            if (value == "false" || value == "0")
            {
                return false;
            }
            return std::nullopt;
        }

        /// Locale-free decimal: [+-]digits[.digits]. Anything else (exponents, nan, inf) is rejected.
        std::optional<double> ParseDecimal(std::string_view value) noexcept
        {
            if (value.empty() || value.size() > kMaxNumberChars)
            {
                return std::nullopt;
            }
            std::size_t index = 0;
            bool negative = false;
            if (value[index] == '+' || value[index] == '-')
            {
                negative = value[index] == '-';
                ++index;
            }
            double result = 0.0;
            std::size_t digits = 0;
            while (index < value.size() && value[index] >= '0' && value[index] <= '9')
            {
                result = result * 10.0 + static_cast<double>(value[index] - '0');
                ++index;
                ++digits;
            }
            if (index < value.size() && value[index] == '.')
            {
                ++index;
                double scale = 0.1;
                while (index < value.size() && value[index] >= '0' && value[index] <= '9')
                {
                    result += static_cast<double>(value[index] - '0') * scale;
                    scale *= 0.1;
                    ++index;
                    ++digits;
                }
            }
            if (digits == 0 || index != value.size() || !std::isfinite(result))
            {
                return std::nullopt;
            }
            return negative ? -result : result;
        }

        /// 0 stays Auto; anything else is clamped to the user range and snapped to 12.5 % steps.
        float NormalizeUiScale(double value) noexcept
        {
            if (value == 0.0)
            {
                return 0.0f;
            }
            const double clamped =
                std::clamp(value, static_cast<double>(kMinEditorUiScale), static_cast<double>(kMaxEditorUiScale));
            const double steps = std::round(clamped / static_cast<double>(kEditorUiScaleStep));
            return static_cast<float>(steps * static_cast<double>(kEditorUiScaleStep));
        }

        bool IsAppearance(std::string_view value) noexcept
        {
            return value == "dark" || value == "light" || value == "hc" || value == "system";
        }

        void AppendLine(std::string& out, std::string_view key, std::string_view value)
        {
            out.append(key);
            out.push_back('=');
            out.append(value);
            out.push_back('\n');
        }

        /// Fixed three decimals without the C locale; the value is a 12.5 % step, so exact.
        std::string FormatScale(float value)
        {
            const auto thousandths = static_cast<std::int64_t>(std::llround(static_cast<double>(value) * 1000.0));
            std::string text = std::to_string(thousandths / 1000);
            text.push_back('.');
            const std::string fraction = std::to_string(thousandths % 1000);
            text.append(3 - fraction.size(), '0');
            text.append(fraction);
            return text;
        }

        void ApplyLine(std::string_view key, std::string_view value, EditorPreferences& prefs)
        {
            if (key == "palette")
            {
                if (!value.empty() && IsStorableText(value, kMaxEditorPaletteNameBytes))
                {
                    prefs.palette.assign(value);
                }
            }
            else if (key == "appearance")
            {
                if (IsAppearance(value))
                {
                    prefs.appearance.assign(value);
                }
            }
            else if (key == "uiScale")
            {
                if (const std::optional<double> number = ParseDecimal(value))
                {
                    prefs.uiScale = NormalizeUiScale(*number);
                }
            }
            else if (key == "density")
            {
                if (value == "standard")
                {
                    prefs.density = Spark::Ui::Density::Standard;
                }
                else if (value == "compact")
                {
                    prefs.density = Spark::Ui::Density::Compact;
                }
            }
            else if (key == "alwaysShowFocus" || key == "reduceMotion" || key == "showStartPage" ||
                     key == "showStatusBar")
            {
                const std::optional<bool> flag = ParseBool(value);
                if (!flag)
                {
                    return;
                }
                bool& field = key == "alwaysShowFocus" ? prefs.alwaysShowFocus
                              : key == "reduceMotion"  ? prefs.reduceMotion
                              : key == "showStartPage" ? prefs.showStartPage
                                                       : prefs.showStatusBar;
                field = *flag;
            }
            else if (key == "lastLayout")
            {
                if (IsStorableText(value, kMaxEditorPreferencePathBytes))
                {
                    prefs.lastLayout.assign(value);
                }
            }
            else if (key == "recentScene")
            {
                // File order is newest first; a repeated path keeps its newest position.
                if (!value.empty() && IsStorableText(value, kMaxEditorPreferencePathBytes) &&
                    prefs.recentScenes.size() < kMaxEditorRecentScenes &&
                    std::find(prefs.recentScenes.begin(), prefs.recentScenes.end(), value) == prefs.recentScenes.end())
                {
                    prefs.recentScenes.emplace_back(value);
                }
            }
        }
    } // namespace

    bool ParseEditorPreferences(std::string_view text, EditorPreferences& out, std::string* error)
    {
        if (text.size() > kMaxEditorPreferencesBytes)
        {
            SetError(error, "editor preferences exceed 64 KiB");
            return false;
        }

        if (text.starts_with(kUtf8ByteOrderMark))
        {
            text.remove_prefix(kUtf8ByteOrderMark.size());
        }

        EditorPreferences parsed;
        bool sawVersion = false;
        while (!text.empty())
        {
            const std::size_t newline = text.find('\n');
            const std::string_view line = Trim(text.substr(0, newline));
            text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
            if (line.empty() || line.front() == '#' || line.front() == ';')
            {
                continue;
            }
            const std::size_t equals = line.find('=');
            if (equals == std::string_view::npos)
            {
                continue;
            }
            const std::string_view key = Trim(line.substr(0, equals));
            const std::string_view value = Trim(line.substr(equals + 1));
            if (key == kVersionKey)
            {
                if (value != kSupportedVersion)
                {
                    SetError(error, "unsupported editor preferences version");
                    return false;
                }
                sawVersion = true;
                continue;
            }
            ApplyLine(key, value, parsed);
        }
        if (!sawVersion)
        {
            SetError(error, "editor preferences have no version=1 line");
            return false;
        }
        out = std::move(parsed);
        return true;
    }

    std::string WriteEditorPreferences(const EditorPreferences& prefs)
    {
        std::string out;
        out.reserve(512);
        AppendLine(out, kVersionKey, kSupportedVersion);
        if (!prefs.palette.empty() && IsStorableText(prefs.palette, kMaxEditorPaletteNameBytes))
        {
            AppendLine(out, "palette", prefs.palette);
        }
        if (IsAppearance(prefs.appearance))
        {
            AppendLine(out, "appearance", prefs.appearance);
        }
        const float scale = std::isfinite(prefs.uiScale) ? NormalizeUiScale(prefs.uiScale) : 0.0f;
        AppendLine(out, "uiScale", FormatScale(scale));
        AppendLine(out, "density", prefs.density == Spark::Ui::Density::Compact ? "compact" : "standard");
        AppendLine(out, "alwaysShowFocus", prefs.alwaysShowFocus ? "true" : "false");
        AppendLine(out, "reduceMotion", prefs.reduceMotion ? "true" : "false");
        AppendLine(out, "showStartPage", prefs.showStartPage ? "true" : "false");
        AppendLine(out, "showStatusBar", prefs.showStatusBar ? "true" : "false");
        if (IsStorableText(prefs.lastLayout, kMaxEditorPreferencePathBytes))
        {
            AppendLine(out, "lastLayout", prefs.lastLayout);
        }
        std::size_t written = 0;
        std::vector<std::string_view> seen;
        for (const std::string& scene : prefs.recentScenes)
        {
            if (written == kMaxEditorRecentScenes)
            {
                break;
            }
            if (scene.empty() || !IsStorableText(scene, kMaxEditorPreferencePathBytes) ||
                std::find(seen.begin(), seen.end(), scene) != seen.end())
            {
                continue;
            }
            AppendLine(out, "recentScene", scene);
            seen.emplace_back(scene);
            ++written;
        }
        return out;
    }

    bool LoadEditorPreferences(const std::filesystem::path& file, EditorPreferences& out, std::string* error)
    {
        std::string content;
        switch (ReadRegularFileBounded(file, kMaxEditorPreferencesBytes, content))
        {
        case BoundedReadStatus::Ok:
            return ParseEditorPreferences(content, out, error);
        case BoundedReadStatus::Missing:
            SetError(error, "the editor preferences file does not exist");
            return false;
        case BoundedReadStatus::NotRegularFile:
            SetError(error, "the editor preferences path is not a regular file");
            return false;
        case BoundedReadStatus::TooLarge:
            SetError(error, "editor preferences exceed 64 KiB");
            return false;
        case BoundedReadStatus::Failed:
            break;
        }
        SetError(error, "cannot read editor preferences");
        return false;
    }

    EditorPreferencesLoadResult LoadEditorPreferencesForSession(const std::filesystem::path& file,
                                                                EditorPreferences& out)
    {
        out = EditorPreferences{};
        EditorPreferencesLoadResult result;
        std::string content;
        switch (ReadRegularFileBounded(file, kMaxEditorPreferencesBytes, content))
        {
        case BoundedReadStatus::Missing:
            result.status = EditorPreferencesLoadStatus::Missing;
            return result;
        case BoundedReadStatus::NotRegularFile:
            result.status = EditorPreferencesLoadStatus::Unreadable;
            result.error = "the editor preferences path is not a regular file";
            return result;
        case BoundedReadStatus::Failed:
            result.status = EditorPreferencesLoadStatus::Unreadable;
            result.error = "cannot read editor preferences";
            return result;
        case BoundedReadStatus::TooLarge:
            result.error = "editor preferences exceed 64 KiB";
            break;
        case BoundedReadStatus::Ok:
            if (ParseEditorPreferences(content, out, &result.error))
            {
                result.status = EditorPreferencesLoadStatus::Loaded;
                return result;
            }
            break;
        }

        // The reader rejected a regular file the user may still want (a newer editor's version, a
        // hand edit). Move it out of the way before anything can save over it.
        for (int attempt = 1; attempt <= kMaxPreferenceBackups; ++attempt)
        {
            std::filesystem::path backup = file;
            backup += attempt == 1 ? std::string(".bak") : ".bak" + std::to_string(attempt);
            std::error_code ec;
            if (std::filesystem::exists(backup, ec) || ec)
            {
                continue;
            }
            std::filesystem::rename(file, backup, ec);
            if (ec)
            {
                break;
            }
            result.status = EditorPreferencesLoadStatus::MovedAside;
            result.backupPath = std::move(backup);
            return result;
        }
        result.status = EditorPreferencesLoadStatus::Unreadable;
        result.error += "; the file could not be moved aside, so it is left untouched";
        return result;
    }

    bool SaveEditorPreferences(const std::filesystem::path& file, const EditorPreferences& prefs, std::string* error)
    {
        std::error_code ec;
        if (file.has_parent_path())
        {
            std::filesystem::create_directories(file.parent_path(), ec);
            if (ec)
            {
                SetError(error, "cannot create the editor preferences directory");
                return false;
            }
        }
        std::filesystem::path temporary = file;
        temporary += ".tmp";
        const std::string text = WriteEditorPreferences(prefs);
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream)
            {
                SetError(error, "cannot create the temporary editor preferences file");
                return false;
            }
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            stream.flush();
            if (!stream)
            {
                stream.close();
                std::filesystem::remove(temporary, ec);
                SetError(error, "cannot write the temporary editor preferences file");
                return false;
            }
        }
        std::filesystem::rename(temporary, file, ec);
        if (ec)
        {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            SetError(error, "cannot replace the editor preferences file");
            return false;
        }
        return true;
    }

    void PushRecentScene(EditorPreferences& prefs, std::string_view scene)
    {
        if (scene.empty() || !IsStorableText(scene, kMaxEditorPreferencePathBytes))
        {
            return;
        }
        prefs.recentScenes.erase(std::remove(prefs.recentScenes.begin(), prefs.recentScenes.end(), scene),
                                 prefs.recentScenes.end());
        prefs.recentScenes.insert(prefs.recentScenes.begin(), std::string(scene));
        if (prefs.recentScenes.size() > kMaxEditorRecentScenes)
        {
            prefs.recentScenes.resize(kMaxEditorRecentScenes);
        }
    }
} // namespace SparkEditor
