/**
 * @file NetworkComponents.h
 * @brief ECS networking components: NetworkIdentity
 *
 * Moved from AIComponents.h to properly separate networking concerns
 * from AI concerns (R6.2 — Architecture Analysis).
 */

#pragma once
#include <cstdint>

// =============================================================================
// NetworkIdentity
// =============================================================================

/**
 * @brief Identifies an entity in the networked game session.
 *
 * Attached to any entity that should be replicated across the network.
 * The NetworkManager uses this component to track ownership, authority,
 * and which properties should be synchronized.
 */
struct NetworkIdentity
{
    uint32_t networkID = 0;         ///< Unique ID assigned by the server
    uint32_t ownerClientID = 0;     ///< Client that owns this entity
    bool isLocalAuthority = false;  ///< Whether this client has authority
    bool replicateTransform = true; ///< Replicate position/rotation
    bool replicateHealth = true;    ///< Replicate health state
};
