/**
 * @file Fonts.h
 * @brief Font discovery, loading (one atlas, four faces plus merged icons) and role lookup.
 *
 * Thread affinity: UI thread only. LoadFontSet mutates the atlas it is given; FindFont,
 * FontSize and ScopedFont act on the current ImGui context. Ownership: the atlas owns every
 * ImFont; FontSet only borrows the pointers, which stay valid until the atlas is cleared.
 */

#pragma once

#include "SparkUI/Enums.h"

#include <filesystem>
#include <span>
#include <string>

#include <imgui.h>

namespace Spark::Ui
{
    /// @brief ImFontConfig::Name of the body face (Small and Body roles).
    inline constexpr const char* kFontNameBody = "Spark.Body";
    /// @brief ImFontConfig::Name of the semibold face (Strong, H2, H1).
    inline constexpr const char* kFontNameStrong = "Spark.Strong";
    /// @brief ImFontConfig::Name of the bold face (Display).
    inline constexpr const char* kFontNameBold = "Spark.Bold";
    /// @brief ImFontConfig::Name of the monospace face (Mono, MonoSmall).
    inline constexpr const char* kFontNameMono = "Spark.Mono";

    /// @brief Faces loaded by LoadFontSet. Every pointer is non-null after a successful load.
    struct FontSet
    {
        ImFont* body = nullptr;
        ImFont* strong = nullptr;
        ImFont* bold = nullptr;
        ImFont* mono = nullptr;
        bool custom = false;    ///< True when the body face came from a font file (not the embedded default).
        std::string diagnostic; ///< Human-readable list of substitutions; empty when every face loaded.
    };

    /// @brief Locate the font directory: exeDir/EditorAssets/Fonts, then each of @p legacyProbes.
    /// @return The first existing directory, or an empty path when none exists.
    std::filesystem::path FindFontDirectory(const std::filesystem::path& exeDir,
                                            std::span<const std::filesystem::path> legacyProbes = {});

    /// @brief Load the Spark faces from @p fontDir into @p atlas at @p bodyPx.
    /// @details Every file is checked for existence before it is opened, so a missing face never
    ///          trips ImGui's load assertion. IBM Plex Sans falls back to Roboto; a missing SemiBold
    ///          or Bold reuses the next available face; an empty or missing directory loads the
    ///          embedded vector default font with custom = false. Font Awesome is merged into all
    ///          four faces at 0.92 x size with a +1 px vertical offset.
    FontSet LoadFontSet(ImFontAtlas& atlas, const std::filesystem::path& fontDir, float bodyPx);

    /// @brief Font for @p r on the current context, found by ImFontConfig name.
    /// @return The role's face, else the Spark body face, else the current font; nullptr only
    ///         when no context is current.
    ImFont* FindFont(FontRole r);

    /// @brief Logical (100 %) size of @p r from the active tokens' typography.
    float FontSize(FontRole r);

    /// @brief RAII PushFont/PopFont for a role.
    class ScopedFont
    {
      public:
        /// @brief Push the role's face at @p sizeLogical (0 = the role's size).
        explicit ScopedFont(FontRole r, float sizeLogical = 0.0f);
        /// @brief Pop the font pushed by the constructor.
        ~ScopedFont();
        ScopedFont(const ScopedFont&) = delete;
        ScopedFont& operator=(const ScopedFont&) = delete;
    };
} // namespace Spark::Ui
