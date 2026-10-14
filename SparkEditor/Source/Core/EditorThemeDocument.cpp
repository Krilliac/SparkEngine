/**
 * @file EditorThemeDocument.cpp
 * @brief Theme file import/export format (moved out of EditorTheme.cpp)
 */

#include "EditorThemeDocument.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace SparkEditor
{

    namespace
    {
        constexpr std::array<std::string_view, static_cast<std::size_t>(ThemeStatusFill::Count)> kStatusFillKeys = {
            "statusFillInfo", "statusFillSuccess", "statusFillWarning", "statusFillError"};

        // The roles derived for a legacy document; these mirror CompleteLegacyFields in EditorTheme.cpp,
        // which cannot be linked here because the fuzz target builds this translation unit alone.
        constexpr float kLegacyStatusFillMix = 0.15f;
        constexpr float kLegacyUnfocusedSelectionMix = 0.22f;
        constexpr float kLegacyAccentStep = 0.12f;
        constexpr float kLightAppearanceLuminance = 0.5f;

        std::string_view ThemeDocumentAppearanceName(Spark::Ui::Appearance appearance)
        {
            switch (appearance)
            {
            case Spark::Ui::Appearance::Light:
                return "light";
            case Spark::Ui::Appearance::HighContrast:
                return "highContrast";
            case Spark::Ui::Appearance::Dark:
                break;
            }
            return "dark";
        }

        std::string_view DensityName(Spark::Ui::Density density)
        {
            return density == Spark::Ui::Density::Compact ? "compact" : "standard";
        }

        std::string ExtractString(const std::string& content, std::string_view key)
        {
            const std::string search = "\"" + std::string(key) + "\": \"";
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

        /// The text after `"key": `, up to the next separator, or nullopt when the key is absent.
        std::optional<std::string> ExtractScalar(const std::string& content, std::string_view key)
        {
            const std::string search = "\"" + std::string(key) + "\": ";
            auto pos = content.find(search);
            if (pos == std::string::npos)
            {
                return std::nullopt;
            }
            pos += search.length();
            const auto end = content.find_first_of(",}\r\n", pos);
            return content.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        }

        std::optional<float> ExtractNumber(const std::string& content, std::string_view key)
        {
            const std::optional<std::string> scalar = ExtractScalar(content, key);
            float value = 0.0f;
            if (!scalar || std::sscanf(scalar->c_str(), "%f", &value) != 1 || !std::isfinite(value))
            {
                return std::nullopt;
            }
            return value;
        }

        std::optional<bool> ExtractFlag(const std::string& content, std::string_view key)
        {
            const std::optional<std::string> scalar = ExtractScalar(content, key);
            if (!scalar)
            {
                return std::nullopt;
            }
            if (scalar->starts_with("true"))
            {
                return true;
            }
            if (scalar->starts_with("false"))
            {
                return false;
            }
            return std::nullopt;
        }

        /// sscanf's %f accepts "nan", "inf" and overflows to infinity; a colour with such a
        /// component used to reach ImGui, whose float-to-byte colour conversion is undefined
        /// for NaN. Such a colour is treated as absent, like a malformed one.
        std::optional<ThemeColor> ExtractColor(const std::string& content, std::string_view key)
        {
            const std::string search = "\"" + std::string(key) + "\": [";
            auto pos = content.find(search);
            if (pos == std::string::npos)
            {
                return std::nullopt;
            }
            pos += search.length();
            const auto end = content.find(']', pos);
            if (end == std::string::npos)
            {
                return std::nullopt;
            }
            const std::string vals = content.substr(pos, end - pos);
            float r = 0;
            float g = 0;
            float b = 0;
            float a = 1;
            // r, g and b are required; a keeps its default when the entry omits alpha.
            if (std::sscanf(vals.c_str(), "%f, %f, %f, %f", &r, &g, &b, &a) < 3)
            {
                return std::nullopt;
            }
            if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b) || !std::isfinite(a))
            {
                return std::nullopt;
            }
            return ThemeColor(r, g, b, a);
        }

        // --- Derivation of roles a legacy document does not carry -------------------------------
        // Components are clamped first so a finite but huge imported value cannot overflow.

        float Unit(float v)
        {
            return std::clamp(v, 0.0f, 1.0f);
        }

        ThemeColor Mix(const ThemeColor& from, const ThemeColor& to, float t)
        {
            const auto channel = [t](float a, float b) { return Unit(a) + (Unit(b) - Unit(a)) * t; };
            return ThemeColor(channel(from.r, to.r), channel(from.g, to.g), channel(from.b, to.b),
                              channel(from.a, to.a));
        }

        ThemeColor Opaque(const ThemeColor& c)
        {
            return ThemeColor(Unit(c.r), Unit(c.g), Unit(c.b), 1.0f);
        }

        float LinearChannel(float c)
        {
            c = Unit(c);
            return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }

        float Luminance(const ThemeColor& c)
        {
            return 0.2126f * LinearChannel(c.r) + 0.7152f * LinearChannel(c.g) + 0.0722f * LinearChannel(c.b);
        }

        float Contrast(const ThemeColor& x, const ThemeColor& y)
        {
            const float lx = Luminance(x);
            const float ly = Luminance(y);
            return (std::max(lx, ly) + 0.05f) / (std::min(lx, ly) + 0.05f);
        }

        /// Dark ink or white, whichever reads better on @p fill.
        ThemeColor ReadableInkOn(const ThemeColor& fill)
        {
            const ThemeColor darkInk(26.0f / 255.0f, 15.0f / 255.0f, 8.0f / 255.0f);
            const ThemeColor lightInk(1.0f, 1.0f, 1.0f);
            return Contrast(darkInk, fill) >= Contrast(lightInk, fill) ? darkInk : lightInk;
        }

        /// Which keys the document defined, so derivation never overwrites an imported value.
        struct Defined
        {
            std::array<bool, kThemeColourFields.size()> colour{};
            std::array<bool, kStatusFillKeys.size()> fill{};
            bool appearance = false;

            bool Has(ThemeColor EditorThemeData::*member) const
            {
                for (std::size_t i = 0; i < kThemeColourFields.size(); ++i)
                {
                    if (kThemeColourFields[i].member == member)
                    {
                        return colour[i];
                    }
                }
                return false;
            }
        };

        bool IsDefaultColor(const ThemeColor& c)
        {
            const ThemeColor unset;
            return c.r == unset.r && c.g == unset.g && c.b == unset.b && c.a == unset.a;
        }

        /// True when @p t sets none of the Ember Signal roles (a default-constructed or pre-token theme).
        bool LacksEmberRoles(const EditorThemeData& t)
        {
            const bool coloursUnset = IsDefaultColor(t.borderStrong) && IsDefaultColor(t.surfaceOverlay) &&
                                      IsDefaultColor(t.surfaceSelectedUnfocused) && IsDefaultColor(t.textOnAccent) &&
                                      IsDefaultColor(t.textInfo) && IsDefaultColor(t.accentHover) &&
                                      IsDefaultColor(t.accentActive) && IsDefaultColor(t.accentText);
            return coloursUnset && std::ranges::all_of(t.statusFills, IsDefaultColor);
        }

        /// Fill every role the document did not define from the colours it did.
        void DeriveMissingRoles(EditorThemeData& t, const Defined& defined)
        {
            const auto derive = [&](ThemeColor EditorThemeData::*member, const ThemeColor& value)
            {
                if (!defined.Has(member))
                {
                    t.*member = value;
                }
            };
            derive(&EditorThemeData::buttonDisabled, t.textDisabled);
            derive(&EditorThemeData::menuItem, t.backgroundLight);
            derive(&EditorThemeData::borderStrong, t.borderLight);
            derive(&EditorThemeData::surfaceOverlay, t.backgroundLight);
            derive(&EditorThemeData::surfaceSelectedUnfocused,
                   Mix(t.background, t.textSecondary, kLegacyUnfocusedSelectionMix));
            derive(&EditorThemeData::textInfo, t.textAccent);
            derive(&EditorThemeData::accentHover, Mix(t.accent, ThemeColor(1.0f, 1.0f, 1.0f), kLegacyAccentStep));
            derive(&EditorThemeData::accentActive, Mix(t.accent, ThemeColor(0.0f, 0.0f, 0.0f), kLegacyAccentStep));
            derive(&EditorThemeData::accentText, t.textAccent);
            derive(&EditorThemeData::textOnAccent, ReadableInkOn(t.accent));

            const ThemeColor opaqueBackground = Opaque(t.background);
            const std::array<const ThemeColor*, kStatusFillKeys.size()> inks = {&t.textInfo, &t.textSuccess,
                                                                                &t.textWarning, &t.textError};
            for (std::size_t i = 0; i < inks.size(); ++i)
            {
                if (!defined.fill[i])
                {
                    t.statusFills[i] = Mix(opaqueBackground, Opaque(*inks[i]), kLegacyStatusFillMix);
                }
            }
            if (!defined.appearance)
            {
                t.appearance = Luminance(t.background) > kLightAppearanceLuminance ? Spark::Ui::Appearance::Light
                                                                                   : Spark::Ui::Appearance::Dark;
            }
        }

        void ReadColours(const std::string& content, EditorThemeData& theme, Defined& defined)
        {
            for (std::size_t i = 0; i < kThemeColourFields.size(); ++i)
            {
                if (const auto color = ExtractColor(content, kThemeColourFields[i].key))
                {
                    theme.*kThemeColourFields[i].member = *color;
                    defined.colour[i] = true;
                }
            }
            for (std::size_t i = 0; i < kStatusFillKeys.size(); ++i)
            {
                if (const auto color = ExtractColor(content, kStatusFillKeys[i]))
                {
                    theme.statusFills[i] = *color;
                    defined.fill[i] = true;
                }
            }
        }

        void ReadMetric(const std::string& content, const ThemeMetricField& field, EditorThemeData& theme)
        {
            if (const auto value = ExtractNumber(content, field.key))
            {
                theme.*field.member = std::clamp(*value, field.minimum, field.maximum);
            }
        }

        void ReadStyle(const std::string& content, EditorThemeData& theme, Defined& defined)
        {
            for (const ThemeMetricField& field : kThemeMetricFields)
            {
                ReadMetric(content, field, theme);
            }
            for (const ThemeMetricField& field : kThemeTypographyFields)
            {
                ReadMetric(content, field, theme);
            }
            for (const ThemeFlagField& field : kThemeFlagFields)
            {
                if (const auto flag = ExtractFlag(content, field.key))
                {
                    theme.*field.member = *flag;
                }
            }
            if (std::string family = ExtractString(content, "fontFamily"); !family.empty())
            {
                theme.fontFamily = std::move(family);
            }

            const std::string appearance = ExtractString(content, "appearance");
            for (const auto candidate :
                 {Spark::Ui::Appearance::Dark, Spark::Ui::Appearance::Light, Spark::Ui::Appearance::HighContrast})
            {
                if (appearance == ThemeDocumentAppearanceName(candidate))
                {
                    theme.appearance = candidate;
                    defined.appearance = true;
                }
            }
            const std::string density = ExtractString(content, "density");
            for (const auto candidate : {Spark::Ui::Density::Standard, Spark::Ui::Density::Compact})
            {
                if (density == DensityName(candidate))
                {
                    theme.density = candidate;
                }
            }
        }
    } // namespace

    bool ParseThemeDocument(const std::string& content, const EditorThemeData& base, EditorThemeData& outTheme,
                            std::string* error)
    {
        const bool deriveRoles = LacksEmberRoles(base);
        outTheme = base;
        outTheme.name = ExtractString(content, "name");
        outTheme.description = ExtractString(content, "description");
        outTheme.author = ExtractString(content, "author");

        Defined defined;
        ReadColours(content, outTheme, defined);
        ReadStyle(content, outTheme, defined);
        if (deriveRoles)
        {
            DeriveMissingRoles(outTheme, defined);
        }

        const bool named = !outTheme.name.empty();
        if (error != nullptr)
        {
            *error = named ? std::string() : std::string("the document names no theme");
        }
        return named;
    }

    bool ParseThemeDocument(const std::string& content, EditorThemeData& outTheme)
    {
        const EditorThemeData base = outTheme;
        return ParseThemeDocument(content, base, outTheme);
    }

    std::string WriteThemeDocument(const EditorThemeData& theme)
    {
        std::ostringstream file;
        const auto writeColour = [&file](std::string_view key, const ThemeColor& c, bool last)
        {
            file << "    \"" << key << "\": [" << c.r << ", " << c.g << ", " << c.b << ", " << c.a << "]"
                 << (last ? "\n" : ",\n");
        };
        const auto writeMetrics = [&file, &theme](const auto& fields)
        {
            for (const ThemeMetricField& field : fields)
            {
                file << "    \"" << field.key << "\": " << theme.*field.member << ",\n";
            }
        };

        file << "{\n";
        file << "  \"version\": " << kThemeDocumentVersion << ",\n";
        file << R"(  "name": ")" << theme.name << "\",\n";
        file << R"(  "description": ")" << theme.description << "\",\n";
        file << R"(  "author": ")" << theme.author << "\",\n";
        file << R"(  "appearance": ")" << ThemeDocumentAppearanceName(theme.appearance) << "\",\n";
        file << R"(  "density": ")" << DensityName(theme.density) << "\",\n";
        file << R"(  "fontFamily": ")" << theme.fontFamily << "\",\n";
        file << "  \"colors\": {\n";
        for (const ThemeColourField& field : kThemeColourFields)
        {
            writeColour(field.key, theme.*field.member, false);
        }
        for (std::size_t i = 0; i < kStatusFillKeys.size(); ++i)
        {
            writeColour(kStatusFillKeys[i], theme.statusFills[i], i + 1 == kStatusFillKeys.size());
        }
        file << "  },\n";
        file << "  \"metrics\": {\n";
        writeMetrics(kThemeMetricFields);
        writeMetrics(kThemeTypographyFields);
        for (std::size_t i = 0; i < kThemeFlagFields.size(); ++i)
        {
            file << "    \"" << kThemeFlagFields[i].key
                 << "\": " << (theme.*kThemeFlagFields[i].member ? "true" : "false")
                 << (i + 1 == kThemeFlagFields.size() ? "\n" : ",\n");
        }
        file << "  }\n}\n";
        return file.str();
    }

} // namespace SparkEditor
