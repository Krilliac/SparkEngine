/**
 * @file TokenAudit.cpp
 * @brief Token-set accessibility audit; the rules mirror tools/ui/generate_palettes.py exactly.
 */

#include "SparkUI/TokenAudit.h"

#include "SparkUI/Color.h"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <utility>

namespace Spark::Ui
{
    namespace
    {
        constexpr std::array kSurfaces = {Role::SurfaceSunken,           Role::SurfaceBase,  Role::SurfaceRaised,
                                          Role::SurfaceOverlay,          Role::SurfaceHover, Role::SurfaceSelected,
                                          Role::SurfaceSelectedUnfocused};
        constexpr std::array kBodyText = {Role::TextPrimary, Role::TextSecondary, Role::TextLink, Role::AccentText};
        constexpr std::array kStatusText = {Role::SuccessText, Role::WarningText, Role::ErrorText, Role::InfoText};
        constexpr std::array kStatusFill = {Role::SuccessFill, Role::WarningFill, Role::ErrorFill, Role::InfoFill};
        constexpr std::array kGraphs = {Role::Graph1,    Role::Graph2,    Role::Graph3,    Role::Graph4,
                                        Role::Graph5,    Role::Category1, Role::Category2, Role::Category3,
                                        Role::Category4, Role::Category5, Role::Category6, Role::Category7,
                                        Role::Category8};

        class Auditor
        {
          public:
            explicit Auditor(const Tokens& tokens) : m_tokens(tokens) {}

            void Contrast(Role fg, Role bg, float required, std::string_view rule)
            {
                const float ratio = ContrastRatio(Get(m_tokens, fg), Get(m_tokens, bg));
                if (ratio < required)
                {
                    m_failures.push_back(AuditFailure{fg, bg, ratio, required, rule});
                }
            }

            void Distance(Role fg, Role bg, OkLab x, OkLab y, float required, std::string_view rule)
            {
                const float distance = DeltaE(x, y);
                if (distance < required)
                {
                    m_failures.push_back(AuditFailure{fg, bg, distance, required, rule});
                }
            }

            void Hue(Role fg, Role bg, float required, std::string_view rule)
            {
                const float degrees = HueDistanceDegrees(Get(m_tokens, fg), Get(m_tokens, bg));
                if (degrees < required)
                {
                    m_failures.push_back(AuditFailure{fg, bg, degrees, required, rule});
                }
            }

            [[nodiscard]] OkLab Simulated(Role role, Cvd kind) const noexcept
            {
                return SimulateCvdLab(Get(m_tokens, role), kind);
            }

            [[nodiscard]] OkLab Lab(Role role) const noexcept { return ToOkLab(Get(m_tokens, role)); }

            std::vector<AuditFailure> Take() { return std::move(m_failures); }

          private:
            const Tokens& m_tokens;
            std::vector<AuditFailure> m_failures;
        };

        struct CvdRule
        {
            Cvd kind;
            std::string_view statusRule;
            std::string_view accentRule;
            bool checksAccent;
        };

        constexpr std::array kCvdRules = {
            CvdRule{Cvd::Protan, "cvd-status-protan", "cvd-accent-protan", true},
            CvdRule{Cvd::Deutan, "cvd-status-deutan", "cvd-accent-deutan", true},
            CvdRule{Cvd::Tritan, "cvd-status-tritan", "", false},
        };
    } // namespace

    std::vector<AuditFailure> AuditTokens(const Tokens& t)
    {
        Auditor audit(t);
        const float textMinimum =
            t.appearance == Appearance::HighContrast ? kAuditHighContrastTextMinimum : kAuditTextMinimum;

        for (const Role fg : kBodyText)
        {
            for (const Role bg : kSurfaces)
            {
                audit.Contrast(fg, bg, textMinimum, "text-contrast");
            }
        }
        for (const Role fg : kStatusText)
        {
            for (const Role bg : kSurfaces)
            {
                audit.Contrast(fg, bg, textMinimum, "text-contrast");
            }
        }
        for (const Role bg : kSurfaces)
        {
            audit.Contrast(Role::TextDisabled, bg, kAuditNonTextMinimum, "disabled-contrast");
        }
        for (const Role bg : {Role::AccentFill, Role::AccentFillHover, Role::AccentFillActive})
        {
            audit.Contrast(Role::TextOnAccent, bg, kAuditTextMinimum, "on-accent-contrast");
        }
        for (std::size_t i = 0; i < kStatusText.size(); ++i)
        {
            audit.Contrast(kStatusText[i], kStatusFill[i], kAuditTextMinimum, "status-on-fill");
        }
        for (const Role bg : {Role::SurfaceBase, Role::SurfaceRaised, Role::SurfaceSunken})
        {
            audit.Contrast(Role::BorderStrong, bg, kAuditNonTextMinimum, "border-strong");
        }
        for (const Role bg : {Role::SurfaceBase, Role::SurfaceRaised, Role::SurfaceSelected})
        {
            audit.Contrast(Role::BorderFocus, bg, kAuditNonTextMinimum, "border-focus");
        }
        audit.Contrast(Role::AccentFill, Role::SurfaceBase, kAuditNonTextMinimum, "accent-on-base");
        for (const Role graph : kGraphs)
        {
            audit.Contrast(graph, Role::SurfaceSunken, kAuditNonTextMinimum, "graph-on-sunken");
        }

        for (const CvdRule& rule : kCvdRules)
        {
            for (std::size_t i = 0; i < kStatusText.size(); ++i)
            {
                for (std::size_t j = i + 1; j < kStatusText.size(); ++j)
                {
                    audit.Distance(kStatusText[i], kStatusText[j], audit.Simulated(kStatusText[i], rule.kind),
                                   audit.Simulated(kStatusText[j], rule.kind), kAuditMinCvdDelta, rule.statusRule);
                }
            }
        }
        for (const CvdRule& rule : kCvdRules)
        {
            if (!rule.checksAccent)
            {
                continue;
            }
            for (const Role status : kStatusText)
            {
                audit.Distance(status, Role::AccentFill, audit.Simulated(status, rule.kind),
                               audit.Simulated(Role::AccentFill, rule.kind), kAuditMinCvdDelta, rule.accentRule);
            }
        }

        audit.Distance(Role::BorderFocus, Role::AccentFill, audit.Lab(Role::BorderFocus), audit.Lab(Role::AccentFill),
                       kAuditMinFocusAccentDelta, "focus-accent-distance");
        audit.Hue(Role::BorderFocus, Role::SurfaceSelected, kAuditMinFocusHueDegrees, "focus-hue");
        return audit.Take();
    }

    Tokens AuditFailFixture(Rgba textSecondary) noexcept
    {
        Tokens fixture = GenerateTokens(BuiltinSeed(PaletteId::Ember), Appearance::Dark);
        fixture.color[static_cast<std::size_t>(Role::TextSecondary)] = textSecondary;
        return fixture;
    }
} // namespace Spark::Ui
