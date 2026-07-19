/**
 * @file RegionMapEditorInternal.h
 * @brief Shared constants and helpers for the RegionMapEditorPanel implementation files
 * @author Spark Engine Team
 * @date 2026
 *
 * Internal to the RegionMapEditor* .cpp split (RegionMapEditorPanel.cpp,
 * RegionMapEditorIO.cpp, RegionMapEditorCanvas.cpp,
 * RegionMapEditorSidePane.cpp) — do not include from other panels.
 * Everything lives in the RegionMapInternal nested namespace so the names
 * cannot collide with other panels' internals (e.g. DecorLayoutEditorInternal.h
 * defines a kTierNames of its own in namespace SparkEditor).
 */

#pragma once

#include <cmath>
#include <cstdio>
#include <string>

namespace SparkEditor
{
    namespace RegionMapInternal
    {
        // Mirrors of the game-module limits (Core/TFTypes.h). The editor cannot
        // include game-module headers, so the values are duplicated here; keep
        // them in sync with kMaxRegions / kMaxCapturePoints.
        constexpr int kMaxRegions = 64;
        constexpr int kMaxCapturePoints = 3;

        inline const char* const kTierNames[] = {"skyanchor", "outpost", "fort", "facility"};
        inline const char* const kFactionTags[] = {"MRA", "AUC", "HLX"};

        inline bool IsValidTier(const std::string& tier)
        {
            for (const char* t : kTierNames)
                if (tier == t)
                    return true;
            return false;
        }

        inline bool IsFactionTag(const std::string& tag)
        {
            for (const char* t : kFactionTags)
                if (tag == t)
                    return true;
            return false;
        }

        /// @brief Number formatting for the writer: integers without a decimal
        ///        point (matches the hand-authored file), fractions trimmed.
        inline std::string FormatNum(double v)
        {
            if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15)
            {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
                return buf;
            }
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.3f", v);
            std::string s = buf;
            while (!s.empty() && s.back() == '0')
                s.pop_back();
            if (!s.empty() && s.back() == '.')
                s.pop_back();
            return s;
        }

        inline float Dist2(float ax, float ay, float bx, float by)
        {
            const float dx = ax - bx;
            const float dy = ay - by;
            return dx * dx + dy * dy;
        }
    } // namespace RegionMapInternal
} // namespace SparkEditor
