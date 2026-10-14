/**
 * @file Glyphs.h
 * @brief UTF-8 literals for the Font Awesome 6 Free Solid glyphs the UI kit draws itself.
 *
 * The launcher, installer and game layer use these instead of SparkEditor/Source/Core/EditorIcons.h.
 * The codepoints live in the private-use range LoadFontSet merges into every face.
 */

#pragma once

namespace Spark::Ui::Glyph
{
    inline constexpr const char* kCircleCheck = "\xef\x81\x98";         ///< U+F058
    inline constexpr const char* kTriangleExclamation = "\xef\x81\xb1"; ///< U+F071
    inline constexpr const char* kCircleXmark = "\xef\x81\x97";         ///< U+F057
    inline constexpr const char* kCircleInfo = "\xef\x81\x9a";          ///< U+F05A
    inline constexpr const char* kLock = "\xef\x80\xa3";                ///< U+F023
    inline constexpr const char* kChevronRight = "\xef\x81\x94";        ///< U+F054
    inline constexpr const char* kChevronDown = "\xef\x81\xb8";         ///< U+F078
    inline constexpr const char* kMagnifyingGlass = "\xef\x80\x82";     ///< U+F002
    inline constexpr const char* kEllipsis = "\xef\x85\x81";            ///< U+F141
    inline constexpr const char* kPlay = "\xef\x81\x8b";                ///< U+F04B
    inline constexpr const char* kXmark = "\xef\x80\x8d";               ///< U+F00D
    inline constexpr const char* kCircleQuestion = "\xef\x81\x99";      ///< U+F059
} // namespace Spark::Ui::Glyph
