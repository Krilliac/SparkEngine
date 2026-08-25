/**
 * @file TFOnboardingSessionRules.h
 * @brief Server-authoritative onboarding state-machine predicates.
 */
#pragma once

namespace Terrafront
{
    inline bool CanBeginAuthentication(bool authenticated, bool enteredWorld) noexcept
    {
        return !authenticated && !enteredWorld;
    }

    inline bool CanMutateCharacterProfile(bool enteredWorld) noexcept
    {
        return !enteredWorld;
    }
} // namespace Terrafront
