/**
 * @file Test_gamemodules_mmochat_di.cpp
 * @brief Regression test: MMOChatSystem resolves networking through the injected
 *        IEngineContext, not the NetworkManager global singleton.
 *
 * The DI-correctness fix routes all network access in MMOChatSystem through the
 * context handed to Initialize(context). Before the fix the system reached for
 * NetworkManager::GetInstance() and never touched the injected context, which is
 * what allowed a client/server split-brain when the two resolved to different
 * NetworkManager instances. This test would fail before the fix (the context's
 * GetNetwork() was never called) and passes after.
 *
 * Guarded by SPARK_TEST_HAS_IMGUI because MMOChatSystem.cpp includes ImGui for
 * its debug UI.
 */

#include "../TestFramework.h"

#ifdef SPARK_TEST_HAS_IMGUI

#include "../../GameModules/SparkGameMMO/Source/Chat/MMOChatSystem.h"
#include "Spark/IEngineContext.h"

#include <string>

namespace
{
    /**
     * @brief Minimal IEngineContext that records GetNetwork() resolution.
     *
     * Every service is an unused null stub except GetNetwork(), which counts how
     * many times the system-under-test resolved networking through the context.
     * Returning nullptr also exercises the graceful "no network" path.
     */
    class RecordingContext : public Spark::IEngineContext
    {
      public:
        GraphicsEngine* GetGraphics() override { return nullptr; }
        const GraphicsEngine* GetGraphics() const override { return nullptr; }
        InputManager* GetInput() override { return nullptr; }
        const InputManager* GetInput() const override { return nullptr; }
        Timer* GetTimer() override { return nullptr; }
        const Timer* GetTimer() const override { return nullptr; }
        Spark::EventBus* GetEventBus() override { return nullptr; }
        const Spark::EventBus* GetEventBus() const override { return nullptr; }
        ::AudioEngine* GetAudio() override { return nullptr; }
        const ::AudioEngine* GetAudio() const override { return nullptr; }
        PhysicsSystem* GetPhysics() override { return nullptr; }
        const PhysicsSystem* GetPhysics() const override { return nullptr; }
        uint32_t GetEngineVersion() const override { return 0; }
        uint32_t GetSDKVersion() const override { return 0; }

        Spark::Net::NetworkManager* GetNetwork() override
        {
            ++m_getNetworkCalls;
            return nullptr;
        }

        int GetNetworkCalls() const { return m_getNetworkCalls; }

      private:
        int m_getNetworkCalls = 0;
    };
} // namespace

TEST(MMO_ChatSystem_ResolvesNetworkViaInjectedContext)
{
    RecordingContext ctx;
    MMO::MMOChatSystem sys;
    EXPECT_TRUE(sys.Initialize(&ctx));

    // SendMessage is the second network-touching path; both it and Initialize
    // must resolve the NetworkManager through the context.
    sys.SendMessage(MMO::ChatChannel::Global, "regression");

#ifdef ENABLE_NETWORKING
    // Before the DI fix, network access went through NetworkManager::GetInstance()
    // and the injected context's GetNetwork() was never called (counter == 0).
    EXPECT_GT(ctx.GetNetworkCalls(), 0);
#endif

    // With a context whose GetNetwork() yields null, networking is skipped but the
    // local history path still records the message — no crash, no global fallback.
    const auto& history = sys.GetHistory();
    ASSERT_FALSE(history.empty());
    EXPECT_EQ(history.back().text, std::string("regression"));

    sys.Shutdown();
}

#endif // SPARK_TEST_HAS_IMGUI
