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
#include "SparkUI/TokenAudit.h"
#include "SparkUI/Tokens.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using namespace Spark::Ui;

    constexpr std::array kFamilies = {PaletteId::Ember, PaletteId::Cobalt, PaletteId::Graphite};
    constexpr std::array kAppearances = {Appearance::Dark, Appearance::Light, Appearance::HighContrast};

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

#include "SparkUI/Fonts.h"
#include "SparkUI/Runtime.h"
#include "SparkUI/Style.h"
#include "SparkUI/Widgets.h"

#include <Spark/UiTheme.h>
#include <imgui.h>

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

TEST(Gated_SparkUi_SdkFallbackColoursMatchEmberDark)
{
    const Tokens tokens = EmberDark();
    const auto packed = [&](Role role) { return ImGui::ColorConvertFloat4ToU32(ToImVec4(Get(tokens, role))); };
    EXPECT_EQ(Sdk::kFallbackSuccess, packed(Role::SuccessText));
    EXPECT_EQ(Sdk::kFallbackWarning, packed(Role::WarningText));
    EXPECT_EQ(Sdk::kFallbackError, packed(Role::ErrorText));
    EXPECT_EQ(Sdk::kFallbackTextOnAccent, packed(Role::TextOnAccent));
}

#endif // SPARK_TEST_HAS_IMGUI
