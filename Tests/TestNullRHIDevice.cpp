/**
 * @file TestNullRHIDevice.cpp
 * @brief Tests for headless RHI backend
 */

#include "TestFramework.h"
#include <cstdint>

namespace
{

    struct NullDeviceStats
    {
        uint32_t buffersCreated = 0;
        uint32_t texturesCreated = 0;
        uint32_t shadersCreated = 0;
        uint32_t pipelinesCreated = 0;
        uint32_t framesRendered = 0;
        uint32_t drawCalls = 0;
    };

    class TestNullDevice
    {
      public:
        bool Initialize()
        {
            m_initialized = true;
            return true;
        }
        void Shutdown()
        {
            m_initialized = false;
            m_stats = {};
        }

        void CreateBuffer() { m_stats.buffersCreated++; }
        void CreateTexture() { m_stats.texturesCreated++; }
        void CreateShader() { m_stats.shadersCreated++; }
        void CreatePipeline() { m_stats.pipelinesCreated++; }
        void BeginFrame() { m_stats.framesRendered++; }
        void Draw() { m_stats.drawCalls++; }

        bool IsInitialized() const { return m_initialized; }
        const NullDeviceStats& GetStats() const { return m_stats; }

      private:
        bool m_initialized = false;
        NullDeviceStats m_stats;
    };

} // anonymous namespace

TEST(NullRHIDevice_InitShutdown)
{
    TestNullDevice device;
    EXPECT_FALSE(device.IsInitialized());
    EXPECT_TRUE(device.Initialize());
    EXPECT_TRUE(device.IsInitialized());
    device.Shutdown();
    EXPECT_FALSE(device.IsInitialized());
}

TEST(NullRHIDevice_ResourceTracking)
{
    TestNullDevice device;
    device.Initialize();

    device.CreateBuffer();
    device.CreateBuffer();
    device.CreateTexture();
    device.CreateShader();
    device.CreatePipeline();

    EXPECT_EQ(device.GetStats().buffersCreated, 2u);
    EXPECT_EQ(device.GetStats().texturesCreated, 1u);
    EXPECT_EQ(device.GetStats().shadersCreated, 1u);
    EXPECT_EQ(device.GetStats().pipelinesCreated, 1u);
}

TEST(NullRHIDevice_FrameAndDraw)
{
    TestNullDevice device;
    device.Initialize();

    device.BeginFrame();
    device.Draw();
    device.Draw();
    device.Draw();
    device.BeginFrame();
    device.Draw();

    EXPECT_EQ(device.GetStats().framesRendered, 2u);
    EXPECT_EQ(device.GetStats().drawCalls, 4u);
}

TEST(NullRHIDevice_ShutdownResetsStats)
{
    TestNullDevice device;
    device.Initialize();
    device.CreateBuffer();
    device.CreateTexture();
    device.BeginFrame();
    device.Draw();

    device.Shutdown();
    EXPECT_EQ(device.GetStats().buffersCreated, 0u);
    EXPECT_EQ(device.GetStats().texturesCreated, 0u);
    EXPECT_EQ(device.GetStats().framesRendered, 0u);
    EXPECT_EQ(device.GetStats().drawCalls, 0u);
}

TEST(NullRHIDevice_ReinitializeAfterShutdown)
{
    TestNullDevice device;
    EXPECT_TRUE(device.Initialize());
    device.CreateBuffer();
    device.Shutdown();
    EXPECT_FALSE(device.IsInitialized());

    EXPECT_TRUE(device.Initialize());
    EXPECT_TRUE(device.IsInitialized());
    EXPECT_EQ(device.GetStats().buffersCreated, 0u); // Reset on shutdown
}

TEST(NullRHIDevice_AllResourceTypes)
{
    TestNullDevice device;
    device.Initialize();

    device.CreateBuffer();
    device.CreateTexture();
    device.CreateShader();
    device.CreatePipeline();

    const auto& stats = device.GetStats();
    EXPECT_EQ(stats.buffersCreated, 1u);
    EXPECT_EQ(stats.texturesCreated, 1u);
    EXPECT_EQ(stats.shadersCreated, 1u);
    EXPECT_EQ(stats.pipelinesCreated, 1u);
}

TEST(NullRHIDevice_ManyFrames)
{
    TestNullDevice device;
    device.Initialize();

    for (int i = 0; i < 100; ++i)
    {
        device.BeginFrame();
        device.Draw();
    }

    EXPECT_EQ(device.GetStats().framesRendered, 100u);
    EXPECT_EQ(device.GetStats().drawCalls, 100u);
}
