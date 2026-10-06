/**
 * @file MMOAuthAdmission.h
 * @brief Server-time admission budget for credential operations on the MMO session gate.
 *
 * Every wire Login costs one 600000-round PBKDF2 on the game thread (unknown usernames included:
 * MMOAccountSystem::Login verifies against a dummy hash). A valid, unique Register also costs
 * one derivation, so the gate must bound total KDF work. The former global 0.25 s cooldown
 * let one client hold every other client at RateLimited by sending continuously.
 *
 * The layered budget prevents one connected peer from consuming the whole aggregate rate:
 *  - Per peer: one credential operation per PeerCooldown seconds and at most
 *    RegistrationsPerPeer registrations per connection.
 *  - Global: a token bucket refilled at GlobalRate operations per second (the old sustained
 *    rate) with GlobalBurst capacity, so distinct peers can be admitted in the same tick. A
 *    single peer can use at most 1 / (PeerCooldown * GlobalRate) of it.
 *  - Global registrations: a separate bucket of RegistrationsPerWindow per
 *    RegistrationWindow seconds bounds account creation across reconnecting peers.
 * Per-account brute force remains bounded by MMOAccountSystem's failed-login lockout.
 *
 * Thread affinity: game thread (owned by MMOSessionGate). Allocation: none. Time is the
 * gate's server tick time, never wall-clock.
 */
#pragma once

#include <algorithm>
#include <cstdint>

namespace MMO
{
    class AuthAdmissionBudget
    {
      public:
        static constexpr float PeerCooldown = 1.0f;
        static constexpr float GlobalRate = 4.0f;
        static constexpr float GlobalBurst = 4.0f;
        static constexpr uint32_t RegistrationsPerPeer = 1;
        static constexpr float RegistrationWindow = 60.0f;
        static constexpr float RegistrationsPerWindow = 8.0f;

        /// Per-connection state; lives in the gate's session slot and resets with it.
        struct Peer
        {
            float cooldown = 0.0f;
            uint32_t registrations = 0;
        };

        enum class Operation : uint8_t
        {
            Login,
            Register
        };

        /// Refill the global buckets by @p elapsed server seconds (non-positive is ignored).
        void Advance(float elapsed)
        {
            if (!(elapsed > 0.0f))
            {
                return;
            }
            m_tokens = (std::min)(GlobalBurst, m_tokens + elapsed * GlobalRate);
            m_registrationTokens = (std::min)(
                RegistrationsPerWindow, m_registrationTokens + elapsed * (RegistrationsPerWindow / RegistrationWindow));
        }

        /// Count down one peer's cooldown by @p elapsed server seconds.
        static void AdvancePeer(Peer& peer, float elapsed)
        {
            if (elapsed > 0.0f)
            {
                peer.cooldown = (std::max)(0.0f, peer.cooldown - elapsed);
            }
        }

        /**
         * @brief Admit one credential operation for @p peer, charging every budget it uses.
         * @return false (nothing charged) when any per-peer or global limit is exhausted.
         */
        bool TryAdmit(Peer& peer, Operation operation)
        {
            if (peer.cooldown > 0.0f || m_tokens < 1.0f)
            {
                return false;
            }
            const bool registration = operation == Operation::Register;
            if (registration && (peer.registrations >= RegistrationsPerPeer || m_registrationTokens < 1.0f))
            {
                return false;
            }
            m_tokens -= 1.0f;
            peer.cooldown = PeerCooldown;
            if (registration)
            {
                m_registrationTokens -= 1.0f;
                ++peer.registrations;
            }
            return true;
        }

        void Reset() { *this = {}; }

      private:
        float m_tokens = GlobalBurst;
        float m_registrationTokens = RegistrationsPerWindow;
    };
} // namespace MMO
