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
