/**
 * @file LauncherViews.h
 * @brief SparkLauncher drawing: Ember Signal style install, the left rail and the Projects and
 *        New Project pages.
 *
 * LauncherApp owns all launcher state and performs every action (open, spawn, remove, create);
 * this file only draws that state and calls those actions. Splitting the two keeps the
 * inventoried LauncherApp.cpp (template.json reader call site) free of layout code.
 *
 * Thread affinity: UI thread only, on the ImGui context the platform backend created.
 * Ownership: LauncherViewState is a plain value owned by LauncherApp; the views keep no state of
 * their own beyond ImGui's per-window storage.
 */

#pragma once

#include "SparkUI/Tokens.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace SparkLauncher
{
    class LauncherApp;

    /**
     * @brief ImGui-free layout and text-fitting rules of the launcher views, kept in the header so
     *        SparkTests checks them headlessly. Widths are device pixels; text width comes from a
     *        caller-supplied measure (ImGui::CalcTextSize in the launcher).
     */
    namespace LauncherLayout
    {
        /// @brief Text that replaces the cut part of a shortened string.
        inline constexpr std::string_view kEllipsis = "...";

        /// @brief Horizontal columns of one project row; identical for present and Missing rows.
        struct ProjectRowColumns
        {
            float textX = 0.0f;        ///< Left edge of the name and path lines.
            float textWidth = 0.0f;    ///< Width the name and path may use.
            float metaX = 0.0f;        ///< Left edge of the "Opened ..." / Missing chip and version column.
            float primaryX = 0.0f;     ///< Left edge of the primary action ("Open" or "Locate...").
            float primaryWidth = 0.0f; ///< Width of the primary action button.
            float secondaryX = 0.0f;   ///< Left edge of the square secondary action (more menu or remove).
        };

        /**
         * @brief Columns of a project row spanning [@p rowMinX, @p rowMaxX].
         *
         * Nothing here depends on whether the project still exists, so Missing rows keep the columns of
         * the rows around them. Right to left: a square @p secondarySide button, @p itemSpacing, the
         * primary button of @p primaryWidth (the widest primary label of any row), @p padding, the
         * metadata column of @p metaWidth, @p padding, then the name and path.
         */
        inline ProjectRowColumns ComputeProjectRowColumns(float rowMinX, float rowMaxX, float padding,
                                                          float itemSpacing, float metaWidth, float primaryWidth,
                                                          float secondarySide)
        {
            ProjectRowColumns columns;
            columns.secondaryX = rowMaxX - padding - secondarySide;
            columns.primaryWidth = primaryWidth;
            columns.primaryX = columns.secondaryX - itemSpacing - primaryWidth;
            columns.metaX = columns.primaryX - padding - metaWidth;
            columns.textX = rowMinX + padding;
            columns.textWidth = std::max(0.0f, columns.metaX - padding - columns.textX);
            return columns;
        }

        /// @brief Byte offset of every UTF-8 code point start in @p text.
        inline std::vector<std::size_t> CodepointStarts(std::string_view text)
        {
            std::vector<std::size_t> starts;
            starts.reserve(text.size());
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                if ((static_cast<unsigned char>(text[i]) & 0xC0u) != 0x80u)
                {
                    starts.push_back(i);
                }
            }
            return starts;
        }

        /**
         * @brief @p text shortened in the middle ("C:/Users/.../Game.sparkproject") so that
         *        @p measure of the result is at most @p maxWidth. Cuts only at code point boundaries.
         * @param measure Callable `float(std::string_view)` returning the drawn width of a string.
         */
        template <typename Measure> std::string MiddleEllipsis(std::string_view text, float maxWidth, Measure&& measure)
        {
            if (measure(text) <= maxWidth)
            {
                return std::string(text);
            }
            const std::vector<std::size_t> starts = CodepointStarts(text);
            const std::size_t count = starts.size();
            const auto build = [&](std::size_t keep)
            {
                const std::size_t head = (keep + 1) / 2;
                const std::size_t tail = keep / 2;
                std::string out(text.substr(0, head < count ? starts[head] : text.size()));
                out += kEllipsis;
                out += text.substr(tail == 0 ? text.size() : starts[count - tail]);
                return out;
            };
            // Largest number of kept code points whose shortened form still fits.
            std::size_t low = 0;
            std::size_t high = count;
            while (low < high)
            {
                const std::size_t mid = (low + high + 1) / 2;
                if (measure(build(mid)) <= maxWidth)
                {
                    low = mid;
                }
                else
                {
                    high = mid - 1;
                }
            }
            return build(low);
        }

        /**
         * @brief @p path shortened to fit @p maxWidth by dropping whole directory names after the root:
         *        "D:/SparkEngine/build/native/bin/Release" becomes "D:/.../bin/Release".
         *
         * Keeps the root (the text up to the first separator, "D:/" or "/") and as many trailing
         * components as fit. When not even the last component fits beside the root, the root goes too
         * (".../Release"); a single name that is still too long is shortened in the middle. Both '/' and
         * a backslash separate components, and the separator in the path is kept.
         * @param measure Callable `float(std::string_view)` returning the drawn width of a string.
         */
        template <typename Measure> std::string EllipsizePath(std::string_view path, float maxWidth, Measure&& measure)
        {
            if (measure(path) <= maxWidth)
            {
                return std::string(path);
            }
            std::vector<std::size_t> separators;
            for (std::size_t i = 0; i + 1 < path.size(); ++i) // a trailing separator starts no component
            {
                if (path[i] == '/' || path[i] == '\\')
                {
                    separators.push_back(i);
                }
            }
            if (separators.size() >= 2)
            {
                const std::string_view root = path.substr(0, separators.front() + 1);
                // Longest tail first: drop the components between the root and separators[k].
                for (std::size_t k = 1; k < separators.size(); ++k)
                {
                    std::string candidate(root);
                    candidate += kEllipsis;
                    candidate += path.substr(separators[k]);
                    if (measure(candidate) <= maxWidth)
                    {
                        return candidate;
                    }
                }
            }
            if (!separators.empty())
            {
                std::string candidate(kEllipsis);
                candidate += path.substr(separators.back());
                if (measure(candidate) <= maxWidth)
                {
                    return candidate;
                }
            }
            return MiddleEllipsis(path, maxWidth, measure);
        }
    } // namespace LauncherLayout

    /// @brief Transient page state that only the views read and write (search text, pending confirm).
    struct LauncherViewState
    {
        std::array<char, 256> search{};  ///< Projects page filter (UTF-8), matched against name and path.
        std::string pendingRemovalPath;  ///< Recent-project path awaiting the remove confirmation.
        std::string pendingRemovalName;  ///< Display name shown in the remove confirmation.
        bool openRemovalConfirm = false; ///< Open the confirmation popup on the next draw of the page.
    };

    /**
     * @brief Style the current ImGui context for the launcher and load its fonts.
     *
     * Installs the Ember tokens for @p appearance with the Comfortable metrics profile and a 16 px
     * body size at @p dpiScale, then loads the Spark faces from EditorAssets/Fonts next to the
     * launcher executable (the embedded default font when that directory is missing). Call once,
     * after ImGui::CreateContext() and before the renderer backend creates its device objects.
     */
    void InstallLauncherStyle(float dpiScale, Spark::Ui::Appearance appearance);

    /**
     * @brief Re-theme the launcher for @p appearance after an OS light/dark or High Contrast change.
     *
     * Switches to the same Ember token set InstallLauncherStyle() builds for @p appearance, at the
     * current scale; fonts are not reloaded. Call between frames on the UI thread, after
     * InstallLauncherStyle().
     */
    void SetLauncherAppearance(Spark::Ui::Appearance appearance);

    /// @brief Draw the whole launcher for one frame: rail, status banner and the active page.
    void DrawLauncher(LauncherApp& app);
} // namespace SparkLauncher
