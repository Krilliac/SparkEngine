// TestCrashReportUploader.cpp - Tests for crash report uploader utilities
// Tests ComputeStackHash, IsCrashUploadRateLimited, and URL auto-detection

#include "TestFramework.h"
#include "Utils/CrashReportUploader.h"
#include <string>
#include <sstream>
#include <cstdint>
#include <chrono>
#include <atomic>

// ============================================================================
// ComputeStackHash is exercised through the real CrashReportUploader.cpp
// implementation. This file used to carry an inline copy of that logic, which
// is precisely why the production parser could stop recognising the engine's
// own stack-trace format without a single test turning red.
// ============================================================================

static std::string TestComputeStackHash(const std::string& logContent, int maxFrames = 5)
{
    return ComputeStackHash(logContent, maxFrames);
}

// ============================================================================
// ComputeStackHash tests
// ============================================================================

TEST(ComputeStackHash_EmptyLog)
{
    std::string hash = TestComputeStackHash("");
    EXPECT_TRUE(hash.empty());
}

TEST(ComputeStackHash_NoFrames)
{
    std::string log = "Some random text\nwith no stack frames\nat all";
    std::string hash = TestComputeStackHash(log);
    EXPECT_TRUE(hash.empty());
}

TEST(ComputeStackHash_GccBacktraceStyle)
{
    std::string log = "================================================================\n"
                      "           SPARK ENGINE CRASH REPORT\n"
                      "================================================================\n"
                      "\n"
                      "*** CRASH DETECTED ***\n"
                      "\n"
                      "  #0  0x00007f1234 in CrashHandler::HandleSignal()\n"
                      "  #1  0x00007f5678 in PhysicsSystem::StepSimulation()\n"
                      "  #2  0x00007f9abc in GameLoop::Update()\n"
                      "  #3  0x00007fdef0 in main()\n";
    std::string hash = TestComputeStackHash(log);
    EXPECT_FALSE(hash.empty());
    EXPECT_EQ(hash.size(), static_cast<size_t>(8));
}

TEST(ComputeStackHash_WindowsSymStyle)
{
    std::string log = "*** CRASH DETECTED ***\n"
                      "0x00007FFA1234 SparkEngine.dll!GraphicsEngine::Present + 0x42\n"
                      "0x00007FFA5678 SparkEngine.dll!GameLoop::Render + 0x1A\n"
                      "0x00007FFA9ABC SparkEngine.dll!WinMain + 0xFF\n";
    std::string hash = TestComputeStackHash(log);
    EXPECT_FALSE(hash.empty());
    EXPECT_EQ(hash.size(), static_cast<size_t>(8));
}

TEST(ComputeStackHash_DeterministicSameInput)
{
    std::string log = "  #0  0xAAAA in Foo::Bar()\n"
                      "  #1  0xBBBB in Baz::Qux()\n";
    std::string hash1 = TestComputeStackHash(log);
    std::string hash2 = TestComputeStackHash(log);
    EXPECT_EQ(hash1, hash2);
}

TEST(ComputeStackHash_DifferentStacksDifferentHash)
{
    std::string log1 = "  #0  0xAAAA in Foo::Bar()\n"
                       "  #1  0xBBBB in Baz::Qux()\n";
    std::string log2 = "  #0  0xAAAA in Alpha::Beta()\n"
                       "  #1  0xBBBB in Gamma::Delta()\n";
    std::string hash1 = TestComputeStackHash(log1);
    std::string hash2 = TestComputeStackHash(log2);
    EXPECT_FALSE(hash1.empty());
    EXPECT_FALSE(hash2.empty());
    EXPECT_NE(hash1, hash2);
}

TEST(ComputeStackHash_AddressChangesIgnored)
{
    std::string log1 = "  #0  0x1111 in Foo::Bar()\n"
                       "  #1  0x2222 in Baz::Qux()\n";
    std::string log2 = "  #0  0x9999 in Foo::Bar()\n"
                       "  #1  0xAAAA in Baz::Qux()\n";
    std::string hash1 = TestComputeStackHash(log1);
    std::string hash2 = TestComputeStackHash(log2);
    EXPECT_EQ(hash1, hash2);
}

TEST(ComputeStackHash_MaxFramesRespected)
{
    std::string log = "  #0  0xAAAA in Foo::A()\n"
                      "  #1  0xBBBB in Foo::B()\n"
                      "  #2  0xCCCC in Foo::C()\n"
                      "  #3  0xDDDD in Foo::D()\n"
                      "  #4  0xEEEE in Foo::E()\n"
                      "  #5  0xFFFF in Foo::F()\n";

    std::string hash2 = TestComputeStackHash(log, 2);
    std::string hash5 = TestComputeStackHash(log, 5);
    EXPECT_NE(hash2, hash5);
}
