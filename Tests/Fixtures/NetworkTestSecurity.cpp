/**
 * @file NetworkTestSecurity.cpp
 * @brief Process-wide NET-100 test identity (see NetworkTestSecurity.h)
 */

#include "NetworkTestSecurity.h"

#include <cstdlib>
#include <iostream>
#include <utility>

namespace SparkTestFixtures
{
    const Spark::Net::ServerIdentity& TestServerIdentity()
    {
        static const Spark::Net::ServerIdentity identity = []
        {
            auto generated = Spark::Net::GenerateServerIdentity();
            if (!generated)
            {
                std::cerr << "FATAL: libsodium could not generate the test server identity\n";
                std::abort();
            }
            return *generated;
        }();
        return identity;
    }

    Spark::Net::NetworkSecurityConfig TestNetworkSecurityConfig()
    {
        Spark::Net::NetworkSecurityConfig config;
        config.identity = TestServerIdentity();
        config.trust = Spark::Net::ServerTrust::Pin(TestServerIdentity().publicKey);
        return config;
    }

    void InstallTestNetworkSecurity()
    {
        Spark::Net::NetworkManager::GetInstance().SetSecurityConfig(TestNetworkSecurityConfig());
    }

    ScopedNetworkSecurity::ScopedNetworkSecurity(Spark::Net::NetworkSecurityConfig replacement)
        : m_saved(Spark::Net::NetworkManager::GetInstance().GetSecurityConfig())
    {
        Spark::Net::NetworkManager::GetInstance().SetSecurityConfig(std::move(replacement));
    }

    ScopedNetworkSecurity::~ScopedNetworkSecurity()
    {
        Spark::Net::NetworkManager::GetInstance().SetSecurityConfig(std::move(m_saved));
    }
} // namespace SparkTestFixtures
