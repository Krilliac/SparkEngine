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
                ContrastOn(fg, bg, Get(m_tokens, bg), required, rule);
            }

            /// Contrast of @p fg on a colour derived from role @p bg (reported against @p bg).
            void ContrastOn(Role fg, Role bg, Rgba background, float required, std::string_view rule)
            {
                const float ratio = ContrastRatio(Get(m_tokens, fg), background);
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
        };

        constexpr std::array kCvdRules = {
            CvdRule{Cvd::Protan, "cvd-status-protan", "cvd-accent-protan"},
            CvdRule{Cvd::Deutan, "cvd-status-deutan", "cvd-accent-deutan"},
            CvdRule{Cvd::Tritan, "cvd-status-tritan", "cvd-accent-tritan"},
        };
    } // namespace

    std::vector<AuditFailure> AuditTokens(const Tokens& t)
    {
        Auditor audit(t);
        const float textMinimum = TextContrastMinimum(t.appearance);

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
        // A hovered / pressed destructive button keeps its ErrorText label on a tinted ErrorFill.
        audit.ContrastOn(Role::ErrorText, Role::ErrorFill, DangerTintFill(t, kDangerHoverTint), kAuditTextMinimum,
                         "danger-hover");
        audit.ContrastOn(Role::ErrorText, Role::ErrorFill, DangerTintFill(t, kDangerPressTint), kAuditTextMinimum,
                         "danger-press");
        for (const Role bg : {Role::SurfaceBase, Role::SurfaceRaised, Role::SurfaceSunken})
        {
            audit.Contrast(Role::BorderStrong, bg, kAuditNonTextMinimum, "border-strong");
        }
        for (const Role bg : {Role::SurfaceBase, Role::SurfaceRaised, Role::SurfaceSelected, Role::AccentFill})
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
            for (const Role status : kStatusText)
            {
                audit.Distance(status, Role::AccentFill, audit.Simulated(status, rule.kind),
                               audit.Simulated(Role::AccentFill, rule.kind), kAuditMinCvdDelta, rule.accentRule);
            }
        }

        audit.Hue(Role::BorderFocus, Role::SurfaceSelected, kAuditMinFocusHueDegrees, "focus-hue");
        return audit.Take();
    }

    float TextContrastMinimum(Appearance appearance) noexcept
    {
        return appearance == Appearance::HighContrast ? kAuditHighContrastTextMinimum : kAuditTextMinimum;
    }

    Role MostReadableRole(const Tokens& t, std::span<const Rgba> backgrounds, std::span<const Role> candidates,
                          float* worstRatio) noexcept
    {
        Role best = candidates.empty() ? Role::TextPrimary : candidates.front();
        float bestWorst = 0.0f;
        for (const Role candidate : candidates)
        {
            float worst = 21.0f;
            for (const Rgba& background : backgrounds)
            {
                const float ratio = ContrastRatio(Get(t, candidate), background);
                worst = ratio < worst ? ratio : worst;
            }
            if (worst > bestWorst)
            {
                bestWorst = worst;
                best = candidate;
            }
        }
        if (worstRatio != nullptr)
        {
            *worstRatio = bestWorst;
        }
        return best;
    }

    Tokens AuditFailFixture(Rgba textSecondary) noexcept
    {
        Tokens fixture = GenerateTokens(BuiltinSeed(PaletteId::Ember), Appearance::Dark);
        fixture.color[static_cast<std::size_t>(Role::TextSecondary)] = textSecondary;
        return fixture;
    }
} // namespace Spark::Ui
