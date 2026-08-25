/** @file TFClientSessionEnd.h @brief Pure client-link teardown decision shared with tests. */
#pragma once

#include "Core/TFTypes.h"

namespace Terrafront
{
    struct TFClientSessionEndDecision
    {
        NetRole role{NetRole::Standalone};
        bool resetLoginFlow{false};
    };

    inline TFClientSessionEndDecision PlanClientSessionEnd(NetRole currentRole, bool loginFlowAtLogin) noexcept
    {
        return {currentRole == NetRole::Client ? NetRole::Standalone : currentRole, !loginFlowAtLogin};
    }
} // namespace Terrafront
