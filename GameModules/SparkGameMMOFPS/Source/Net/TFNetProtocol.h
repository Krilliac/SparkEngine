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
