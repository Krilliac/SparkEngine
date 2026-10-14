/**
 * @file RuntimeState.h
 * @brief Private per-context state behind Runtime.h, shared by the SparkUIKit sources.
 *
 * Thread affinity: UI thread only. Ownership: allocated by Spark::Ui::Install and owned by the
 * ImGui context through a Shutdown context hook that deletes it.
 */

#pragma once

#include "SparkUI/Tokens.h"

#include <vector>

#include <imgui.h>

namespace Spark::Ui::Detail
{
    /// @brief One open property grid.
    struct PropertyGridFrame
    {
        bool stacked = false;
    };

    /// @brief Everything Spark UI remembers about one ImGui context.
    struct RuntimeState
    {
        Tokens tokens;
        float dpiScale = 1.0f;
        float userScale = 1.0f;
        float pendingDpiScale = 1.0f;
        float pendingUserScale = 1.0f;
        bool hasPendingScale = false;
        bool reduceMotion = false;
        std::vector<PropertyGridFrame> grids;
        std::vector<bool> cardClicked; ///< One entry per open card: clicked this frame.
    };

    /// @brief State of the current context, created with Ember Dark defaults on first use;
    ///        nullptr only when no ImGui context is current.
    RuntimeState* CurrentState();
} // namespace Spark::Ui::Detail
