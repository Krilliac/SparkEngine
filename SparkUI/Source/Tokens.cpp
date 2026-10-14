/**
 * @file Tokens.cpp
 * @brief Built-in token tables (Palettes.inl), the family derivation rules for custom seeds and
 *        the metrics profiles.
 */

#include "SparkUI/Tokens.h"

#include "SparkUI/Color.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>

namespace Spark::Ui
{
    namespace
    {
        constexpr std::size_t kAppearanceCount = 3;
        constexpr std::size_t kFamilyCount = 3;
        constexpr std::size_t kColumnCount = kAppearanceCount * kFamilyCount;

        using PaletteRow = std::array<std::uint32_t, kColumnCount>;

        constexpr std::array<PaletteRow, kRoleCount> kPalettes = {{
#define SPARK_UI_PALETTE_ROW(role, ...) PaletteRow{__VA_ARGS__},
#include "Palettes.inl"
#undef SPARK_UI_PALETTE_ROW
        }};

        constexpr std::array<Role, kRoleCount> kPaletteRoles = {{
#define SPARK_UI_PALETTE_ROW(role, ...) Role::role,
#include "Palettes.inl"
#undef SPARK_UI_PALETTE_ROW
        }};

        constexpr bool PaletteRowsMatchRoleOrder() noexcept
        {
            for (std::size_t i = 0; i < kRoleCount; ++i)
            {
                if (static_cast<std::size_t>(kPaletteRoles[i]) != i)
                {
                    return false;
                }
            }
            return true;
        }
        static_assert(PaletteRowsMatchRoleOrder(), "Palettes.inl rows must follow the Role enum order");

        constexpr std::array<std::string_view, kRoleCount> kRoleNames = {{
#define SPARK_UI_PALETTE_ROW(role, ...) #role,
#include "Palettes.inl"
#undef SPARK_UI_PALETTE_ROW
        }};

        constexpr std::array<PaletteSeed, kFamilyCount> kSeeds = {{
            {"Ember Studio", Hex(0x110e0d), Hex(0x1a1716), Hex(0x24201e), Hex(0x3c3733), Hex(0xefeeeb), Hex(0x918b85),
             Hex(0xf1823a), Hex(0x68d3ee)},
            {"Cobalt Forge", Hex(0x0b1017), Hex(0x151f2a), Hex(0x202e3d), Hex(0x314557), Hex(0xe6eef7), Hex(0x94a8bb),
             Hex(0x55c2e8), Hex(0xa99aff)},
            {"Graphite Signal", Hex(0x101113), Hex(0x1b1c1f), Hex(0x282a2f), Hex(0x46484e), Hex(0xececef),
             Hex(0xa8aab1), Hex(0xc9d1dd), Hex(0x78d3b2)},
        }};

        // Margin the custom-seed correction adds above each audit minimum (same as the generator).
        constexpr float kContrastMargin = 0.06f;
        constexpr float kSelectionAlpha = 0.35f;
        constexpr float kHighContrastSelectionAlpha = 0.50f;
        constexpr float kDropTargetAlpha = 0.70f;

        constexpr std::array kSurfaces = {Role::SurfaceSunken,           Role::SurfaceBase,  Role::SurfaceRaised,
                                          Role::SurfaceOverlay,          Role::SurfaceHover, Role::SurfaceSelected,
                                          Role::SurfaceSelectedUnfocused};

        Tokens TableTokens(std::size_t family, Appearance appearance) noexcept
        {
            Tokens t;
            t.appearance = appearance;
            const std::size_t column = family * kAppearanceCount + static_cast<std::size_t>(appearance);
            for (std::size_t role = 0; role < kRoleCount; ++role)
            {
                t.color[role] = FromPackedRgba(kPalettes[role][column]);
            }
            return t;
        }

        Rgba& At(Tokens& t, Role r) noexcept
        {
            return t.color[static_cast<std::size_t>(r)];
        }

        bool SameColours(const PaletteSeed& x, const PaletteSeed& y) noexcept
        {
            return x.sunken == y.sunken && x.base == y.base && x.raised == y.raised &&
                   x.borderSubtle == y.borderSubtle && x.text == y.text && x.textMuted == y.textMuted &&
                   x.accent == y.accent && x.secondary == y.secondary;
        }

        Rgba WithLightnessOf(Rgba hue, Rgba lightnessSource) noexcept
        {
            OkLab lab = ToOkLab(hue);
            lab.L = ToOkLab(lightnessSource).L;
            return FromOkLab(lab, hue.a);
        }

        float WorstContrast(Rgba fg, std::span<const Rgba> backgrounds) noexcept
        {
            float worst = 21.0f;
            for (const Rgba& bg : backgrounds)
            {
                const float ratio = ContrastRatio(fg, bg);
                worst = ratio < worst ? ratio : worst;
            }
            return worst;
        }

        void Lift(Tokens& t, Role role, std::initializer_list<Role> backgrounds, float minimum) noexcept
        {
            std::array<Rgba, kSurfaces.size()> colours{};
            std::size_t count = 0;
            for (const Role bg : backgrounds)
            {
                colours[count++] = Get(t, bg);
            }
            At(t, role) =
                EnsureContrast(Get(t, role), std::span<const Rgba>(colours.data(), count), minimum + kContrastMargin);
        }

        void LiftOnSurfaces(Tokens& t, Role role, float minimum) noexcept
        {
            std::array<Rgba, kSurfaces.size()> colours{};
            for (std::size_t i = 0; i < kSurfaces.size(); ++i)
            {
                colours[i] = Get(t, kSurfaces[i]);
            }
            At(t, role) = EnsureContrast(Get(t, role), colours, minimum + kContrastMargin);
        }

        /// Family rules (spec section 2.3) applied to a seed that is not one of the built-ins.
        Tokens DeriveCustom(const PaletteSeed& seed, Appearance appearance) noexcept
        {
            Tokens t = TableTokens(0, appearance);
            if (appearance == Appearance::Dark)
            {
                At(t, Role::SurfaceSunken) = seed.sunken;
                At(t, Role::SurfaceBase) = seed.base;
                At(t, Role::SurfaceRaised) = seed.raised;
                At(t, Role::BorderSubtle) = seed.borderSubtle;
                At(t, Role::TextPrimary) = seed.text;
                At(t, Role::SurfaceOverlay) = Mix(seed.raised, seed.text, 0.035f);
                At(t, Role::SurfaceHover) = Mix(seed.base, seed.accent, 0.18f);
                At(t, Role::SurfaceSelected) = Mix(seed.base, seed.accent, 0.28f);
                At(t, Role::SurfaceSelectedUnfocused) = Mix(seed.base, seed.textMuted, 0.22f);
                At(t, Role::BorderStrong) = Mix(seed.borderSubtle, seed.textMuted, 0.60f);
                At(t, Role::TextSecondary) = Mix(seed.textMuted, seed.text, 0.30f);
                At(t, Role::TextDisabled) = Mix(seed.base, seed.textMuted, 0.75f);
                At(t, Role::AccentFill) = seed.accent;
                At(t, Role::AccentFillHover) = ShiftLightness(seed.accent, 0.05f);
                At(t, Role::AccentFillActive) = ShiftLightness(seed.accent, -0.06f);
                At(t, Role::AccentText) = ShiftLightness(seed.accent, 0.08f);
                At(t, Role::Secondary) = seed.secondary;
                At(t, Role::TextLink) = seed.secondary;
                At(t, Role::BorderFocus) = seed.secondary;
            }
            else
            {
                // Light and High Contrast keep Ember's neutral ramp; the seed contributes its accent
                // and secondary hues at the lightness Ember uses for those roles.
                for (const Role role :
                     {Role::AccentFill, Role::AccentFillHover, Role::AccentFillActive, Role::AccentText})
                {
                    At(t, role) = WithLightnessOf(seed.accent, Get(t, role));
                }
                for (const Role role : {Role::Secondary, Role::TextLink})
                {
                    At(t, role) = WithLightnessOf(seed.secondary, Get(t, role));
                }
                if (appearance == Appearance::Light)
                {
                    At(t, Role::SurfaceHover) = Mix(Get(t, Role::SurfaceBase), Get(t, Role::AccentFill), 0.10f);
                    At(t, Role::SurfaceSelected) = Mix(Get(t, Role::SurfaceBase), Get(t, Role::AccentFill), 0.20f);
                    At(t, Role::BorderFocus) = Get(t, Role::Secondary);
                }
            }

            const float textMinimum = appearance == Appearance::HighContrast ? 7.0f : 4.5f;
            for (const Role role : {Role::TextPrimary, Role::TextSecondary, Role::TextLink, Role::AccentText,
                                    Role::SuccessText, Role::WarningText, Role::ErrorText, Role::InfoText})
            {
                LiftOnSurfaces(t, role, textMinimum);
            }
            LiftOnSurfaces(t, Role::TextDisabled, 3.0f);
            Lift(t, Role::BorderStrong, {Role::SurfaceBase, Role::SurfaceRaised, Role::SurfaceSunken}, 3.0f);
            Lift(t, Role::BorderFocus, {Role::SurfaceBase, Role::SurfaceRaised, Role::SurfaceSelected}, 3.0f);
            Lift(t, Role::AccentFill, {Role::SurfaceBase}, 3.0f);

            // Dark or light text on the accent, whichever reads better on all three fills.
            const std::array fills = {Get(t, Role::AccentFill), Get(t, Role::AccentFillHover),
                                      Get(t, Role::AccentFillActive)};
            const Rgba darkInk = Hex(0x1a0f08);
            const Rgba lightInk = Hex(0xffffff);
            At(t, Role::TextOnAccent) =
                WorstContrast(darkInk, fills) >= WorstContrast(lightInk, fills) ? darkInk : lightInk;
            Lift(t, Role::TextOnAccent, {Role::AccentFill, Role::AccentFillHover, Role::AccentFillActive}, 4.5f);

            At(t, Role::Graph1) = Get(t, Role::AccentFill);
            At(t, Role::Graph2) = Get(t, Role::Secondary);
            At(t, Role::Graph3) = Get(t, Role::SuccessText);
            At(t, Role::Graph4) = Get(t, Role::WarningText);
            for (const Role role :
                 {Role::Graph2, Role::Graph5, Role::Category1, Role::Category2, Role::Category3, Role::Category4,
                  Role::Category5, Role::Category6, Role::Category7, Role::Category8})
            {
                Lift(t, role, {Role::SurfaceSunken}, 3.0f);
            }

            const float selectionAlpha =
                appearance == Appearance::HighContrast ? kHighContrastSelectionAlpha : kSelectionAlpha;
            At(t, Role::DropTarget) = WithAlpha(Get(t, Role::Secondary), kDropTargetAlpha);
            At(t, Role::SelectionTextBg) = WithAlpha(Get(t, Role::AccentFill), selectionAlpha);
            return t;
        }
    } // namespace

    const PaletteSeed& BuiltinSeed(PaletteId id) noexcept
    {
        const auto index = static_cast<std::size_t>(id);
        return kSeeds[index < kSeeds.size() ? index : 0];
    }

    Tokens GenerateTokens(const PaletteSeed& seed, Appearance appearance, Density density) noexcept
    {
        Tokens t;
        bool builtin = false;
        for (std::size_t family = 0; family < kSeeds.size(); ++family)
        {
            if (SameColours(seed, kSeeds[family]))
            {
                t = TableTokens(family, appearance);
                builtin = true;
                break;
            }
        }
        if (!builtin)
        {
            t = DeriveCustom(seed, appearance);
        }
        t.metrics = MetricsFor(density, MetricsProfile::Editor);
        if (appearance == Appearance::HighContrast)
        {
            t.metrics.focusRing = 3.0f;
        }
        t.type = Typography{};
        return t;
    }

    Metrics MetricsFor(Density density, MetricsProfile profile) noexcept
    {
        Metrics m;
        // Radii and borders are shared by every profile (spec section 2.6).
        m.rWindow = 3.0f;
        m.rChild = 3.0f;
        m.rFrame = 3.0f;
        m.rPopup = 5.0f;
        m.rScrollbar = 3.0f;
        m.rGrab = 2.0f;
        m.rTab = 3.0f;
        m.rCard = 6.0f;
        m.bWindow = 0.0f;
        m.bChild = 0.0f;
        m.bPopup = 1.0f;
        m.bFrame = 1.0f;
        m.tabOverline = 2.0f;
        m.focusRing = 2.0f;

        struct Spacing
        {
            float windowPadX, windowPadY, framePadX, framePadY, itemSpacingX, itemSpacingY, itemInnerX, itemInnerY,
                cellPadX, cellPadY, indent, scrollbar, grabMin;
        };
        constexpr Spacing kStandard{8, 8, 8, 5, 8, 6, 6, 4, 6, 4, 16, 12, 12};
        constexpr Spacing kCompact{8, 6, 6, 3, 8, 4, 4, 4, 6, 2, 14, 10, 10};
        constexpr Spacing kComfortable{24, 20, 12, 8, 12, 10, 8, 6, 8, 6, 16, 12, 12};
        constexpr Spacing kHud{12, 10, 10, 6, 8, 6, 6, 4, 6, 4, 16, 12, 12};

        Spacing s = density == Density::Compact ? kCompact : kStandard;
        if (profile == MetricsProfile::Comfortable)
        {
            s = kComfortable;
        }
        else if (profile == MetricsProfile::Hud)
        {
            s = kHud;
        }
        m.windowPadX = s.windowPadX;
        m.windowPadY = s.windowPadY;
        m.framePadX = s.framePadX;
        m.framePadY = s.framePadY;
        m.itemSpacingX = s.itemSpacingX;
        m.itemSpacingY = s.itemSpacingY;
        m.itemInnerX = s.itemInnerX;
        m.itemInnerY = s.itemInnerY;
        m.cellPadX = s.cellPadX;
        m.cellPadY = s.cellPadY;
        m.indent = s.indent;
        m.scrollbar = s.scrollbar;
        m.grabMin = s.grabMin;
        return m;
    }

    Rgba Get(const Tokens& t, Role r) noexcept
    {
        const auto index = static_cast<std::size_t>(r);
        return index < kRoleCount ? t.color[index] : Rgba{};
    }

    std::string_view RoleName(Role r) noexcept
    {
        const auto index = static_cast<std::size_t>(r);
        return index < kRoleCount ? kRoleNames[index] : std::string_view{"Unknown"};
    }

    std::string_view AppearanceName(Appearance appearance) noexcept
    {
        switch (appearance)
        {
        case Appearance::Dark:
            return "Dark";
        case Appearance::Light:
            return "Light";
        case Appearance::HighContrast:
            return "HighContrast";
        }
        return "Unknown";
    }
} // namespace Spark::Ui
