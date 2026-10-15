/**
 * @file Glyphs.h
 * @brief UTF-8 literals for the Font Awesome 6 Free Solid glyphs the UI kit draws itself.
 *
 * The launcher, installer and game layer use these instead of SparkEditor/Source/Core/EditorIcons.h.
 * The codepoints live in the private-use range LoadFontSet merges into every face. Each trailing
 * comment names the codepoint and fa-solid-900.ttf's glyph name; tools/ui/check_icon_glyphs.py
 * fails when the bytes, the comment and the font disagree.
 */

#pragma once

namespace Spark::Ui::Glyph
{
    inline constexpr const char* kCircleCheck = "\xef\x81\x98";         ///< U+F058 circle-check
    inline constexpr const char* kTriangleExclamation = "\xef\x81\xb1"; ///< U+F071 triangle-exclamation
    inline constexpr const char* kCircleXmark = "\xef\x81\x97";         ///< U+F057 circle-xmark
    inline constexpr const char* kCircleInfo = "\xef\x81\x9a";          ///< U+F05A circle-info
    inline constexpr const char* kLock = "\xef\x80\xa3";                ///< U+F023 lock
    inline constexpr const char* kChevronRight = "\xef\x81\x94";        ///< U+F054 chevron-right
    inline constexpr const char* kChevronDown = "\xef\x81\xb8";         ///< U+F078 chevron-down
    inline constexpr const char* kMagnifyingGlass = "\xef\x80\x82";     ///< U+F002 magnifying-glass
    inline constexpr const char* kEllipsis = "\xef\x85\x81";            ///< U+F141 ellipsis
    inline constexpr const char* kPlay = "\xef\x81\x8b";                ///< U+F04B play
    inline constexpr const char* kXmark = "\xef\x80\x8d";               ///< U+F00D xmark
    inline constexpr const char* kCircleQuestion = "\xef\x81\x99";      ///< U+F059 circle-question
} // namespace Spark::Ui::Glyph
