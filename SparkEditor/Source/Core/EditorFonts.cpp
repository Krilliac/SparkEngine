/**
 * @file EditorFonts.cpp
 * @brief Editor font loading over Spark::Ui::LoadFontSet.
 * @author Spark Engine Team
 * @date 2025
 */

#include "EditorFonts.h"
#include "../Utils/EditorLaunchContext.h"
#include "../Utils/EditorProcessLaunch.h"
#include "Utils/Validate.h"
#include "SparkUI/Fonts.h"
#include <array>
#include <filesystem>
#include <iostream>

namespace SparkEditor
{

    ImFont* EditorFonts::s_defaultFont = nullptr;
    ImFont* EditorFonts::s_boldFont = nullptr;
    ImFont* EditorFonts::s_monoFont = nullptr;
    ImFont* EditorFonts::s_largeFont = nullptr;
    bool EditorFonts::s_customFontsLoaded = false;

    bool EditorFonts::LoadFonts(float baseFontSize)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Loading editor fonts (baseFontSize=%.1f)", baseFontSize);
        SPARK_WARN_IF(Spark::LogCategory::Editor, baseFontSize <= 0.0f, "Font size is non-positive");
        ImGuiIO& io = ImGui::GetIO();

        // exeDir/EditorAssets/Fonts is searched first, so launching the editor from any working
        // directory finds the staged fonts; the relative probes cover in-tree runs. The executable
        // directory is UTF-8, so it is widened with u8path (a plain path(std::string) would use the
        // ANSI code page on Windows and mangle non-ASCII install folders).
        namespace fs = std::filesystem;
        const std::array<fs::path, 4> legacyProbes = {
            fs::path("EditorAssets") / "Fonts",
            fs::path("Fonts"),
            fs::path("../SparkEditor/Fonts"),
            fs::path("../Fonts"),
        };
        const fs::path fontDir =
            Spark::Ui::FindFontDirectory(LaunchContext::PathFromUtf8(GetEditorExecutableDirectory()), legacyProbes);
        if (fontDir.empty())
        {
            std::cout << "[EditorFonts] Custom fonts not found, using ImGui default font\n";
        }
        else
        {
            std::cout << "[EditorFonts] Loading fonts from: " << LaunchContext::PathToUtf8(fontDir) << "\n";
        }

        const Spark::Ui::FontSet fonts = Spark::Ui::LoadFontSet(*io.Fonts, fontDir, baseFontSize);
        s_defaultFont = fonts.body;
        s_boldFont = fonts.strong;
        s_monoFont = fonts.mono;
        s_largeFont = fonts.bold;
        s_customFontsLoaded = fonts.custom;
        if (!fonts.diagnostic.empty())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Editor font substitutions: %s", fonts.diagnostic.c_str());
        }
        std::cout << "[EditorFonts] Fonts loaded (custom=" << s_customFontsLoaded << ")\n";
        return s_customFontsLoaded;
    }

} // namespace SparkEditor
