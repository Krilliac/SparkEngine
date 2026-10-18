/**
 * @file TFNetProtocol.h
 * @brief TERRAFRONT wire protocol — every app-level message on the network.
 *
 * FROZEN CONTRACT (see DESIGN.md §3). Messages ride NetworkManager's handler
 * registry. Entity transforms/health replicate through EntityReplicator, NOT
 * through these messages — TFMsg is for events and commands only.
 *
 * All structs are POD, packed, little-endian on the wire (x64 native order;
 * cross-endian hosts are out of scope v1). Every struct is static_asserted so
 * accidental layout drift breaks the build, not the game.
 *
 * Umbrella header: the contract is split into sibling part-headers purely for
 * file-size sanity — TFNetProtocolIds.h (the TFMsg enum),
 * TFNetProtocolGameplay.h (packed gameplay structs), and
 * TFNetProtocolOnboarding.h (auth/character/unlock/continent-hop structs).
 * Include THIS header as before; the split is an implementation detail and
 * every declaration keeps its exact name, layout, and namespace.
 */
#pragma once

#include "Core/TFTypes.h"
#include <cstdint>

// Dependency order: message ids first, then the packed wire structs.
#include "Net/TFNetProtocolIds.h"
#include "Net/TFNetProtocolGameplay.h"
#include "Net/TFNetProtocolOnboarding.h"

namespace Terrafront
{
#pragma pack(push, 1)

    /// S->C, reliable, sent just before TF_WorldWelcome (TF-120): the continents.json key of the continent this
    /// server hosts, NUL-terminated. The client compares it with the continent it loaded at boot and disconnects
    /// on a mismatch. A server that never sends it leaves the client unguarded.
    struct TF_ContinentIdentity
    {
        char key[64];
    };
    static_assert(sizeof(TF_ContinentIdentity) == 64, "wire layout frozen");

#pragma pack(pop)
} // namespace Terrafront
