/**
 * @file UiTheme.h
 * @brief Header-only accessor that lets game modules read the host's Spark UI theme (colours,
 *        scale, fonts) from the ImGui context the engine injects.
 *
 * The host (SparkEngine's GameImGuiLayer) owns a SparkUiRuntimeBlock and points
 * ImGui::GetIO().UserData at it; a module reads it through Runtime(). Every accessor is
 * fail-safe: a missing context, a null UserData, or a block whose magic, version or size does not
 * match yields the caller's fallback instead of undefined behaviour.
 *
 * Contract: header-only, no exported symbols, no part of the module binary ABI (SPARK_SDK_VERSION
 * is unchanged). Thread affinity: the thread that owns the current ImGui context (the render
 * thread's ImGui pass). Ownership: the block is owned by the host and only read here.
 * Everything is compiled only when SPARK_HAS_IMGUI is defined.
 */

#pragma once

#ifdef SPARK_HAS_IMGUI

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <imgui.h>

namespace Spark::Ui::Sdk
{
    /// @brief Colours a module may ask the host for.
    enum class UiRole : std::uint8_t
    {
        Text,
        TextSecondary,
        Accent,
        TextOnAccent,
        Success,
        Warning,
        Error,
        Info,
        SurfaceOverlay,
        Count
    };

    /// @brief Block magic: the bytes 'S' 'P' 'U' 'I' read as a little-endian 32-bit value.
    inline constexpr std::uint32_t kRuntimeBlockMagic = 0x49555053u;
    /// @brief Current block layout version.
    inline constexpr std::uint16_t kRuntimeBlockVersion = 1;

    /// @brief Theme snapshot the host publishes through ImGuiIO::UserData.
    struct SparkUiRuntimeBlock
    {
        std::uint32_t magic = kRuntimeBlockMagic;
        std::uint16_t version = kRuntimeBlockVersion;
        std::uint16_t size = 0; ///< sizeof(SparkUiRuntimeBlock) as written by the host.
        std::uint32_t colors[static_cast<std::size_t>(UiRole::Count)] = {};
        float scale = 1.0f;     ///< Effective UI scale.
        float fontBody = 15.0f; ///< Logical body size in pixels.
        float fontSmall = 13.0f;
        float fontH1 = 20.0f;
        std::uint8_t reduceMotion = 0;
        std::uint8_t highContrast = 0;
        std::uint8_t reserved[2] = {};
    };
    static_assert(sizeof(SparkUiRuntimeBlock) == 64, "SparkUiRuntimeBlock layout is part of the host contract");

    /// @brief Ember Dark SuccessText, used when the host publishes no block.
    inline constexpr ImU32 kFallbackSuccess = IM_COL32(0x82, 0xFF, 0xD9, 255);
    /// @brief Ember Dark WarningText, used when the host publishes no block.
    inline constexpr ImU32 kFallbackWarning = IM_COL32(0xFF, 0xD8, 0x67, 255);
    /// @brief Ember Dark ErrorText, used when the host publishes no block.
    inline constexpr ImU32 kFallbackError = IM_COL32(0xFF, 0xAA, 0xB9, 255);
    /// @brief Ember Dark TextOnAccent, used on success-filled buttons when no block is published.
    inline constexpr ImU32 kFallbackTextOnAccent = IM_COL32(0x1A, 0x0F, 0x08, 255);

    /// @brief The host's block, or nullptr when absent or of another layout.
    inline const SparkUiRuntimeBlock* Runtime()
    {
        if (ImGui::GetCurrentContext() == nullptr)
        {
            return nullptr;
        }
        const auto* block = static_cast<const SparkUiRuntimeBlock*>(ImGui::GetIO().UserData);
        if (block == nullptr || block->magic != kRuntimeBlockMagic || block->version != kRuntimeBlockVersion ||
            block->size != sizeof(SparkUiRuntimeBlock))
        {
            return nullptr;
        }
        return block;
    }

    /// @brief Host colour for @p role, or @p fallback when there is no valid block.
    inline ImU32 Color(UiRole role, ImU32 fallback)
    {
        const SparkUiRuntimeBlock* block = Runtime();
        const auto index = static_cast<std::size_t>(role);
        if (block == nullptr || index >= static_cast<std::size_t>(UiRole::Count))
        {
            return fallback;
        }
        return block->colors[index];
    }

    /// @brief Font whose ImFontConfig name is @p configName ("Spark.Body", "Spark.Strong",
    ///        "Spark.Bold", "Spark.Mono"); the current font when none matches.
    /// @return nullptr only when no ImGui context is current.
    inline ImFont* FindFont(const char* configName)
    {
        if (ImGui::GetCurrentContext() == nullptr)
        {
            return nullptr;
        }
        ImFontAtlas* atlas = ImGui::GetIO().Fonts;
        if (atlas != nullptr && configName != nullptr)
        {
            for (ImFont* font : atlas->Fonts)
            {
                if (font != nullptr && font->Sources.Size > 0 && std::strcmp(font->Sources[0]->Name, configName) == 0)
                {
                    return font;
                }
            }
        }
        return ImGui::GetFont();
    }

    /// @brief HUD body face.
    inline ImFont* HudFont()
    {
        return FindFont("Spark.Body");
    }

    /// @brief HUD emphasis face.
    inline ImFont* HudFontBold()
    {
        return FindFont("Spark.Bold");
    }

    /// @brief Push success-tinted button colours (4 colours); pair with PopSuccessButton().
    inline void PushSuccessButton()
    {
        ImVec4 fill = ImGui::ColorConvertU32ToFloat4(Color(UiRole::Success, kFallbackSuccess));
        ImVec4 idle = fill;
        idle.w *= 0.85f;
        ImVec4 active = fill;
        active.w *= 0.70f;
        ImGui::PushStyleColor(ImGuiCol_Button, idle);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, fill);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
        ImGui::PushStyleColor(ImGuiCol_Text, Color(UiRole::TextOnAccent, kFallbackTextOnAccent));
    }

    /// @brief Pop the colours pushed by PushSuccessButton().
    inline void PopSuccessButton()
    {
        ImGui::PopStyleColor(4);
    }
} // namespace Spark::Ui::Sdk

#endif // SPARK_HAS_IMGUI
