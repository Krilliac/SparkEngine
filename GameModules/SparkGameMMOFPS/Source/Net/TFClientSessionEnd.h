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

    /**
     * @brief Whether a UI logout must close the transport to end the server session.
     *
     * The authority unbinds an account only when the session is cleaned up. A
     * remote client's session ends on its socket leave, so logout disconnects
     * the transport. The in-process listen-host / standalone player has no
     * socket; TFClientNet::Disconnect runs the same authoritative cleanup for it
     * directly, and hosting continues.
     */
    inline bool LogoutStopsTransport(NetRole currentRole) noexcept
    {
        return currentRole == NetRole::Client;
    }
} // namespace Terrafront
