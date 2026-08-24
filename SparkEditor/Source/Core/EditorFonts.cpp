/**
 * @file EditorFonts.cpp
 * @brief Font loading implementation for the Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 */

#include "EditorFonts.h"
#include "EditorIcons.h"
#include "../Utils/EditorProcessLaunch.h"
#include "Utils/Validate.h"
#include <iostream>
#include <filesystem>
#include <vector>

#ifdef IMGUI_ENABLE_FREETYPE
#include "misc/freetype/imgui_freetype.h"
// Light hinting + LCD subpixel = crisp small UI text without pixel-grid stair-stepping.
#define SPARK_FT_FLAGS (ImGuiFreeTypeBuilderFlags_LightHinting | ImGuiFreeTypeBuilderFlags_LoadColor)
#else
#define SPARK_FT_FLAGS 0
#endif

namespace SparkEditor
{

    ImFont* EditorFonts::s_defaultFont = nullptr;
    ImFont* EditorFonts::s_boldFont = nullptr;
    ImFont* EditorFonts::s_monoFont = nullptr;
    ImFont* EditorFonts::s_largeFont = nullptr;
    bool EditorFonts::s_customFontsLoaded = false;

    void EditorFonts::LoadFonts(float baseFontSize)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Loading editor fonts (baseFontSize=%.1f)", baseFontSize);
        SPARK_WARN_IF(Spark::LogCategory::Editor, baseFontSize <= 0.0f, "Font size is non-positive");
        ImGuiIO& io = ImGui::GetIO();

        // Always prefer assets next to SparkEditor.exe. The previous Windows
        // path left this empty and depended on the process CWD, so launching
        // the editor from Explorer/Codex/etc. silently dropped FontAwesome.
        namespace fs = std::filesystem;
        const std::vector<fs::path> fontPaths = {
            fs::path(GetEditorExecutableDirectory()) / "EditorAssets" / "Fonts",
            fs::path("EditorAssets") / "Fonts", fs::path("Fonts"), fs::path("../SparkEditor/Fonts"),
            fs::path("../Fonts"),
        };

        fs::path fontDir;
        for (const fs::path& path : fontPaths)
        {
            if (fs::exists(path / "Roboto-Regular.ttf") || fs::exists(path / "IBMPlexSans-Regular.ttf"))
            {
                fontDir = path;
                break;
            }
        }

        if (fontDir.empty())
        {
            std::cout << "[EditorFonts] Custom fonts not found, using ImGui default font\n";
            s_defaultFont = io.Fonts->AddFontDefault();
            return;
        }

        std::cout << "[EditorFonts] Loading fonts from: " << fontDir.string() << "\n";

        // Prefer IBM Plex Sans (matches SparkEditor hi-fi design); fall back to Roboto.
        std::string regularPath = (fontDir / "IBMPlexSans-Regular.ttf").string();
        if (!std::filesystem::exists(regularPath))
            regularPath = (fontDir / "Roboto-Regular.ttf").string();

        std::string boldFacePath = (fontDir / "IBMPlexSans-SemiBold.ttf").string();
        if (!std::filesystem::exists(boldFacePath))
            boldFacePath = (fontDir / "IBMPlexSans-Bold.ttf").string();
        if (!std::filesystem::exists(boldFacePath))
            boldFacePath = (fontDir / "Roboto-Bold.ttf").string();

        // Icon font config (merged into each primary font)
        static const ImWchar iconRanges[] = {ICON_FA_MIN, ICON_FA_MAX, 0};
        std::string iconFontPath = (fontDir / "fa-solid-900.ttf").string();
        bool hasIconFont = std::filesystem::exists(iconFontPath);

        // --- Default font: IBM Plex Sans / Roboto + FontAwesome ---
        // OversampleH=3, V=2, PixelSnapH=false produces visibly smoother subpixel
        // text at small UI sizes (matches the hi-fi design's text clarity).
        ImFontConfig defaultConfig;
        defaultConfig.OversampleH = 3;
        defaultConfig.OversampleV = 2;
        defaultConfig.PixelSnapH = false;
        defaultConfig.RasterizerMultiply = 1.05f;
        defaultConfig.FontLoaderFlags = SPARK_FT_FLAGS;
        s_defaultFont = io.Fonts->AddFontFromFileTTF(regularPath.c_str(), baseFontSize, &defaultConfig);

        if (s_defaultFont && hasIconFont)
        {
            ImFontConfig iconConfig;
            iconConfig.MergeMode = true;
            iconConfig.GlyphMinAdvanceX = baseFontSize;
            iconConfig.PixelSnapH = true;
            iconConfig.OversampleH = 3;
            iconConfig.OversampleV = 2;
            iconConfig.FontLoaderFlags = SPARK_FT_FLAGS;
            io.Fonts->AddFontFromFileTTF(iconFontPath.c_str(), baseFontSize - 1.0f, &iconConfig, iconRanges);
        }

        // --- Bold font ---
        if (std::filesystem::exists(boldFacePath))
        {
            ImFontConfig boldConfig;
            boldConfig.OversampleH = 3;
            boldConfig.OversampleV = 2;
            boldConfig.PixelSnapH = false;
            boldConfig.FontLoaderFlags = SPARK_FT_FLAGS;
            s_boldFont = io.Fonts->AddFontFromFileTTF(boldFacePath.c_str(), baseFontSize, &boldConfig);
        }

        // --- Monospace font ---
        std::string monoPath = (fontDir / "JetBrainsMono-Regular.ttf").string();
        if (std::filesystem::exists(monoPath))
        {
            ImFontConfig monoConfig;
            monoConfig.OversampleH = 3;
            monoConfig.OversampleV = 2;
            monoConfig.PixelSnapH = false;
            monoConfig.FontLoaderFlags = SPARK_FT_FLAGS;
            s_monoFont = io.Fonts->AddFontFromFileTTF(monoPath.c_str(), baseFontSize - 1.0f, &monoConfig);
        }

        // --- Large font (for titles/headers) ---
        ImFontConfig largeConfig;
        largeConfig.OversampleH = 3;
        largeConfig.OversampleV = 2;
        largeConfig.PixelSnapH = false;
        largeConfig.FontLoaderFlags = SPARK_FT_FLAGS;
        s_largeFont = io.Fonts->AddFontFromFileTTF(boldFacePath.c_str(), baseFontSize + 5.0f, &largeConfig);

        if (s_largeFont && hasIconFont)
        {
            ImFontConfig iconLargeConfig;
            iconLargeConfig.MergeMode = true;
            iconLargeConfig.GlyphMinAdvanceX = baseFontSize + 5.0f;
            iconLargeConfig.PixelSnapH = true;
            iconLargeConfig.FontLoaderFlags = SPARK_FT_FLAGS;
            io.Fonts->AddFontFromFileTTF(iconFontPath.c_str(), baseFontSize + 4.0f, &iconLargeConfig, iconRanges);
        }

        s_customFontsLoaded = (s_defaultFont != nullptr);
        std::cout << "[EditorFonts] Fonts loaded successfully (custom=" << s_customFontsLoaded << ")\n";
    }

} // namespace SparkEditor
