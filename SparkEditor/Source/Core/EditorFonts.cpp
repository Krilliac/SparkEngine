/**
 * @file EditorFonts.cpp
 * @brief Font loading implementation for the Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 */

#include "EditorFonts.h"
#include "EditorIcons.h"
#include <iostream>
#include <filesystem>

namespace SparkEditor {

ImFont* EditorFonts::s_defaultFont = nullptr;
ImFont* EditorFonts::s_boldFont = nullptr;
ImFont* EditorFonts::s_monoFont = nullptr;
ImFont* EditorFonts::s_largeFont = nullptr;
bool EditorFonts::s_customFontsLoaded = false;

void EditorFonts::LoadFonts(float baseFontSize)
{
    ImGuiIO& io = ImGui::GetIO();

    // Font search paths (relative to executable)
    const char* fontPaths[] = {
        "EditorAssets/Fonts/",
        "Fonts/",
        "../SparkEditor/Fonts/",
        "../Fonts/",
    };

    std::string fontDir;
    for (const char* path : fontPaths) {
        if (std::filesystem::exists(std::string(path) + "Roboto-Regular.ttf")) {
            fontDir = path;
            break;
        }
    }

    if (fontDir.empty()) {
        std::cout << "[EditorFonts] Custom fonts not found, using ImGui default font\n";
        s_defaultFont = io.Fonts->AddFontDefault();
        return;
    }

    std::cout << "[EditorFonts] Loading fonts from: " << fontDir << "\n";

    // Icon font config (merged into each primary font)
    static const ImWchar iconRanges[] = { ICON_FA_MIN, ICON_FA_MAX, 0 };
    std::string iconFontPath = fontDir + "fa-solid-900.ttf";
    bool hasIconFont = std::filesystem::exists(iconFontPath);

    // --- Default font: Roboto Regular + FontAwesome ---
    ImFontConfig defaultConfig;
    defaultConfig.OversampleH = 2;
    defaultConfig.OversampleV = 1;
    defaultConfig.PixelSnapH = true;
    s_defaultFont = io.Fonts->AddFontFromFileTTF(
        (fontDir + "Roboto-Regular.ttf").c_str(), baseFontSize, &defaultConfig);

    if (s_defaultFont && hasIconFont) {
        ImFontConfig iconConfig;
        iconConfig.MergeMode = true;
        iconConfig.GlyphMinAdvanceX = baseFontSize;
        iconConfig.PixelSnapH = true;
        io.Fonts->AddFontFromFileTTF(iconFontPath.c_str(), baseFontSize - 1.0f, &iconConfig, iconRanges);
    }

    // --- Bold font ---
    std::string boldPath = fontDir + "Roboto-Bold.ttf";
    if (std::filesystem::exists(boldPath)) {
        ImFontConfig boldConfig;
        boldConfig.OversampleH = 2;
        s_boldFont = io.Fonts->AddFontFromFileTTF(boldPath.c_str(), baseFontSize, &boldConfig);
    }

    // --- Monospace font ---
    std::string monoPath = fontDir + "JetBrainsMono-Regular.ttf";
    if (std::filesystem::exists(monoPath)) {
        ImFontConfig monoConfig;
        monoConfig.OversampleH = 2;
        s_monoFont = io.Fonts->AddFontFromFileTTF(monoPath.c_str(), baseFontSize - 1.0f, &monoConfig);
    }

    // --- Large font (for titles/headers) ---
    ImFontConfig largeConfig;
    largeConfig.OversampleH = 2;
    s_largeFont = io.Fonts->AddFontFromFileTTF(
        (fontDir + "Roboto-Bold.ttf").c_str(), baseFontSize + 5.0f, &largeConfig);

    if (s_largeFont && hasIconFont) {
        ImFontConfig iconLargeConfig;
        iconLargeConfig.MergeMode = true;
        iconLargeConfig.GlyphMinAdvanceX = baseFontSize + 5.0f;
        iconLargeConfig.PixelSnapH = true;
        io.Fonts->AddFontFromFileTTF(iconFontPath.c_str(), baseFontSize + 4.0f, &iconLargeConfig, iconRanges);
    }

    s_customFontsLoaded = (s_defaultFont != nullptr);
    std::cout << "[EditorFonts] Fonts loaded successfully (custom=" << s_customFontsLoaded << ")\n";
}

} // namespace SparkEditor
