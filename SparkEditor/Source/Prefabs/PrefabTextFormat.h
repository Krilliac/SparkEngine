/**
 * @file PrefabTextFormat.h
 * @brief Text grammar of `.sparkprefab` files: parse versions N-1..N, render version N
 *
 * Pure text in, text out, with no file I/O: PrefabAsset owns reading, durable writing and `.bak`
 * recovery. Stateless and safe to call from any thread.
 */

#pragma once

#include "PrefabAsset.h"

#include <string>
#include <vector>

namespace SparkEditor::PrefabTextFormat
{
    struct ParsedPrefab
    {
        std::string name;
        std::vector<SerializedComponent> components;
    };

    enum class ParseResult
    {
        Ok,
        Rejected,    ///< Damaged, malformed, or older than PrefabAsset::kOldestSupportedPrefabVersion
        NewerVersion ///< Written by a newer format version; must fail closed
    };

    /**
     * @brief Parse a prefab of any version from kOldestSupportedPrefabVersion to kPrefabFormatVersion
     * @param reason On failure, an actionable reason naming the line, component and property
     */
    ParseResult Parse(const std::string& text, ParsedPrefab& out, std::string& reason);

    /**
     * @brief Render a kPrefabFormatVersion document
     *
     * Properties are sorted by name, so re-saving an unchanged prefab produces identical bytes.
     * Every name must be non-empty; PrefabAsset::Save checks that before calling.
     */
    std::string Render(const std::string& name, const std::vector<SerializedComponent>& components);

} // namespace SparkEditor::PrefabTextFormat
