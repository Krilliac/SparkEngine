/**
 * @file ProcessWin32JobPolicy.h
 * @brief Testable lifetime policy for Win32 child-process job assignment.
 */

#pragma once

namespace Spark::ProcessDetail
{

    /**
     * @brief Whether a child should inherit the engine process lifetime.
     *
     * Detached children are explicitly fire-and-forget and must outlive the
     * launcher when appropriate (for example SparkDaemon). Tracked children
     * remain in the kill-on-close job so crashes do not orphan consoles/tools.
     */
    [[nodiscard]] constexpr bool ShouldAssignToKillOnCloseJob(bool detached) noexcept
    {
        return !detached;
    }

} // namespace Spark::ProcessDetail
