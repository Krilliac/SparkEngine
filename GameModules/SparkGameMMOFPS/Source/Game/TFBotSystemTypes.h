/**
 * @file TFBotSystemTypes.h
 * @brief Shared bot-facing declarations split out of TFBotSystem.h: the base
 *        engine/protocol includes the bot system builds on, the forward
 *        declarations of its frozen cross-agent contracts, and the bot
 *        player-id range constants. Included by TFBotSystem.h (the umbrella);
 *        no other file needs to include this directly.
 */
#pragma once

#include "Core/TFEvents.h"
#include "Core/TFTypes.h"
#include "Net/TFNetProtocol.h"

#include <memory>
#include <random>
#include <string>
#include <vector>

namespace Terrafront
{

    struct PawnInfo;      // defined in Game/TFPlayerSystem.h (frozen W1 contract)
    class TFChaosHarness; // Game/TFChaosHarness.h (bots-chaos lane validation)

    /// Bot player ids live in their own range, distinct from real network client
    /// ids (small integers from NetworkManager) and kTFLocalHostPlayer (0xFFFFFF01).
    constexpr PlayerId kTFBotIdBase = 0xB0700000u;
    constexpr uint32_t kTFMaxBots = 32;

} // namespace Terrafront
