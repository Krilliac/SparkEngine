/**
 * @file EditorThemeDocument.cpp
 * @brief Theme file import/export format (moved out of EditorTheme.cpp)
 */

#include "EditorThemeDocument.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string_view>
#include <utility>

namespace SparkEditor
{

    namespace
    {
        /// Every colour the format carries, in the order ExportTheme writes them.
        constexpr std::array<std::pair<std::string_view, ThemeColor EditorThemeData::*>, 25> kThemeColors = {{
            {"background", &EditorThemeData::background},
            {"backgroundDark", &EditorThemeData::backgroundDark},
            {"backgroundLight", &EditorThemeData::backgroundLight},
            {"backgroundAccent", &EditorThemeData::backgroundAccent},
            {"backgroundHeader", &EditorThemeData::backgroundHeader},
            {"backgroundActive", &EditorThemeData::backgroundActive},
            {"backgroundHover", &EditorThemeData::backgroundHover},
            {"backgroundSelected", &EditorThemeData::backgroundSelected},
            {"text", &EditorThemeData::text},
            {"textDisabled", &EditorThemeData::textDisabled},
            {"textSecondary", &EditorThemeData::textSecondary},
            {"textAccent", &EditorThemeData::textAccent},
            {"textWarning", &EditorThemeData::textWarning},
            {"textError", &EditorThemeData::textError},
            {"textSuccess", &EditorThemeData::textSuccess},
            {"button", &EditorThemeData::button},
            {"buttonHovered", &EditorThemeData::buttonHovered},
            {"buttonActive", &EditorThemeData::buttonActive},
            {"frame", &EditorThemeData::frame},
            {"frameHovered", &EditorThemeData::frameHovered},
            {"frameActive", &EditorThemeData::frameActive},
            {"border", &EditorThemeData::border},
            {"borderLight", &EditorThemeData::borderLight},
            {"borderAccent", &EditorThemeData::borderAccent},
            {"borderSeparator", &EditorThemeData::borderSeparator},
        }};

        std::string ExtractString(const std::string& content, const std::string& key)
        {
            const std::string search = "\"" + key + "\": \"";
            auto pos = content.find(search);
            if (pos == std::string::npos)
            {
                return "";
            }
            pos += search.length();
            const auto end = content.find('\"', pos);
            if (end == std::string::npos)
            {
                return "";
            }
            return content.substr(pos, end - pos);
        }

        /// sscanf's %f accepts "nan", "inf" and overflows to infinity; a colour with such a
        /// component used to reach ImGui, whose float-to-byte colour conversion is undefined
        /// for NaN. Such a colour now reads as the default, like a malformed one.
        ThemeColor ExtractColor(const std::string& content, std::string_view key)
        {
            const std::string search = "\"" + std::string(key) + "\": [";
            auto pos = content.find(search);
            if (pos == std::string::npos)
            {
                return {};
            }
            pos += search.length();
            const auto end = content.find(']', pos);
            if (end == std::string::npos)
            {
                return {};
            }
            const std::string vals = content.substr(pos, end - pos);
            float r = 0;
            float g = 0;
            float b = 0;
            float a = 1;
            // r, g and b are required; a keeps its default when the entry omits alpha.
            if (std::sscanf(vals.c_str(), "%f, %f, %f, %f", &r, &g, &b, &a) < 3)
            {
                return {};
            }
            if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) || !std::isfinite(a))
            {
                return {};
            }
            return {r, g, b, a};
        }
    } // namespace

    bool ParseThemeDocument(const std::string& content, EditorThemeData& outTheme)
    {
        outTheme.name = ExtractString(content, "name");
        outTheme.description = ExtractString(content, "description");
        outTheme.author = ExtractString(content, "author");
        for (const auto& [key, member] : kThemeColors)
        {
            outTheme.*member = ExtractColor(content, key);
        }
        return !outTheme.name.empty();
    }

    std::string WriteThemeDocument(const EditorThemeData& theme)
    {
        std::ostringstream file;
        file << "{\n";
        file << "  \"name\": \"" << theme.name << "\",\n";
        file << "  \"description\": \"" << theme.description << "\",\n";
        file << "  \"author\": \"" << theme.author << "\",\n";
        file << "  \"colors\": {\n";
        for (size_t index = 0; index < kThemeColors.size(); ++index)
        {
            const auto& [key, member] = kThemeColors[index];
            const ThemeColor& c = theme.*member;
            file << "    \"" << key << "\": [" << c.r << ", " << c.g << ", " << c.b << ", " << c.a << "]";
            file << (index + 1 < kThemeColors.size() ? ",\n" : "\n");
        }
        file << "  }\n}\n";
        return file.str();
    }

} // namespace SparkEditor
