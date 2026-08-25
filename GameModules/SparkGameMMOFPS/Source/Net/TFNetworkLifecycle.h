/**
 * @file TFNetworkLifecycle.h
 * @brief Shared, executable stop/restart contract for Terrafront network sessions.
 */
#pragma once

#include "Core/TFTypes.h"

#include <unordered_set>
#include <utility>
#include <vector>

namespace Terrafront
{
    /**
     * Drain every unique session before unregistering transport handlers.
     *
     * The helper is intentionally transport-agnostic so production teardown and
     * the regression suite execute the same ordering/state transition. The
     * caller supplies authoritative cleanup and handler-unregistration actions.
     */
    template <typename CleanupFn, typename UnregisterFn>
    void StopNetworkSessionLifecycle(std::unordered_set<PlayerId>& knownClients, bool& handlersRegistered,
                                     const std::vector<PlayerId>& additionalSessions, CleanupFn&& cleanup,
                                     UnregisterFn&& unregisterHandlers)
    {
        std::unordered_set<PlayerId> sessions = knownClients;
        for (const PlayerId player : additionalSessions)
        {
            if (player != kInvalidPlayer)
                sessions.insert(player);
        }

        // Both callbacks are named references here. Invoke them as lvalues so a
        // caller with an rvalue-qualified functor is never repeatedly consumed
        // while several sessions are drained.
        for (const PlayerId player : sessions)
            cleanup(player);
        knownClients.clear();

        if (std::exchange(handlersRegistered, false))
            unregisterHandlers();
    }
} // namespace Terrafront
