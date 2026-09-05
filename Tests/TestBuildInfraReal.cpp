/**
 * @file TestBuildInfraReal.cpp
 * @brief Real SDK version-contract tests binding the build system to Spark/Version.h.
 *
 * SPARK_ENGINE_VERSION is a CMake cache variable the release pipeline overrides
 * from the git tag; project() feeds it into cmake/SparkGeneratedVersion.h.in,
 * which SparkSDK/Include/Spark/Version.h includes. Nothing proved that chain
 * held, so a package built from a 1.1.0 tag could ship an SDK header still
 * announcing 1.0.0. SPARK_TESTS_EXPECTED_VERSION is injected from PROJECT_VERSION
 * by Tests/CMakeLists.txt, so these tests fail the moment the two diverge.
 */

#include "TestFramework.h"

#include <Spark/Version.h>

#include <cstdint>
#include <string>

namespace
{
    /** @brief Compose the SDK header's version macros into a dotted string. */
    std::string HeaderVersionString()
    {
        return std::to_string(SPARK_ENGINE_VERSION_MAJOR) + "." + std::to_string(SPARK_ENGINE_VERSION_MINOR) + "." +
               std::to_string(SPARK_ENGINE_VERSION_PATCH);
    }
} // namespace

#ifndef SPARK_TESTS_EXPECTED_VERSION
TEST(BuildInfra_SdkHeaderVersionMatchesProjectVersion)
{
    SKIP_TEST("SPARK_TESTS_EXPECTED_VERSION was not injected by the build system");
}
#else
TEST(BuildInfra_SdkHeaderVersionMatchesProjectVersion)
{
    const std::string expected = SPARK_TESTS_EXPECTED_VERSION;
    EXPECT_FALSE(expected.empty());
    EXPECT_EQ(HeaderVersionString(), expected);
}
#endif

TEST(BuildInfra_PackedEngineVersionMatchesComponents)
{
    const uint32_t packed = Spark::GetEngineVersion();
    EXPECT_EQ(static_cast<int>((packed >> 16) & 0xFFu), SPARK_ENGINE_VERSION_MAJOR);
    EXPECT_EQ(static_cast<int>((packed >> 8) & 0xFFu), SPARK_ENGINE_VERSION_MINOR);
    EXPECT_EQ(static_cast<int>(packed & 0xFFu), SPARK_ENGINE_VERSION_PATCH);
}

TEST(BuildInfra_SdkCompatibilityIsExactMatchOnly)
{
    const uint32_t current = Spark::GetSDKVersion();
    EXPECT_TRUE(Spark::IsSDKCompatible(current));
    EXPECT_FALSE(Spark::IsSDKCompatible(current + 1u));
    EXPECT_FALSE(Spark::IsSDKCompatible(current - 1u));
}
