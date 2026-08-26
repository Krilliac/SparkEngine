#include "TestFramework.h"

#include "Core/EngineContext.h"
#include "Core/SubsystemConsoleCommands.h"
#include "Engine/ECS/Components.h"
#include "Utils/SparkConsole.h"

namespace
{
    struct InjectedContextReset
    {
        ~InjectedContextReset() { EngineContext::SetInjected(nullptr); }
    };
} // namespace

TEST(EcsCameraConsole_ControlsExplicitSceneWorldWithoutLegacyCamera)
{
    EngineContext context;
    World world;
    const EntityID cameraEntity = world.CreateEntity("Main Camera");
    Transform& transform = world.AddComponent<Transform>(cameraEntity);
    Camera& camera = world.AddComponent<Camera>(cameraEntity);
    camera.isMainCamera = true;
    context.SetWorld(&world);
    EngineContext::SetInjected(&context);
    const InjectedContextReset reset;

    auto& console = Spark::SimpleConsole::GetInstance();
    EXPECT_TRUE(console.Initialize());
    Spark::RegisterSubsystemConsoleCommands();

    EXPECT_TRUE(console.ExecuteCommand("cam_fov 65"));
    EXPECT_TRUE(console.ExecuteCommand("cam_pos 4 5 -6"));
    EXPECT_TRUE(console.ExecuteCommand("cam_lookat 0 1 2"));

    EXPECT_NEAR(camera.fov, 65.0f, 0.001f);
    EXPECT_NEAR(transform.position.x, 4.0f, 0.001f);
    EXPECT_NEAR(transform.position.y, 5.0f, 0.001f);
    EXPECT_NEAR(transform.position.z, -6.0f, 0.001f);
    EXPECT_TRUE(transform.rotation.x > 0.0f);
    EXPECT_TRUE(transform.rotation.y < 0.0f);
}
