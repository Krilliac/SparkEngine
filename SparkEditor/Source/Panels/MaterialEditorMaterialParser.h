/**
 * @file MaterialEditorMaterialParser.h
 * @brief Shared material schema and production text-parser entry for editor files and fuzzing.
 */
#pragma once
#include <cstddef>
#include <string_view>
namespace SparkEditor
{
    struct MaterialDefinition;
    inline constexpr std::size_t kMaxMaterialTextBytes = 16u * 1024u * 1024u;
    /// @brief Append the editor's standard PBR parameters and texture slots.
    void InitializeStandardPBRParameters(MaterialDefinition& material);
    /// @brief Parse into a temporary definition; rejection preserves output, acceptance preserves filePath.
    bool ParseMaterialText(std::string_view text, MaterialDefinition& material);
} // namespace SparkEditor
