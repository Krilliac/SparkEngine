/**
 * @file EditorThemeDocument.h
 * @brief Reader and writer for exported editor theme files (ThemeCustomizer import/export)
 *
 * ThemeCustomizer::ExportTheme writes what WriteThemeDocument renders and
 * ThemeCustomizer::ImportTheme parses a file through ParseThemeDocument. The format is a
 * JSON-like document scraped by key: "name"/"description"/"author" strings and one
 * [r, g, b, a] array per exported colour, read with sscanf. Both live in their own
 * translation unit so the SEC-120 fuzz target links the shipped reader without ImGui.
 *
 * Thread affinity: any thread; no shared state.
 * Ownership: inputs are borrowed; @p outTheme is written in place.
 * Allocation: the three strings, bounded by the caller's file-size limit.
 */

#pragma once

#include "EditorTheme.h"

#include <cstdint>
#include <string>

namespace SparkEditor
{

    /// @brief Largest theme file ImportTheme reads (an exported theme is about 2 KB).
    inline constexpr std::uint64_t kMaxThemeDocumentBytes = std::uint64_t{1024} * 1024;

    /**
     * @brief Parse an exported theme into @p outTheme.
     *
     * Sets name, description, author and every exported colour; fields the format does not
     * carry keep their values. A colour that is missing, malformed or has a component that
     * is not finite reads as ThemeColor() (opaque black).
     * @return true when the document names a theme.
     */
    bool ParseThemeDocument(const std::string& content, EditorThemeData& outTheme);

    /// @brief Render @p theme as ExportTheme writes it.
    std::string WriteThemeDocument(const EditorThemeData& theme);

} // namespace SparkEditor
