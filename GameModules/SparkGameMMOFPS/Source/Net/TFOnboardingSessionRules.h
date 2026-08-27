/**
 * @file TFOnboardingSessionRules.h
 * @brief Server-authoritative onboarding state-machine predicates.
 */
#pragma once

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
} // namespace Terrafront
