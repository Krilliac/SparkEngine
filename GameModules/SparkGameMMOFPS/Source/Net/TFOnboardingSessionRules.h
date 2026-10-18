/**
 * @file TFOnboardingSessionRules.h
 * @brief Server-authoritative onboarding state-machine predicates.
 */
#pragma once

#include <cstdint>

namespace Terrafront
{
    /**
     * Credential onboarding (login and registration) is accepted only from the local host or a loopback client.
     *
     * The original reason, an unauthenticated plaintext transport, is gone: NET-100 seals the transport and
     * SCRAM keeps passwords off the wire. The gate stays as policy until remote account onboarding is an
     * explicit product decision (it also enables remote registration).
     */
    inline bool CanUseCredentialOnboarding(bool localHostSentinel, bool loopbackNetworkClient) noexcept
    {
        return localHostSentinel || loopbackNetworkClient;
    }

    inline bool CanBeginAuthentication(bool authenticated, bool enteredWorld) noexcept
    {
        return !authenticated && !enteredWorld;
    }

    inline bool CanMutateCharacterProfile(bool enteredWorld) noexcept
    {
        return !enteredWorld;
    }

    /**
     * @brief Whether a client FactionSelect may change the session faction.
     *
     * A session bound to a character takes its faction only from the character
     * record (HandleEnterWorld); a client packet can never rebind it -- not
     * before the first spawn and not while dead. Unbound sessions keep the
     * legacy rule: no switch while a pawn is alive.
     */
    inline bool CanApplyFactionSelect(bool characterBound, bool pawnAlive) noexcept
    {
        return !characterBound && !pawnAlive;
    }

    /**
     * @brief True when a session other than `sender` already has `characterId` entered.
     * @param activeCharacters Map of session (PlayerId) -> entered character id.
     */
    template <typename ActiveCharacterMap, typename Session>
    bool IsCharacterResidentElsewhere(const ActiveCharacterMap& activeCharacters, Session sender,
                                      uint64_t characterId) noexcept
    {
        for (const auto& [session, character] : activeCharacters)
        {
            if (character == characterId && session != sender)
                return true;
        }
        return false;
    }
} // namespace Terrafront
