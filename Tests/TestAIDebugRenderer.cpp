// TestAIDebugRenderer.cpp - Tests for AIDebugRenderer
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cstdint>
#include <string>
#include <vector>

namespace TestAIDbg
{

    struct XMFLOAT3
    {
        float x, y, z;
    };

    struct DebugDrawOptions
    {
        bool drawNavMesh = true;
        bool drawNavMeshBounds = false;
        bool drawPaths = true;
        bool drawPerceptionCones = true;
        bool drawBehaviorTreeState = false;
        bool drawFormations = true;
        bool drawCoverPoints = true;
        bool drawTacticalPoints = false;
        float navMeshOpacity = 0.3f;
        float pathLineWidth = 2.0f;
        uint32_t navMeshColor = 0x4000FF00;
        uint32_t pathColor = 0xFFFFFF00;
        uint32_t perceptionConeColor = 0x400000FF;
        uint32_t coverPointColor = 0xFFFF0000;
        uint32_t activeNodeColor = 0xFF00FF00;
    };

    class AIDebugRenderer
    {
      public:
        void Initialize()
        {
            m_initialized = true;
            m_enabled = false;
            m_selectedAgent = 0;
            m_options = DebugDrawOptions{};
        }

        void Update(float deltaTime) { (void)deltaTime; }

        void Shutdown()
        {
            m_enabled = false;
            m_initialized = false;
        }

        void DrawPath(const std::vector<XMFLOAT3>& waypoints, uint32_t color)
        {
            (void)color;
            m_lastPathSize = waypoints.size();
        }

        void SetOptions(const DebugDrawOptions& options) { m_options = options; }
        const DebugDrawOptions& GetOptions() const { return m_options; }
        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }
        void SetSelectedAgent(uint32_t entityId) { m_selectedAgent = entityId; }
        uint32_t GetSelectedAgent() const { return m_selectedAgent; }

        std::string Console_GetStatus() const
        {
            std::string status = "AIDebugRenderer: ";
            status += m_enabled ? "ENABLED" : "DISABLED";
            if (m_selectedAgent != 0)
                status += " selectedAgent=" + std::to_string(m_selectedAgent);
            status += " navMesh=" + std::string(m_options.drawNavMesh ? "on" : "off");
            status += " paths=" + std::string(m_options.drawPaths ? "on" : "off");
            return status;
        }

        size_t m_lastPathSize = 0;

      private:
        DebugDrawOptions m_options;
        bool m_enabled = false;
        bool m_initialized = false;
        uint32_t m_selectedAgent = 0;
    };

} // namespace TestAIDbg

TEST(AIDebugRenderer_InitAndDefaults)
{
    using namespace TestAIDbg;
    AIDebugRenderer renderer;
    renderer.Initialize();

    EXPECT_FALSE(renderer.IsEnabled());
    EXPECT_EQ(renderer.GetSelectedAgent(), uint32_t(0));
    EXPECT_TRUE(renderer.GetOptions().drawNavMesh);
    EXPECT_TRUE(renderer.GetOptions().drawPaths);
    EXPECT_FALSE(renderer.GetOptions().drawBehaviorTreeState);
}

TEST(AIDebugRenderer_EnableDisable)
{
    using namespace TestAIDbg;
    AIDebugRenderer renderer;
    renderer.Initialize();

    renderer.SetEnabled(true);
    EXPECT_TRUE(renderer.IsEnabled());

    renderer.SetEnabled(false);
    EXPECT_FALSE(renderer.IsEnabled());
}

TEST(AIDebugRenderer_SelectedAgent)
{
    using namespace TestAIDbg;
    AIDebugRenderer renderer;
    renderer.Initialize();

    renderer.SetSelectedAgent(42);
    EXPECT_EQ(renderer.GetSelectedAgent(), uint32_t(42));

    renderer.SetSelectedAgent(0);
    EXPECT_EQ(renderer.GetSelectedAgent(), uint32_t(0));
}

TEST(AIDebugRenderer_OptionModification)
{
    using namespace TestAIDbg;
    AIDebugRenderer renderer;
    renderer.Initialize();

    DebugDrawOptions opts;
    opts.drawNavMesh = false;
    opts.drawBehaviorTreeState = true;
    opts.navMeshOpacity = 0.5f;
    renderer.SetOptions(opts);

    EXPECT_FALSE(renderer.GetOptions().drawNavMesh);
    EXPECT_TRUE(renderer.GetOptions().drawBehaviorTreeState);
    EXPECT_NEAR(renderer.GetOptions().navMeshOpacity, 0.5f, 0.001f);
}

TEST(AIDebugRenderer_ConsoleGetStatus)
{
    using namespace TestAIDbg;
    AIDebugRenderer renderer;
    renderer.Initialize();

    std::string status = renderer.Console_GetStatus();
    EXPECT_TRUE(status.find("AIDebugRenderer") != std::string::npos);
    EXPECT_TRUE(status.find("DISABLED") != std::string::npos);

    renderer.SetEnabled(true);
    status = renderer.Console_GetStatus();
    EXPECT_TRUE(status.find("ENABLED") != std::string::npos);

    renderer.SetSelectedAgent(99);
    status = renderer.Console_GetStatus();
    EXPECT_TRUE(status.find("selectedAgent=99") != std::string::npos);
}

TEST(AIDebugRenderer_DrawPathEmptyNoCrash)
{
    using namespace TestAIDbg;
    AIDebugRenderer renderer;
    renderer.Initialize();
    renderer.SetEnabled(true);

    std::vector<XMFLOAT3> emptyPath;
    renderer.DrawPath(emptyPath, 0xFFFFFF00);
    EXPECT_EQ(renderer.m_lastPathSize, size_t(0));
}

TEST(AIDebugRenderer_ShutdownResetsState)
{
    using namespace TestAIDbg;
    AIDebugRenderer renderer;
    renderer.Initialize();
    renderer.SetEnabled(true);
    renderer.SetSelectedAgent(10);

    renderer.Shutdown();
    EXPECT_FALSE(renderer.IsEnabled());
}
