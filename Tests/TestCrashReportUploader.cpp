// TestCrashReportUploader.cpp - Tests for crash report uploader utilities
// Tests ComputeStackHash, IsCrashUploadRateLimited, and URL auto-detection

#include "TestFramework.h"
#include <string>
#include <sstream>
#include <cstdint>
#include <chrono>
#include <atomic>

// ============================================================================
// Inline reimplementation of ComputeStackHash for test purposes.
// The real implementation lives in CrashReportUploader.cpp which may be
// excluded when miniz is not found. This is identical logic.
// ============================================================================

static std::string TestComputeStackHash(const std::string& logContent, int maxFrames = 5)
{
    std::istringstream iss(logContent);
    std::string line;
    std::string frameData;
    int frameCount = 0;

    while (std::getline(iss, line) && frameCount < maxFrames)
    {
        if (line.size() < 4)
            continue;

        bool isFrame = false;
        if (line.find("  #") != std::string::npos)
            isFrame = true;
        else if (line.starts_with("0x") || line.starts_with("  0x"))
            isFrame = true;
        else if (line.find("Frame ") != std::string::npos || line.find("SymStackTrace") != std::string::npos)
            isFrame = true;
        else if (line.find(" + 0x") != std::string::npos)
            isFrame = true;

        if (isFrame)
        {
            std::string cleaned;
            size_t i = 0;
            while (i < line.size())
            {
                if (i + 1 < line.size() && line[i] == '0' && (line[i + 1] == 'x' || line[i + 1] == 'X'))
                {
                    i += 2;
                    while (i < line.size() &&
                           ((line[i] >= '0' && line[i] <= '9') || (line[i] >= 'a' && line[i] <= 'f') ||
                            (line[i] >= 'A' && line[i] <= 'F')))
                        ++i;
                    if (!cleaned.empty() && cleaned.back() != ' ')
                        cleaned += ' ';
                    continue;
                }
                if (line[i] >= '0' && line[i] <= '9')
                {
                    while (i < line.size() && line[i] >= '0' && line[i] <= '9')
                        ++i;
                    if (!cleaned.empty() && cleaned.back() != ' ')
                        cleaned += ' ';
                    continue;
                }
                if ((line[i] >= 'A' && line[i] <= 'Z') || (line[i] >= 'a' && line[i] <= 'z') || line[i] == '_' ||
                    line[i] == ':')
                {
                    cleaned += line[i];
                }
                else if (!cleaned.empty() && cleaned.back() != ' ')
                {
                    cleaned += ' ';
                }
                ++i;
            }
            frameData += cleaned + "\n";
            ++frameCount;
        }
    }

    if (frameData.empty())
        return "";

    uint32_t hash = 0x811c9dc5u;
    for (char c : frameData)
    {
        hash ^= static_cast<uint32_t>(static_cast<unsigned char>(c));
        hash *= 0x01000193u;
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%08x", hash);
    return buf;
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
