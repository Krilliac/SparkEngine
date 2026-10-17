/**
 * @file ImGuiUtils.h
 * @brief Utility helpers for Dear ImGui rendering in the SparkEditor
 *
 * Thread affinity: main (ImGui) thread only; every helper draws into the current window.
 */

#pragma once

#include <imgui.h>
#include "SparkUI/Widgets.h"

namespace SparkEditor
{

    /// Draw a token-coloured vertical separator between SameLine() items. It places itself on the
    /// current line and leaves the cursor on the same line, so callers write
    /// `Item(); VerticalSeparator(); Item();`. Forwards to Spark::Ui::ToolbarSeparator().
    inline void VerticalSeparator()
    {
        Spark::Ui::ToolbarSeparator();
    }

} // namespace SparkEditor
