/**
 * @file GatewayLocalAdapters.h
 * @brief Local, deterministic stand-ins for the product-owned services behind the gateway seams.
 *
 * OD-08 keeps identity, entitlement, moderation and matchmaking out of engine scope, so the
 * engine meets "runs locally with deterministic adapters" at the seams those services plug
 * into (docs/specs/online-services.md sections 4 and 6):
 * - LocalFixtureAuthenticator (IGatewayAuthenticator, boundary B4) answers admission from a
 *   bounded fixture file instead of an identity, entitlement and moderation backend.
 * - LocalDeterministicPlacement (IAreaPlacementPolicy, boundary B9) places an admission
 *   instead of a matchmaker.
 * Neither reads a clock or a random source, so a fixed admission script gives a fixed result.
 */

#pragma once

#include "GatewayCoordinator.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Spark::Gateway
{
    /**
     * Largest admission fixture accepted, in bytes. A principal entry takes at least 70 bytes, so
     * this also caps a fixture at about 900 principals.
     */
    inline constexpr size_t LocalFixtureMaximumBytes = 64u * 1024u;
    /** Longest principal identifier a fixture may declare. */
    inline constexpr size_t LocalFixtureMaximumPrincipalIdSize = 128;

    /**
     * Admission from a fixture file (boundary B4). The fixture is strict JSON:
     *
     *     {"version": 1, "principals": [
     *         {"credential": "...", "principalId": "...", "entitled": true, "moderation": "none"}]}
     *
     * `moderation` is "none" or "banned". Unknown or repeated keys, a repeated credential or
     * principalId, an empty, over-long or non-printable field, or a file over
     * LocalFixtureMaximumBytes reject the whole fixture: the authenticator is then
     * not ready and GatewayCoordinator refuses every admission (fail closed).
     *
     * Authenticate() rejects an unknown credential, a banned principal and an unentitled one, in
     * that order, each with a fixed reason. It never logs or echoes the credential.
     *
     * Thread affinity: immutable after construction, so Authenticate() and IsReady() are safe
     * on any transport thread. Not a secret store: the fixture is for owner-local development.
     */
    class LocalFixtureAuthenticator final : public IGatewayAuthenticator
    {
      public:
        /** [startup thread] Load and validate @p fixtureFile. Check IsReady() / Error(). */
        explicit LocalFixtureAuthenticator(const std::filesystem::path& fixtureFile);

        /** [startup thread] Validate fixture text already in memory. */
        [[nodiscard]] static LocalFixtureAuthenticator FromText(std::string_view fixtureText);

        [[nodiscard]] AuthenticationResult Authenticate(const AdmissionRequest& request) override;
        [[nodiscard]] bool IsReady() const override { return m_error.empty(); }
        /** Why the fixture was rejected; empty when ready. Never contains a credential. */
        [[nodiscard]] const std::string& Error() const { return m_error; }
        [[nodiscard]] size_t GetPrincipalCount() const { return m_principals.size(); }

      private:
        struct Principal
        {
            std::string principalId;
            bool entitled = false;
            bool banned = false;
        };

        LocalFixtureAuthenticator() = default;
        void Load(std::string_view fixtureText);

        std::unordered_map<std::string, Principal> m_principals;
        std::string m_error;
    };

    /**
     * Local matchmaking stand-in (boundary B9): the online area with free capacity that has the
     * fewest sessions, ties broken by the lowest AreaID. The result depends only on @p areas, not
     * on their order, so a placement tie resolves the same way on every run. This is the policy
     * GatewayCoordinator uses unless a product policy is passed to it.
     */
    class LocalDeterministicPlacement final : public IAreaPlacementPolicy
    {
      public:
        [[nodiscard]] Net::AreaID Place(const PlacementRequest& request, std::span<const AreaSnapshot> areas) override;
    };
} // namespace Spark::Gateway
