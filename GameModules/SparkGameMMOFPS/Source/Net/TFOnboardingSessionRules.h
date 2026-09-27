/**
 * @file TFOnboardingSessionRules.h
 * @brief Server-authoritative onboarding state-machine predicates.
 */
#pragma once

#include <cstdint>

namespace Terrafront
{
    /** Credential onboarding is local-only until the gameplay transport is cryptographically authenticated. */
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
