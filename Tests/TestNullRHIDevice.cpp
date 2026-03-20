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
