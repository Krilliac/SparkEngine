/**
 * @file FuzzEditorThemeImportProduction.cpp
 * @brief libc++-compiled production adapter for the editor theme import libFuzzer harness.
 *
 * ThemeCustomizer::ImportTheme reads a theme file through one bounded handle and hands it to
 * SparkEditor::ParseThemeDocument; EditorTheme::ApplyTheme then converts every colour to
 * ImGui's packed bytes, a float-to-integer conversion that is undefined for NaN. The adapter
 * parses the fuzz bytes into a theme whose other fields hold sentinels. A violated contract
 * aborts so libFuzzer records a crash:
 *  - the result is true exactly when the theme has a name, and no extracted string holds a
 *    quote (the reader ends a string at the first one);
 *  - every imported colour component is finite;
 *  - fields the format does not carry keep their values;
 *  - parsing the same bytes twice gives the same theme;
 *  - the theme written back by WriteThemeDocument (what ExportTheme writes) parses to the
 *    same strings and verdict, and writing that again gives the same document.
 */

#include "FuzzEditorThemeImportProduction.h"

#include "Core/EditorThemeDocument.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    using SparkEditor::EditorThemeData;
    using SparkEditor::ThemeColor;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzEditorThemeImport: ParseThemeDocument violated: %s\n", what);
        std::abort();
    }

    // The colours the export format carries.
    constexpr std::array<ThemeColor EditorThemeData::*, 25> kExported = {&EditorThemeData::background,
                                                                         &EditorThemeData::backgroundDark,
                                                                         &EditorThemeData::backgroundLight,
                                                                         &EditorThemeData::backgroundAccent,
                                                                         &EditorThemeData::backgroundHeader,
                                                                         &EditorThemeData::backgroundActive,
                                                                         &EditorThemeData::backgroundHover,
                                                                         &EditorThemeData::backgroundSelected,
                                                                         &EditorThemeData::text,
                                                                         &EditorThemeData::textDisabled,
                                                                         &EditorThemeData::textSecondary,
                                                                         &EditorThemeData::textAccent,
                                                                         &EditorThemeData::textWarning,
                                                                         &EditorThemeData::textError,
                                                                         &EditorThemeData::textSuccess,
                                                                         &EditorThemeData::button,
                                                                         &EditorThemeData::buttonHovered,
                                                                         &EditorThemeData::buttonActive,
                                                                         &EditorThemeData::frame,
                                                                         &EditorThemeData::frameHovered,
                                                                         &EditorThemeData::frameActive,
                                                                         &EditorThemeData::border,
                                                                         &EditorThemeData::borderLight,
                                                                         &EditorThemeData::borderAccent,
                                                                         &EditorThemeData::borderSeparator};

    EditorThemeData SentinelTheme()
    {
        EditorThemeData theme;
        theme.name = "sentinel";
        theme.buttonDisabled = ThemeColor(0.25f, 0.5f, 0.75f, 0.125f);
        theme.titleBar = ThemeColor(0.1f, 0.2f, 0.3f, 0.4f);
        theme.graph5 = ThemeColor(0.9f, 0.8f, 0.7f, 0.6f);
        theme.frameRounding = 7.0f;
        theme.fontFamily = "Sentinel Sans";
        return theme;
    }

    bool SameColor(const ThemeColor& a, const ThemeColor& b)
    {
        return std::memcmp(&a, &b, sizeof(ThemeColor)) == 0;
    }

    bool SameTheme(const EditorThemeData& a, const EditorThemeData& b)
    {
        if (a.name != b.name || a.description != b.description || a.author != b.author)
        {
            return false;
        }
        for (const auto member : kExported)
        {
            if (!SameColor(a.*member, b.*member))
            {
                return false;
            }
        }
        return true;
    }

    bool UntouchedFields(const EditorThemeData& theme)
    {
        const EditorThemeData sentinel = SentinelTheme();
        return SameColor(theme.buttonDisabled, sentinel.buttonDisabled) &&
               SameColor(theme.titleBar, sentinel.titleBar) && SameColor(theme.graph5, sentinel.graph5) &&
               std::memcmp(&theme.frameRounding, &sentinel.frameRounding, sizeof(float)) == 0 &&
               theme.fontFamily == sentinel.fontFamily;
    }
} // namespace

extern "C" int SparkFuzzParseEditorTheme(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    const std::string content = size == 0 ? std::string() : std::string(reinterpret_cast<const char*>(data), size);

    EditorThemeData theme = SentinelTheme();
    const bool accepted = SparkEditor::ParseThemeDocument(content, theme);
    if (accepted != !theme.name.empty())
    {
        InvariantFailure("the result disagrees with whether the theme has a name");
    }
    for (const std::string* text : {&theme.name, &theme.description, &theme.author})
    {
        if (text->find('"') != std::string::npos)
        {
            InvariantFailure("an extracted string holds a quote");
        }
    }
    for (const auto member : kExported)
    {
        const ThemeColor& color = theme.*member;
        if (!std::isfinite(color.r) || !std::isfinite(color.g) || !std::isfinite(color.b) || !std::isfinite(color.a))
        {
            InvariantFailure("an imported colour component is not finite");
        }
    }
    if (!UntouchedFields(theme))
    {
        InvariantFailure("a field the format does not carry changed");
    }

    EditorThemeData again = SentinelTheme();
    if (SparkEditor::ParseThemeDocument(content, again) != accepted || !SameTheme(again, theme))
    {
        InvariantFailure("parsing the same bytes twice gave different themes");
    }

    const std::string written = SparkEditor::WriteThemeDocument(theme);
    EditorThemeData reread = SentinelTheme();
    if (SparkEditor::ParseThemeDocument(written, reread) != accepted)
    {
        InvariantFailure("the written theme parses to a different verdict");
    }
    if (reread.name != theme.name || reread.description != theme.description || reread.author != theme.author)
    {
        InvariantFailure("write -> parse changed a theme string");
    }
    if (SparkEditor::WriteThemeDocument(reread) != written)
    {
        InvariantFailure("write -> parse -> write changed the document");
    }
    return 0;
}
