/**
 * @file BasicMaterialEditorInternal.h
 * @brief Shared helpers for the BasicMaterialEditorPanel implementation files
 * @author Spark Engine Team
 * @date 2026
 *
 * Internal to the BasicMaterialEditor* .cpp split (BasicMaterialEditorPanel.cpp,
 * BasicMaterialEditorDrawing.cpp) — do not include from other panels.
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace SparkEditor
{
    inline std::string ToLower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }
} // namespace SparkEditor
