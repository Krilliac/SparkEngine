/**
 * @file NetworkTestSecurity.h
 * @brief NET-100 test fixture: the SparkTests process-wide server identity and pinned trust
 *
 * NetworkManager has no unauthenticated mode, so every test that starts a server
 * or connects a client needs a server identity and a pinned key. TestMain installs
 * one ephemeral identity (generated in memory, never written to disk) and pins its
 * public key, so no test touches the per-user identity file or known_hosts.
 * Raw-socket peers that stand in for a server sign with the same identity
 * (TestServerIdentity) so the singleton client accepts them.
 *
 * Thread affinity: game/test thread. Ownership: the identity lives for the process.
 */

#pragma once

#include "Engine/Networking/NetworkManager.h"

namespace SparkTestFixtures
{
    /** @brief The process-wide ephemeral test server identity (generated on first use). */
    const Spark::Net::ServerIdentity& TestServerIdentity();

    /** @brief The security configuration TestMain installs: TestServerIdentity plus a pin on its key. */
    Spark::Net::NetworkSecurityConfig TestNetworkSecurityConfig();

    /** @brief Install TestNetworkSecurityConfig on the NetworkManager singleton. */
    void InstallTestNetworkSecurity();

    /**
     * @brief Restores the singleton's security configuration when it goes out of scope
     *
     * Tests that change the configuration (a wrong pin, no identity, TOFU in a
     * temporary directory) hold one of these so later tests see the default.
     */
    class ScopedNetworkSecurity
    {
      public:
        explicit ScopedNetworkSecurity(Spark::Net::NetworkSecurityConfig replacement);
        ~ScopedNetworkSecurity();

        ScopedNetworkSecurity(const ScopedNetworkSecurity&) = delete;
        ScopedNetworkSecurity& operator=(const ScopedNetworkSecurity&) = delete;

      private:
        Spark::Net::NetworkSecurityConfig m_saved;
    };
} // namespace SparkTestFixtures
