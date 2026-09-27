/**
 * @file TFClientMsgRouting.h
 * @brief The single list of client-originated TFMsg ids the authority routes
 *        into TFServerSim::RouteClientMessage.
 *
 * TFServerSim::RegisterNetHandlers, UnregisterNetHandlers and the
 * RouteClientMessage enter-world gate all read these arrays, so a gameplay id
 * cannot be gated (and handled) yet never registered with NetworkManager --
 * the way TFMsg::LoadoutExtChange was once dropped as an "unknown message
 * type" on the socket path while only loopback reached its handler.
 *
 * Contract: header-only constexpr data, no allocation, safe from any thread.
 */
#pragma once

#include "Net/TFNetProtocol.h"

#include <algorithm>
#include <array>

namespace Terrafront
{
    /// Gameplay ids: routed on the socket path and rejected unless the sender has entered the world.
    inline constexpr std::array<TFMsg, 18> kTFEnteredWorldGatedMsgs = {
        TFMsg::ClientInput,     TFMsg::SpawnRequest,       TFMsg::FireEvent,        TFMsg::FactionSelect,
        TFMsg::VehicleEnter,    TFMsg::VehicleExit,        TFMsg::AegisDeploy,      TFMsg::SquadMsg,
        TFMsg::ChatMsg,         TFMsg::LoadoutChange,      TFMsg::LoadoutExtChange, TFMsg::UnlockRequest,
        TFMsg::RedeployRequest, TFMsg::OutfitRequest,      TFMsg::AbilityRequest,   TFMsg::GrenadeThrow,
        TFMsg::PingPlace,       TFMsg::ContinentHopRequest};

    /// Post-login onboarding ids: routed on the socket path, never enter-world gated.
    inline constexpr std::array<TFMsg, 4> kTFOnboardingMsgs = {TFMsg::CharListRequest, TFMsg::CharCreateReq,
                                                               TFMsg::CharDeleteReq, TFMsg::EnterWorldReq};

    /// Login and registration ids: registered as sensitive handlers (SCRAM messages, NET-100).
    inline constexpr std::array<TFMsg, 3> kTFCredentialMsgs = {TFMsg::LoginRequest, TFMsg::LoginProof,
                                                               TFMsg::RegisterRequest};

    /// True when RouteClientMessage must reject `id` from a sender that has not entered the world.
    constexpr bool IsEnteredWorldGatedMsg(TFMsg id) noexcept
    {
        return std::find(kTFEnteredWorldGatedMsgs.begin(), kTFEnteredWorldGatedMsgs.end(), id) !=
               kTFEnteredWorldGatedMsgs.end();
    }
} // namespace Terrafront
