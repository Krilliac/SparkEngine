/**
 * @file DecorLayoutEditorInternal.h
 * @brief Shared constants and helpers for the DecorLayoutEditorPanel implementation files
 * @author Spark Engine Team
 * @date 2026
 *
 * Internal to the DecorLayoutEditor* .cpp split (DecorLayoutEditorPanel.cpp,
 * DecorLayoutEditorIO.cpp, DecorLayoutEditorCanvas.cpp,
 * DecorLayoutEditorSidePane.cpp) — do not include from other panels.
 */

#pragma once

#include <cmath>
#include <cstddef>
#include <string>

namespace SparkEditor
{
    // Mirrors of the game-module limits/conventions. The editor cannot
    // include game-module headers, so the values are duplicated here; keep
    // them in sync with their sources:
    //  - kMaxCollideParts / kMaxDecorPerRegion: World/TFRegionDecor.cpp
    //  - kPawnRadiusM: Game/TFMovementModel.h (kTFPawnRadiusM 0.4 => 0.8 m diameter)
    constexpr size_t kMaxCollideParts = 5;
    constexpr size_t kMaxDecorPerRegion = 20;
    constexpr float kPawnRadiusM = 0.4f;

    /// Fixed tier order — matches both the hand-authored file and the
    /// kTiers iteration in TFRegionDecor::LoadTemplates.
    inline const char* const kTierNames[] = {"outpost", "fort", "facility", "skyanchor"};
    constexpr int kTierCount = 4;

    /// Short display label: path basename without the .obj suffix.
    inline std::string ModelLabel(const std::string& model)
    {
        const size_t slash = model.find_last_of('/');
        std::string base = (slash == std::string::npos) ? model : model.substr(slash + 1);
        if (base.size() > 4 && base.compare(base.size() - 4, 4, ".obj") == 0)
            base.resize(base.size() - 4);
        return base;
    }

    /// Snap to 15-degree steps, normalized to (-180, 180].
    inline float SnapYaw15(float deg)
    {
        float snapped = std::round(deg / 15.0f) * 15.0f;
        while (snapped > 180.0f)
            snapped -= 360.0f;
        while (snapped <= -180.0f)
            snapped += 360.0f;
        return snapped;
    }
} // namespace SparkEditor
