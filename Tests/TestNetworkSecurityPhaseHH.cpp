/**
 * @file TestNetworkSecurityPhaseHH.cpp
 * @brief Phase HH Theme 3D tests for Spark::Net::NetworkSecurity
 *
 * NetworkSecurity is a per-instance registry (not a singleton) of single-use
 * CSPRNG connection tokens. It provides no encryption or connection
 * authentication; these tests pin its fail-closed token lifecycle.
 */

#include "TestFramework.h"
#include "Engine/Networking/NetworkSecurity.h"

TEST(NetworkSecurityPhaseHH_ConnectionTokenRoundTrip)
{
    Spark::Net::NetworkSecurity sec;
    Spark::Net::NetworkSecurity::Token token{};
    ASSERT_TRUE(sec.GenerateConnectionToken(token));
    EXPECT_TRUE(sec.ValidateConnectionToken(token));
    // Token is single-use; second validation must fail.
    EXPECT_FALSE(sec.ValidateConnectionToken(token));
}

TEST(NetworkSecurityPhaseHH_UnknownTokenFailsValidation)
{
    Spark::Net::NetworkSecurity sec;
    Spark::Net::NetworkSecurity::Token fake{};
    fake.fill(0xAB);
    EXPECT_FALSE(sec.ValidateConnectionToken(fake));
}
