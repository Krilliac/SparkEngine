/**
 * @file FuzzEditorThemeImportProduction.cpp
 * @brief libc++-compiled production adapter for the editor theme import libFuzzer harness.
 *
 * ThemeCustomizer::ImportTheme reads a theme file through one bounded handle and hands it to
 * SparkEditor::ParseThemeDocument; EditorTheme::ApplyTheme then converts every colour to
 * ImGui's packed bytes, a float-to-integer conversion that is undefined for NaN. The adapter
 * parses the fuzz bytes over two different populated base themes. A violated contract aborts
 * so libFuzzer records a crash:
 *  - the result is true exactly when the theme has a name, and no extracted string holds a
 *    quote (the reader ends a string at the first one);
 *  - every imported colour component and metric is finite and every metric is inside its range;
 *  - a field the document does not define keeps the base value (checked by comparing the two
 *    parses: a field that differs between them must equal its own base);
 *  - parsing the same bytes twice gives the same theme, and the derivation path for a base
 *    without Ember Signal roles yields finite colours;
 *  - the theme written back by WriteThemeDocument (what ExportTheme writes) parses to the
 *    same strings and verdict, and writing that again gives the same document.
 */

#include "FuzzEditorThemeImportProduction.h"

#include "Core/EditorThemeDocument.h"

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
    using SparkEditor::ThemeMetricField;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzEditorThemeImport: ParseThemeDocument violated: %s\n", what);
        std::abort();
    }

    /// A populated base: every colour distinct from the default, so no role is derived.
    EditorThemeData SentinelTheme(float seed)
    {
        EditorThemeData theme;
        theme.name = "sentinel";
        float next = seed;
        for (const auto& field : SparkEditor::kThemeColourFields)
        {
            theme.*field.member = ThemeColor(next, next / 2.0f, next / 3.0f, 0.5f);
            next += 0.01f;
        }
        for (ThemeColor& fill : theme.statusFills)
        {
            fill = ThemeColor(next, 0.5f, 0.25f, 1.0f);
            next += 0.01f;
        }
        const bool first = seed < 0.5f;
        theme.frameRounding = seed * 20.0f;
        theme.fontSize = 10.0f + seed * 10.0f;
        theme.focusRingWidth = 1.0f + seed;
        theme.enableShadows = first;
        theme.fontFamily = first ? "Sentinel Sans" : "Other Sans";
        theme.appearance = first ? Spark::Ui::Appearance::HighContrast : Spark::Ui::Appearance::Light;
        theme.density = first ? Spark::Ui::Density::Compact : Spark::Ui::Density::Standard;
        return theme;
    }

    bool SameColor(const ThemeColor& a, const ThemeColor& b)
    {
        return std::memcmp(&a, &b, sizeof(ThemeColor)) == 0;
    }

    bool FiniteColor(const ThemeColor& c)
    {
        return std::isfinite(c.r) && std::isfinite(c.g) && std::isfinite(c.b) && std::isfinite(c.a);
    }

    bool SameTheme(const EditorThemeData& a, const EditorThemeData& b)
    {
        if (a.name != b.name || a.description != b.description || a.author != b.author)
        {
            return false;
        }
        for (const auto& field : SparkEditor::kThemeColourFields)
        {
            if (!SameColor(a.*field.member, b.*field.member))
            {
                return false;
            }
        }
        for (std::size_t i = 0; i < a.statusFills.size(); ++i)
        {
            if (!SameColor(a.statusFills[i], b.statusFills[i]))
            {
                return false;
            }
        }
        return a.fontFamily == b.fontFamily && a.appearance == b.appearance && a.density == b.density;
    }

    void CheckFinite(const EditorThemeData& theme)
    {
        for (const auto& field : SparkEditor::kThemeColourFields)
        {
            if (!FiniteColor(theme.*field.member))
            {
                InvariantFailure("an imported colour component is not finite");
            }
        }
        for (const ThemeColor& fill : theme.statusFills)
        {
            if (!FiniteColor(fill))
            {
                InvariantFailure("an imported status fill component is not finite");
            }
        }
    }

    void CheckMetricRange(const EditorThemeData& theme, const ThemeMetricField& field)
    {
        const float value = theme.*field.member;
        if (!(value >= field.minimum && value <= field.maximum))
        {
            InvariantFailure("a metric is outside its range");
        }
    }

    /// True when the two parses agree on a field, or each still holds its own base value there
    /// (which is what a field the document does not define looks like).
    template <typename Value>
    bool AgreesOrKeptBase(const Value& outA, const Value& outB, const Value& baseA, const Value& baseB)
    {
        return outA == outB || (outA == baseA && outB == baseB);
    }

    bool ColourAgreesOrKeptBase(const ThemeColor& outA, const ThemeColor& outB, const ThemeColor& baseA,
                                const ThemeColor& baseB)
    {
        return SameColor(outA, outB) || (SameColor(outA, baseA) && SameColor(outB, baseB));
    }

    void CheckBasePreserved(const EditorThemeData& baseA, const EditorThemeData& outA, const EditorThemeData& baseB,
                            const EditorThemeData& outB)
    {
        for (const auto& field : SparkEditor::kThemeColourFields)
        {
            if (!ColourAgreesOrKeptBase(outA.*field.member, outB.*field.member, baseA.*field.member,
                                        baseB.*field.member))
            {
                InvariantFailure("a colour the document does not define lost its base value");
            }
        }
        for (std::size_t i = 0; i < outA.statusFills.size(); ++i)
        {
            if (!ColourAgreesOrKeptBase(outA.statusFills[i], outB.statusFills[i], baseA.statusFills[i],
                                        baseB.statusFills[i]))
            {
                InvariantFailure("a status fill the document does not define lost its base value");
            }
        }
        const auto checkMetrics = [&](const auto& fields)
        {
            for (const ThemeMetricField& field : fields)
            {
                CheckMetricRange(outA, field);
                CheckMetricRange(outB, field);
                if (!AgreesOrKeptBase(outA.*field.member, outB.*field.member, baseA.*field.member, baseB.*field.member))
                {
                    InvariantFailure("a metric the document does not define lost its base value");
                }
            }
        };
        checkMetrics(SparkEditor::kThemeMetricFields);
        checkMetrics(SparkEditor::kThemeTypographyFields);
        for (const auto& field : SparkEditor::kThemeFlagFields)
        {
            if (!AgreesOrKeptBase(outA.*field.member, outB.*field.member, baseA.*field.member, baseB.*field.member))
            {
                InvariantFailure("a flag the document does not define lost its base value");
            }
        }
        if (!AgreesOrKeptBase(outA.fontFamily, outB.fontFamily, baseA.fontFamily, baseB.fontFamily) ||
            !AgreesOrKeptBase(outA.appearance, outB.appearance, baseA.appearance, baseB.appearance) ||
            !AgreesOrKeptBase(outA.density, outB.density, baseA.density, baseB.density))
        {
            InvariantFailure("the font family, appearance or density lost its base value");
        }
    }
} // namespace

extern "C" int SparkFuzzParseEditorTheme(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    const std::string content = size == 0 ? std::string() : std::string(reinterpret_cast<const char*>(data), size);

    const EditorThemeData baseA = SentinelTheme(0.25f);
    const EditorThemeData baseB = SentinelTheme(0.75f);
    EditorThemeData theme;
    std::string error;
    const bool accepted = SparkEditor::ParseThemeDocument(content, baseA, theme, &error);
    if (accepted != !theme.name.empty() || accepted != error.empty())
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
    CheckFinite(theme);

    EditorThemeData otherBase;
    if (SparkEditor::ParseThemeDocument(content, baseB, otherBase) != accepted)
    {
        InvariantFailure("the verdict depends on the base theme");
    }
    CheckBasePreserved(baseA, theme, baseB, otherBase);

    EditorThemeData again;
    if (SparkEditor::ParseThemeDocument(content, baseA, again) != accepted || !SameTheme(again, theme))
    {
        InvariantFailure("parsing the same bytes twice gave different themes");
    }

    // A base without Ember Signal roles takes the derivation path; its colours must stay finite.
    EditorThemeData derived;
    if (SparkEditor::ParseThemeDocument(content, derived) != accepted)
    {
        InvariantFailure("the verdict depends on the base theme");
    }
    CheckFinite(derived);

    const std::string written = SparkEditor::WriteThemeDocument(theme);
    EditorThemeData reread = baseA;
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
