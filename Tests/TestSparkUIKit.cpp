/**
 * @file TestSparkUIKit.cpp
 * @brief Spark UI library: token audit, style mapping, fonts, per-context runtime, widgets and
 *        the SDK theme accessor.
 *
 * The token tests need only SparkUITokens (linked into every SparkTests build); the rest need
 * Dear ImGui and run when SparkTests links SparkUIKit (SPARK_TEST_HAS_IMGUI). Every test is named
 * Gated_SparkUi_* so SPARK_TEST_NAME_PREFIX=Gated_SparkUi_ selects the whole family.
 */

#include "TestFramework.h"

#include "SparkUI/Color.h"
#include "SparkUI/Dpi.h"
#include "SparkUI/TokenAudit.h"
#include "SparkUI/Tokens.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace Spark::Ui;

    constexpr std::array kFamilies = {PaletteId::Ember, PaletteId::Cobalt, PaletteId::Graphite};
    constexpr std::array kAppearances = {Appearance::Dark, Appearance::Light, Appearance::HighContrast};

    Tokens EmberTokens(Appearance appearance)
    {
        return GenerateTokens(BuiltinSeed(PaletteId::Ember), appearance);
    }

    void PrintFailures(std::string_view label, const std::vector<AuditFailure>& failures)
    {
        for (const AuditFailure& failure : failures)
        {
            std::cerr << "  " << label << ": " << failure.rule << " " << RoleName(failure.fg) << " vs "
                      << RoleName(failure.bg) << " = " << failure.ratio << " (needs " << failure.required << ")\n";
        }
    }
} // namespace

TEST(Gated_SparkUi_TokenAudit_AllBuiltinSetsPass)
{
    std::size_t audited = 0;
    for (const PaletteId family : kFamilies)
    {
        for (const Appearance appearance : kAppearances)
        {
            const Tokens tokens = GenerateTokens(BuiltinSeed(family), appearance);
            EXPECT_TRUE(tokens.appearance == appearance);
            const std::vector<AuditFailure> failures = AuditTokens(tokens);
            const std::string label =
                std::string(BuiltinSeed(family).id) + " " + std::string(AppearanceName(appearance));
            PrintFailures(label, failures);
            EXPECT_EQ(failures.size(), std::size_t{0});
            ++audited;
        }
    }
    // A loop that silently skipped sets would also report no failures.
    EXPECT_EQ(audited, std::size_t{9});
}

TEST(Gated_SparkUi_TokenAudit_FailFixtureFails)
{
    const std::vector<AuditFailure> failures = AuditTokens(AuditFailFixture());
    EXPECT_GE(failures.size(), std::size_t{1});
    bool flagsTextSecondaryOnBase = false;
    for (const AuditFailure& failure : failures)
    {
        flagsTextSecondaryOnBase =
            flagsTextSecondaryOnBase || (failure.fg == Role::TextSecondary && failure.bg == Role::SurfaceBase &&
                                         failure.rule == "text-contrast" && failure.ratio < failure.required);
    }
    EXPECT_TRUE(flagsTextSecondaryOnBase);

    // The fixture differs from Ember Dark only in TextSecondary, so the real colour passes.
    const Tokens emberDark = GenerateTokens(BuiltinSeed(PaletteId::Ember), Appearance::Dark);
    EXPECT_EQ(AuditTokens(AuditFailFixture(Get(emberDark, Role::TextSecondary))).size(), std::size_t{0});
}

namespace
{
    bool HasFailure(const std::vector<AuditFailure>& failures, std::string_view rule, Role fg, Role bg)
    {
        for (const AuditFailure& failure : failures)
        {
            if (failure.rule == rule && failure.fg == fg && failure.bg == bg && failure.ratio < failure.required)
            {
                return true;
            }
        }
        return false;
    }
} // namespace

TEST(Gated_SparkUi_TokenAudit_EnforcesUnrelaxedSpecRules)
{
    // Spec section 2.5 thresholds, not a relaxed variant.
    EXPECT_EQ(kAuditMinCvdDelta, 0.08f);
    EXPECT_EQ(kAuditNonTextMinimum, 3.0f);
    EXPECT_EQ(TextContrastMinimum(Appearance::HighContrast), 7.0f);
    EXPECT_EQ(TextContrastMinimum(Appearance::Light), 4.5f);

    // BorderFocus must reach 3:1 against AccentFill: the original seed pair (#68d3ee ring next to
    // the #f1823a accent, about 1.5:1) is a failure even though it passes on every surface.
    Tokens seedPair = GenerateTokens(BuiltinSeed(PaletteId::Ember), Appearance::Dark);
    seedPair.color[static_cast<std::size_t>(Role::BorderFocus)] = Hex(0x68d3ee);
    seedPair.color[static_cast<std::size_t>(Role::AccentFill)] = Hex(0xf1823a);
    const std::vector<AuditFailure> focus = AuditTokens(seedPair);
    PrintFailures("seed focus/accent", focus);
    EXPECT_TRUE(HasFailure(focus, "border-focus", Role::BorderFocus, Role::AccentFill));

    // Accent pairs are checked under tritan vision too: an error colour equal to the accent fails there.
    Tokens accentTwin = GenerateTokens(BuiltinSeed(PaletteId::Ember), Appearance::Dark);
    accentTwin.color[static_cast<std::size_t>(Role::ErrorText)] = Get(accentTwin, Role::AccentFill);
    const std::vector<AuditFailure> cvd = AuditTokens(accentTwin);
    EXPECT_TRUE(HasFailure(cvd, "cvd-accent-tritan", Role::ErrorText, Role::AccentFill));
    EXPECT_TRUE(HasFailure(cvd, "cvd-accent-protan", Role::ErrorText, Role::AccentFill));

    // The destructive button's hovered / pressed fills are audited: a fill already close to the
    // label leaves no room for the tint.
    Tokens weakDanger = GenerateTokens(BuiltinSeed(PaletteId::Ember), Appearance::Dark);
    weakDanger.color[static_cast<std::size_t>(Role::ErrorFill)] = Hex(0x8a5058);
    const std::vector<AuditFailure> danger = AuditTokens(weakDanger);
    EXPECT_TRUE(HasFailure(danger, "danger-press", Role::ErrorText, Role::ErrorFill));
}

TEST(Gated_SparkUi_DangerButtonLabelReadsInEveryState)
{
    std::size_t checked = 0;
    for (const PaletteId family : kFamilies)
    {
        for (const Appearance appearance : kAppearances)
        {
            const Tokens tokens = GenerateTokens(BuiltinSeed(family), appearance);
            const Rgba label = Get(tokens, Role::ErrorText);
            EXPECT_GE(ContrastRatio(label, Get(tokens, Role::ErrorFill)), 4.5f);
            EXPECT_GE(ContrastRatio(label, DangerTintFill(tokens, kDangerHoverTint)), 4.5f);
            EXPECT_GE(ContrastRatio(label, DangerTintFill(tokens, kDangerPressTint)), 4.5f);
            // Hover and press are visibly different from the resting fill and from each other.
            EXPECT_TRUE(DangerTintFill(tokens, kDangerHoverTint) != Get(tokens, Role::ErrorFill));
            EXPECT_TRUE(DangerTintFill(tokens, kDangerPressTint) != DangerTintFill(tokens, kDangerHoverTint));
            ++checked;
        }
    }
    EXPECT_EQ(checked, std::size_t{9});
}

TEST(Gated_SparkUi_MostReadableRoleMeasuresContrast)
{
    const Tokens light = GenerateTokens(BuiltinSeed(PaletteId::Ember), Appearance::Light);
    // Ember Light's TextOnAccent is white, which fails on the bright end of an orange ramp.
    const std::array ramp = {Hex(0xe8611a), Hex(0xf1823a), Hex(0xffc857)};
    const std::array inks = {Role::TextOnAccent, Role::TextPrimary};
    float worst = 0.0f;
    EXPECT_TRUE(MostReadableRole(light, ramp, inks, &worst) == Role::TextPrimary);
    EXPECT_GE(worst, 4.5f);

    const Tokens dark = EmberTokens(Appearance::Dark);
    EXPECT_TRUE(MostReadableRole(dark, ramp, inks, &worst) == Role::TextOnAccent);
    EXPECT_GE(worst, 4.5f);

    EXPECT_TRUE(MostReadableRole(dark, ramp, std::span<const Role>{}, &worst) == Role::TextPrimary);
    EXPECT_EQ(worst, 0.0f);
}

TEST(Gated_SparkUi_MinimumAlphaForContrastIsTight)
{
    const Rgba fg = Hex(0xffd867);
    const Rgba bg = Hex(0x1a1716);
    const float alpha = MinimumAlphaForContrast(fg, bg, 3.0f);
    EXPECT_GT(alpha, 0.0f);
    EXPECT_LT(alpha, 1.0f);
    EXPECT_GE(ContrastRatio(WithAlpha(fg, alpha), bg), 3.0f);
    EXPECT_LT(ContrastRatio(WithAlpha(fg, alpha - 0.01f), bg), 3.0f);
    // Unreachable even when opaque: the caller gets full opacity.
    EXPECT_EQ(MinimumAlphaForContrast(Hex(0x202020), bg, 3.0f), 1.0f);
}

TEST(Gated_SparkUi_SdlDpiUsesRatioOrDisplayDpiNeverBoth)
{
    // HiDPI framebuffer (Wayland 2x): the backend already scales through DisplayFramebufferScale.
    EXPECT_EQ(SdlDpiScale(2.0f, 192.0f), 1.0f);
    // Unscaled framebuffer: display DPI / 96.
    EXPECT_EQ(SdlDpiScale(1.0f, 144.0f), 1.5f);
    EXPECT_EQ(SdlDpiScale(1.0f, 96.0f), 1.0f);
    // No information falls back to 1.
    EXPECT_EQ(SdlDpiScale(1.0f, 0.0f), 1.0f);
    EXPECT_EQ(SdlDpiScale(std::numeric_limits<float>::quiet_NaN(), 120.0f), 1.25f);
    EXPECT_EQ(SdlDpiScale(1.0f, std::numeric_limits<float>::infinity()), 1.0f);
}

TEST(Gated_SparkUi_TokenColourScienceReferenceValues)
{
    EXPECT_NEAR(ContrastRatio(Hex(0xffffff), Hex(0x000000)), 21.0f, 0.01f);
    EXPECT_NEAR(ContrastRatio(Hex(0x777777), Hex(0xffffff)), 4.48f, 0.01f);
    const OkLab white = ToOkLab(Hex(0xffffff));
    EXPECT_NEAR(white.L, 1.0f, 0.001f);
    EXPECT_NEAR(white.a, 0.0f, 0.001f);
    EXPECT_NEAR(white.b, 0.0f, 0.001f);
    const Rgba roundTrip = FromOkLab(ToOkLab(Hex(0xf1823a)));
    EXPECT_NEAR(roundTrip.r, Hex(0xf1823a).r, 0.001f);
    EXPECT_NEAR(roundTrip.g, Hex(0xf1823a).g, 0.001f);
    EXPECT_NEAR(roundTrip.b, Hex(0xf1823a).b, 0.001f);
    // Protanopia collapses red and green far more than it collapses blue and yellow.
    const float redGreen =
        DeltaE(SimulateCvdLab(Hex(0xff0000), Cvd::Protan), SimulateCvdLab(Hex(0x00ff00), Cvd::Protan));
    const float redGreenNormal = DeltaE(ToOkLab(Hex(0xff0000)), ToOkLab(Hex(0x00ff00)));
    EXPECT_LT(redGreen, redGreenNormal);

    bool converged = false;
    const std::array backgrounds = {Hex(0x1a1716)};
    const Rgba lifted = EnsureContrast(Hex(0x5f5a55), backgrounds, 4.5f, &converged);
    EXPECT_TRUE(converged);
    EXPECT_GE(ContrastRatio(lifted, Hex(0x1a1716)), 4.5f);
}

#ifdef SPARK_TEST_HAS_IMGUI

#ifndef SPARK_HAS_IMGUI
#define SPARK_HAS_IMGUI 1
#endif

#include "SparkUI/Brand.h"
#include "SparkUI/Fonts.h"
#include "SparkUI/Runtime.h"
#include "SparkUI/Style.h"
#include "SparkUI/Widgets.h"

#include <Spark/UiTheme.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <system_error>

namespace
{
    namespace fs = std::filesystem;

    /// Creates an ImGui context, makes it current and restores the previous one on destruction.
    class ScopedImGuiContext
    {
      public:
        ScopedImGuiContext() : m_previous(ImGui::GetCurrentContext()), m_context(ImGui::CreateContext())
        {
            // CreateContext only becomes current when none was; nested scopes must switch explicitly.
            ImGui::SetCurrentContext(m_context);
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2(1280.0f, 720.0f);
        }

        ~ScopedImGuiContext()
        {
            ImGui::DestroyContext(m_context);
            ImGui::SetCurrentContext(m_previous);
        }

        ScopedImGuiContext(const ScopedImGuiContext&) = delete;
        ScopedImGuiContext& operator=(const ScopedImGuiContext&) = delete;

        static void BuildAtlas()
        {
            unsigned char* pixels = nullptr;
            int width = 0;
            int height = 0;
            ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        }

        [[nodiscard]] ImGuiContext* Get() const noexcept { return m_context; }

      private:
        ImGuiContext* m_previous;
        ImGuiContext* m_context;
    };

    class ScopedTempDirectory
    {
      public:
        explicit ScopedTempDirectory(std::string_view stem)
        {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            m_path = fs::temp_directory_path() / (std::string(stem) + "-" + std::to_string(stamp));
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

    fs::path SourceFontDirectory()
    {
        return fs::path(SPARK_TEST_SOURCE_DIR) / "SparkEditor" / "Fonts";
    }

    bool SameVec(ImVec2 a, ImVec2 b)
    {
        return a.x == b.x && a.y == b.y;
    }

    bool SameStyle(const ImGuiStyle& a, const ImGuiStyle& b)
    {
        bool same = SameVec(a.WindowPadding, b.WindowPadding) && SameVec(a.FramePadding, b.FramePadding) &&
                    SameVec(a.ItemSpacing, b.ItemSpacing) && SameVec(a.ItemInnerSpacing, b.ItemInnerSpacing) &&
                    SameVec(a.CellPadding, b.CellPadding) && SameVec(a.WindowMinSize, b.WindowMinSize) &&
                    SameVec(a.TouchExtraPadding, b.TouchExtraPadding) &&
                    SameVec(a.SeparatorTextPadding, b.SeparatorTextPadding) && a.IndentSpacing == b.IndentSpacing &&
                    a.ScrollbarSize == b.ScrollbarSize && a.GrabMinSize == b.GrabMinSize &&
                    a.WindowRounding == b.WindowRounding && a.ChildRounding == b.ChildRounding &&
                    a.FrameRounding == b.FrameRounding && a.PopupRounding == b.PopupRounding &&
                    a.ScrollbarRounding == b.ScrollbarRounding && a.GrabRounding == b.GrabRounding &&
                    a.TabRounding == b.TabRounding && a.WindowBorderSize == b.WindowBorderSize &&
                    a.ChildBorderSize == b.ChildBorderSize && a.PopupBorderSize == b.PopupBorderSize &&
                    a.FrameBorderSize == b.FrameBorderSize && a.TabBorderSize == b.TabBorderSize &&
                    a.TabBarBorderSize == b.TabBarBorderSize && a.TabBarOverlineSize == b.TabBarOverlineSize &&
                    a.SeparatorTextBorderSize == b.SeparatorTextBorderSize &&
                    a.DockingSeparatorSize == b.DockingSeparatorSize && a.ColumnsMinSpacing == b.ColumnsMinSpacing &&
                    a.MouseCursorScale == b.MouseCursorScale && a.FontSizeBase == b.FontSizeBase &&
                    a.FontScaleMain == b.FontScaleMain && a.FontScaleDpi == b.FontScaleDpi &&
                    a._MainScale == b._MainScale;
        for (int i = 0; i < ImGuiCol_COUNT; ++i)
        {
            const ImVec4& x = a.Colors[i];
            const ImVec4& y = b.Colors[i];
            same = same && x.x == y.x && x.y == y.y && x.z == y.z && x.w == y.w;
        }
        return same;
    }

    Tokens EmberDark()
    {
        return GenerateTokens(BuiltinSeed(PaletteId::Ember), Appearance::Dark);
    }
} // namespace

TEST(Gated_SparkUi_ApplyStyleIsIdempotent)
{
    const Tokens tokens = EmberDark();
    ImGuiStyle once;
    ApplyStyle(tokens, 1.5f, once);
    ImGuiStyle twice = once;
    ApplyStyle(tokens, 1.5f, twice);
    EXPECT_TRUE(SameStyle(once, twice));

    // Absolute: floor(token x scale) for metrics, max(1, round(scale)) for hairlines.
    EXPECT_EQ(once.FramePadding.x, 12.0f);
    EXPECT_EQ(once.FramePadding.y, 7.0f);
    EXPECT_EQ(once.FrameBorderSize, 2.0f);
    EXPECT_EQ(once.PopupRounding, 7.0f);
    EXPECT_EQ(once.FontSizeBase, tokens.type.body);

    // Going back to 1.0 from a scaled style equals applying 1.0 to a fresh one.
    ImGuiStyle fresh;
    ApplyStyle(tokens, 1.0f, fresh);
    ApplyStyle(tokens, 1.0f, twice);
    EXPECT_TRUE(SameStyle(fresh, twice));
    EXPECT_EQ(fresh.FramePadding.y, 5.0f);
    EXPECT_EQ(fresh.FrameBorderSize, 1.0f);

    // Hairlines never vanish below 100 %.
    ImGuiStyle small;
    ApplyStyle(tokens, 0.75f, small);
    EXPECT_EQ(small.FrameBorderSize, 1.0f);
    EXPECT_EQ(small.PopupBorderSize, 1.0f);
}

TEST(Gated_SparkUi_ClampScaleBoundsEffectiveScale)
{
    EXPECT_EQ(ClampScale(1.0f, 1.0f), 1.0f);
    EXPECT_EQ(ClampScale(2.0f, 2.0f), 3.0f);
    EXPECT_EQ(ClampScale(0.5f, 1.0f), 0.75f);
    EXPECT_EQ(ClampScale(std::nanf(""), 1.25f), 1.25f);
    EXPECT_EQ(ClampScale(1.5f, -1.0f), 1.5f);
}

TEST(Gated_SparkUi_LoadFontSetMissingFilesFallsBack)
{
    ScopedImGuiContext context;
    ScopedTempDirectory partial("spark-ui-fonts-partial");
    fs::copy_file(SourceFontDirectory() / "Roboto-Regular.ttf", partial.Path() / "Roboto-Regular.ttf");

    const FontSet robotoOnly = LoadFontSet(*ImGui::GetIO().Fonts, partial.Path(), 15.0f);
    ASSERT_TRUE(robotoOnly.body != nullptr);
    EXPECT_TRUE(robotoOnly.custom);
    EXPECT_TRUE(robotoOnly.strong == robotoOnly.body);
    EXPECT_TRUE(robotoOnly.bold == robotoOnly.body);
    EXPECT_TRUE(robotoOnly.mono == robotoOnly.body);
    EXPECT_FALSE(robotoOnly.diagnostic.empty());

    ScopedTempDirectory empty("spark-ui-fonts-empty");
    const FontSet embedded = LoadFontSet(*ImGui::GetIO().Fonts, empty.Path(), 15.0f);
    ASSERT_TRUE(embedded.body != nullptr);
    EXPECT_FALSE(embedded.custom);
    EXPECT_TRUE(embedded.mono == embedded.body);

    const FontSet missing = LoadFontSet(*ImGui::GetIO().Fonts, partial.Path() / "does-not-exist", 15.0f);
    ASSERT_TRUE(missing.body != nullptr);
    EXPECT_FALSE(missing.custom);

    ScopedImGuiContext::BuildAtlas();
    EXPECT_GE(ImGui::GetIO().Fonts->Fonts.Size, 3);
}

TEST(Gated_SparkUi_FindFontByConfigName)
{
    ScopedImGuiContext context;
    const FontSet set = LoadFontSet(*ImGui::GetIO().Fonts, SourceFontDirectory(), 15.0f);
    ScopedImGuiContext::BuildAtlas();
    ASSERT_TRUE(set.body != nullptr && set.strong != nullptr && set.bold != nullptr && set.mono != nullptr);
    EXPECT_TRUE(set.custom);
    EXPECT_TRUE(set.diagnostic.empty());
    EXPECT_TRUE(set.strong != set.body && set.bold != set.body && set.mono != set.body);

    EXPECT_TRUE(FindFont(FontRole::Small) == set.body);
    EXPECT_TRUE(FindFont(FontRole::Body) == set.body);
    EXPECT_TRUE(FindFont(FontRole::Strong) == set.strong);
    EXPECT_TRUE(FindFont(FontRole::H2) == set.strong);
    EXPECT_TRUE(FindFont(FontRole::H1) == set.strong);
    EXPECT_TRUE(FindFont(FontRole::Display) == set.bold);
    EXPECT_TRUE(FindFont(FontRole::Mono) == set.mono);
    EXPECT_TRUE(FindFont(FontRole::MonoSmall) == set.mono);
    EXPECT_TRUE(Sdk::FindFont(kFontNameMono) == set.mono);
    EXPECT_TRUE(Sdk::HudFontBold() == set.bold);
    EXPECT_EQ(FontSize(FontRole::Display), 26.0f);

    const std::array<fs::path, 1> probes = {SourceFontDirectory()};
    EXPECT_TRUE(FindFontDirectory(fs::path(), probes) == SourceFontDirectory());
    EXPECT_TRUE(FindFontDirectory(fs::path()).empty());
}

TEST(Gated_SparkUi_LoadFontSetUnicodeDirectory)
{
    // A non-ASCII install folder must load: LoadFontSet hands ImGui UTF-8, and a path(std::string) or
    // path::string() round trip would go through the ANSI code page on Windows and lose these characters.
    ScopedImGuiContext context;
    ScopedTempDirectory root("spark-ui-fonts-unicode");
    const fs::path fontDir = root.Path() / fs::path(u8"Schrift-éß-字体");
    fs::create_directories(fontDir);
    std::size_t staged = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(SourceFontDirectory()))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".ttf")
        {
            fs::copy_file(entry.path(), fontDir / entry.path().filename());
            ++staged;
        }
    }
    ASSERT_TRUE(staged >= 4);

    const std::array<fs::path, 1> probes = {fontDir};
    EXPECT_TRUE(FindFontDirectory(fs::path(), probes) == fontDir);

    const FontSet set = LoadFontSet(*ImGui::GetIO().Fonts, fontDir, 15.0f);
    ScopedImGuiContext::BuildAtlas();
    ASSERT_TRUE(set.body != nullptr && set.strong != nullptr && set.bold != nullptr && set.mono != nullptr);
    EXPECT_TRUE(set.custom);
    EXPECT_TRUE(set.diagnostic.empty());
    EXPECT_TRUE(set.strong != set.body && set.bold != set.body && set.mono != set.body);
}

TEST(Gated_SparkUi_PropertyGridStacksWhenNarrow)
{
    ScopedImGuiContext context;
    ScopedImGuiContext::BuildAtlas();
    Install(context.Get(), EmberDark(), 1.0f, 1.0f, false);

    struct Geometry
    {
        ImVec2 labelMin;
        ImVec2 labelMax;
        ImVec2 controlMin;
    };
    const auto layout = [](const char* window, float width)
    {
        Geometry geometry{};
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(width, 300.0f));
        ImGui::Begin(window, nullptr, ImGuiWindowFlags_NoSavedSettings);
        if (BeginPropertyGrid("grid"))
        {
            static char name[32] = "Crate";
            PropertyLabel("Name");
            geometry.labelMin = ImGui::GetItemRectMin();
            geometry.labelMax = ImGui::GetItemRectMax();
            ImGui::InputText("##name", name, sizeof(name));
            geometry.controlMin = ImGui::GetItemRectMin();
            EndPropertyGrid();
        }
        ImGui::End();
        return geometry;
    };

    ImGui::NewFrame();
    const float stackWidth = Em(18.0f);
    const Geometry narrow = layout("Narrow inspector", stackWidth * 0.6f);
    const Geometry wide = layout("Wide inspector", stackWidth * 2.5f);
    ImGui::EndFrame();

    EXPECT_GE(narrow.controlMin.y, narrow.labelMax.y);
    EXPECT_NEAR(narrow.controlMin.x, narrow.labelMin.x, 0.5f);
    EXPECT_LT(wide.controlMin.y, wide.labelMax.y);
    EXPECT_GT(wide.controlMin.x, wide.labelMax.x);
}

TEST(Gated_SparkUi_ScaleChangesApplyAtFrameStart)
{
    ScopedImGuiContext context;
    Install(context.Get(), EmberDark(), 1.0f, 1.0f, false);
    EXPECT_EQ(Scale(), 1.0f);
    EXPECT_EQ(ImGui::GetStyle().FramePadding.y, 5.0f);
    EXPECT_FALSE(ImGui::GetIO().ConfigDpiScaleFonts);

    RequestScale(1.5f, 1.0f);
    EXPECT_EQ(Scale(), 1.0f);
    EXPECT_TRUE(BeginFrameScale());
    EXPECT_EQ(Scale(), 1.5f);
    EXPECT_EQ(ImGui::GetStyle().FramePadding.y, 7.0f);
    EXPECT_EQ(ImGui::GetStyle().FontScaleDpi, 1.5f);
    EXPECT_EQ(ImGui::GetStyle().FontScaleMain, 1.0f);
    EXPECT_EQ(Px(8.0f), 12.0f);
    EXPECT_FALSE(BeginFrameScale());

    // The user factor multiplies the DPI factor and the product is clamped.
    RequestScale(2.0f, 2.0f);
    EXPECT_TRUE(BeginFrameScale());
    EXPECT_EQ(Scale(), 3.0f);
    EXPECT_EQ(ImGui::GetStyle().FontScaleDpi * ImGui::GetStyle().FontScaleMain, 3.0f);
}

TEST(Gated_SparkUi_RuntimeStateIsPerContext)
{
    ScopedImGuiContext first;
    Install(first.Get(), GenerateTokens(BuiltinSeed(PaletteId::Ember), Appearance::Light), 1.0f, 1.0f, true);
    {
        ScopedImGuiContext second;
        Install(second.Get(), EmberDark(), 2.0f, 1.0f, false);
        EXPECT_TRUE(ActiveTokens().appearance == Appearance::Dark);
        EXPECT_EQ(Scale(), 2.0f);
        EXPECT_FALSE(ReduceMotion());
    }
    // The second context's destruction freed only its own state.
    ImGui::SetCurrentContext(first.Get());
    EXPECT_TRUE(ActiveTokens().appearance == Appearance::Light);
    EXPECT_EQ(Scale(), 1.0f);
    EXPECT_TRUE(ReduceMotion());
    EXPECT_TRUE(StatusTextRole(Status::Error) == Role::ErrorText);
    EXPECT_EQ(std::string(StatusWord(Status::Neutral)), std::string("Note"));
}

TEST(Gated_SparkUi_RuntimeBlockRoundTrip)
{
    ScopedImGuiContext context;
    const Tokens tokens = EmberDark();
    Install(context.Get(), tokens, 1.25f, 1.0f, true);

    Sdk::SparkUiRuntimeBlock block;
    FillSdkRuntimeBlock(block);
    EXPECT_EQ(block.size, static_cast<std::uint16_t>(sizeof(Sdk::SparkUiRuntimeBlock)));
    EXPECT_EQ(block.scale, 1.25f);
    EXPECT_EQ(block.fontBody, tokens.type.body);
    EXPECT_EQ(block.reduceMotion, std::uint8_t{1});
    EXPECT_EQ(block.highContrast, std::uint8_t{0});

    ImGuiIO& io = ImGui::GetIO();
    EXPECT_TRUE(Sdk::Runtime() == nullptr);
    io.UserData = &block;
    EXPECT_TRUE(Sdk::Runtime() == &block);
    const ImU32 success = ImGui::ColorConvertFloat4ToU32(ToImVec4(Get(tokens, Role::SuccessText)));
    const ImU32 accent = ImGui::ColorConvertFloat4ToU32(ToImVec4(Get(tokens, Role::AccentFill)));
    EXPECT_EQ(Sdk::Color(Sdk::UiRole::Success, 0u), success);
    EXPECT_EQ(Sdk::Color(Sdk::UiRole::Accent, 0u), accent);

    block.version = static_cast<std::uint16_t>(Sdk::kRuntimeBlockVersion + 1);
    EXPECT_TRUE(Sdk::Runtime() == nullptr);
    EXPECT_EQ(Sdk::Color(Sdk::UiRole::Success, 0x12345678u), 0x12345678u);
    block.version = Sdk::kRuntimeBlockVersion;
    block.size = static_cast<std::uint16_t>(block.size - 1);
    EXPECT_TRUE(Sdk::Runtime() == nullptr);
    block.size = static_cast<std::uint16_t>(sizeof(Sdk::SparkUiRuntimeBlock));
    block.magic = 0;
    EXPECT_TRUE(Sdk::Runtime() == nullptr);
    io.UserData = nullptr;
}

TEST(Gated_SparkUi_BrandBandInkReadsInEveryAppearance)
{
    for (const Appearance appearance : kAppearances)
    {
        const Tokens tokens = EmberTokens(appearance);
        const BrandBandStyle band = ChooseBrandBandStyle(tokens);
        EXPECT_GE(band.worstContrast, TextContrastMinimum(appearance));
        if (band.gradient)
        {
            for (const std::uint32_t stop : kBrandGradientStops)
            {
                EXPECT_GE(ContrastRatio(Get(tokens, band.ink), Hex(stop)), TextContrastMinimum(appearance));
            }
        }
        else
        {
            EXPECT_GE(ContrastRatio(Get(tokens, band.ink), Get(tokens, Role::SurfaceRaised)),
                      TextContrastMinimum(appearance));
        }
    }
    // Light no longer puts white on the gradient; High Contrast (7:1) cannot use the gradient at all.
    EXPECT_TRUE(ChooseBrandBandStyle(EmberTokens(Appearance::Light)).ink == Role::TextPrimary);
    EXPECT_TRUE(ChooseBrandBandStyle(EmberTokens(Appearance::Dark)).gradient);
    EXPECT_FALSE(ChooseBrandBandStyle(EmberTokens(Appearance::HighContrast)).gradient);
}

TEST(Gated_SparkUi_FocusRingUsesTokenWidthInEveryHost)
{
    ScopedImGuiContext context;
    ScopedImGuiContext::BuildAtlas();
    Install(context.Get(), EmberTokens(Appearance::HighContrast), 1.0f, 1.0f, false);
    EXPECT_EQ(ActiveTokens().metrics.focusRing, 3.0f);
    InstallFocusRing(context.Get());
    InstallFocusRing(context.Get());
    const ImGuiID owner = ImHashStr("SparkUi.FocusRing");
    int hooks = 0;
    for (const ImGuiContextHook& hook : context.Get()->Hooks)
    {
        hooks += hook.Owner == owner ? 1 : 0;
    }
    EXPECT_EQ(hooks, 3); // installed once: NewFramePost, EndFramePre, Shutdown

    const ImVec4 focusColour = ImGui::GetStyleColorVec4(ImGuiCol_NavCursor);
    EXPECT_GT(focusColour.w, 0.0f);

    ImGuiContext& g = *context.Get();
    ImGuiID buttonId = 0;
    // Returns the focus window's vertex count after the frame.
    const auto frame = [&](bool focusFirst, bool cursorVisible)
    {
        ImGui::NewFrame();
        // ImGui's own 2 px cursor is hidden for the frame; the hook draws the token ring instead.
        EXPECT_EQ(ImGui::GetStyleColorVec4(ImGuiCol_NavCursor).w, 0.0f);
        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f));
        ImGui::SetNextWindowSize(ImVec2(300.0f, 200.0f));
        ImGui::Begin("Focus ring host", nullptr, ImGuiWindowFlags_NoSavedSettings);
        if (focusFirst)
        {
            ImGui::SetKeyboardFocusHere();
        }
        g.NavCursorVisible = cursorVisible;
        ImGui::Button("Focusable");
        buttonId = ImGui::GetItemID();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImGui::End();
        ImGui::EndFrame();
        return drawList->VtxBuffer.Size;
    };

    frame(true, true);
    const int withRing = frame(false, true);
    EXPECT_EQ(g.NavId, buttonId);
    const int withoutRing = frame(false, false);
    EXPECT_GT(withRing, withoutRing);

    // The colour is back after every frame, so themes and readers outside the frame see it.
    const ImVec4 restored = ImGui::GetStyleColorVec4(ImGuiCol_NavCursor);
    EXPECT_EQ(restored.w, focusColour.w);
    EXPECT_EQ(restored.x, focusColour.x);
}

TEST(Gated_SparkUi_SdkFallbackColoursMatchEmberDark)
{
    const Tokens tokens = EmberDark();
    const auto packed = [&](Role role) { return ImGui::ColorConvertFloat4ToU32(ToImVec4(Get(tokens, role))); };
    EXPECT_EQ(Sdk::kFallbackSuccess, packed(Role::SuccessText));
    EXPECT_EQ(Sdk::kFallbackWarning, packed(Role::WarningText));
    EXPECT_EQ(Sdk::kFallbackError, packed(Role::ErrorText));
    EXPECT_EQ(Sdk::kFallbackTextOnAccent, packed(Role::TextOnAccent));
}

TEST(Gated_SparkUi_HeroBannerCaptionReadsInEveryAppearance)
{
    for (const PaletteId family : kFamilies)
    {
        for (const Appearance appearance : kAppearances)
        {
            const Tokens tokens = GenerateTokens(BuiltinSeed(family), appearance);
            const HeroBannerStyle style = ChooseHeroBannerStyle(tokens);
            EXPECT_GE(style.captionContrast, kHeroCaptionContrastMinimum);
            // Recompute against every colour the banner can show under the text, independently of
            // the value the chooser reports.
            const Rgba raised = Get(tokens, Role::SurfaceRaised);
            std::vector<Rgba> banner;
            if (style.gradient)
            {
                for (const std::uint32_t stop : kBrandGradientStops)
                {
                    banner.push_back(Composite(WithAlpha(Hex(stop), style.washAlpha), raised));
                }
            }
            else
            {
                banner.push_back(raised);
            }
            for (const Rgba& colour : banner)
            {
                EXPECT_GE(ContrastRatio(Get(tokens, style.caption), colour), kHeroCaptionContrastMinimum);
                EXPECT_GE(ContrastRatio(Get(tokens, style.title), colour), TextContrastMinimum(appearance));
            }
            // High Contrast draws a flat, bordered surface instead of the brand wash.
            EXPECT_EQ(style.gradient, appearance != Appearance::HighContrast);
        }
    }
    // The Light version line measured 4.37:1 once rendered in the thin mono face; it now uses primary ink.
    EXPECT_TRUE(ChooseHeroBannerStyle(EmberTokens(Appearance::Light)).caption == Role::TextPrimary);
}

TEST(Gated_SparkUi_FitTextToWidthKeepsEndsAndUtf8)
{
    // One unit per code point, so the expected cuts are exact.
    const auto codePoints = [](std::string_view text)
    {
        float count = 0.0f;
        for (const char c : text)
        {
            count += (static_cast<unsigned char>(c) & 0xC0u) != 0x80u ? 1.0f : 0.0f;
        }
        return count;
    };

    EXPECT_EQ(FitTextToWidth("abcdefghij", 10.0f, codePoints, EllipsisPosition::Middle, "~"),
              std::string("abcdefghij"));
    EXPECT_EQ(FitTextToWidth("abcdefghij", 7.0f, codePoints, EllipsisPosition::Middle, "~"), std::string("abc~hij"));
    EXPECT_EQ(FitTextToWidth("abcdefghij", 7.0f, codePoints, EllipsisPosition::Leading, "~"), std::string("~efghij"));
    EXPECT_EQ(FitTextToWidth("abcdefghij", 0.5f, codePoints, EllipsisPosition::Middle, "~"), std::string());

    // A path keeps its file name when the folder is cut from the front.
    const std::string path = "D:/Projects/Some/Very/Deep/Folder/Levels/Arena.scene";
    const std::string fitted = FitTextToWidth(path, 20.0f, codePoints, EllipsisPosition::Leading, "~");
    EXPECT_EQ(fitted.size(), std::size_t{20});
    EXPECT_TRUE(fitted.ends_with("Levels/Arena.scene"));

    // Multi-byte text is only cut between code points: alpha..theta (8 code points, 16 bytes).
    const std::string greek = "\xce\xb1\xce\xb2\xce\xb3\xce\xb4\xce\xb5\xce\xb6\xce\xb7\xce\xb8";
    const std::string ellipsis = "\xe2\x80\xa6";
    EXPECT_EQ(FitTextToWidth(greek, 5.0f, codePoints, EllipsisPosition::Middle, ellipsis),
              std::string("\xce\xb1\xce\xb2") + ellipsis + "\xce\xb7\xce\xb8");
}

TEST(Gated_SparkUi_SliderValueSitsBesideTrack)
{
    ScopedImGuiContext context;
    ScopedImGuiContext::BuildAtlas();
    Install(context.Get(), EmberDark(), 1.0f, 1.0f, false);

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(600.0f, 200.0f));
    ImGui::Begin("Slider host", nullptr, ImGuiWindowFlags_NoSavedSettings);
    const float trackWidth = Em(6.0f);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    float size = 64.0f;
    EXPECT_FALSE(SliderWithValue("##Size", &size, 32.0f, 128.0f, "%.0f px", trackWidth, "Thumbnail Size"));
    // The last item is the value slot; it starts after the track instead of sharing its rectangle.
    const ImVec2 slotMin = ImGui::GetItemRectMin();
    const ImVec2 slotMax = ImGui::GetItemRectMax();
    ImGui::End();
    ImGui::EndFrame();

    EXPECT_GE(slotMin.x, start.x + trackWidth);
    EXPECT_NEAR(slotMin.y, start.y, 0.5f);
    EXPECT_GT(slotMax.x - slotMin.x, 0.0f);
    EXPECT_EQ(size, 64.0f); // an untouched slider leaves the value alone
}

#endif // SPARK_TEST_HAS_IMGUI
