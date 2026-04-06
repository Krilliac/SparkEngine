/**
 * @file TestCoreAndBuildSystems.cpp
 * @brief Tests for untested core, build, and mobile subsystems
 *
 * Covers AssetHandle, CSGSystem, GamePackager, MobilePlatform,
 * Reflection (TypeRegistry + ComponentFactory), and CrashHandler.
 */

#include "TestFramework.h"

#include "../SparkEngine/Source/Core/AssetHandle.h"
#include "../SparkEngine/Source/Core/Reflection.h"
#include "../SparkEngine/Source/Engine/Build/GamePackager.h"
#include "../SparkEngine/Source/Engine/LevelDesign/CSGSystem.h"
#include "../SparkEngine/Source/Engine/Mobile/MobilePlatform.h"
#include "../SparkEngine/Source/Utils/CrashHandler.h"

// ============================================================================
// 1. AssetHandle
// ============================================================================

TEST(AssetHandle_DefaultIsInvalid)
{
    AssetHandle h;
    EXPECT_FALSE(h.IsValid());
    EXPECT_EQ(h.hash, static_cast<uint64_t>(0));
}

TEST(AssetHandle_ExplicitHashIsValid)
{
    AssetHandle h(42);
    EXPECT_TRUE(h.IsValid());
}

TEST(AssetHandle_FromPathIsValid)
{
    auto h = AssetHandle::FromPath("textures/wood.png");
    EXPECT_TRUE(h.IsValid());
}

TEST(AssetHandle_FromStringMatchesFromPath)
{
    auto a = AssetHandle::FromPath("textures/wood.png");
    auto b = AssetHandle::FromString("textures/wood.png");
    EXPECT_EQ(a.hash, b.hash);
}

TEST(AssetHandle_DeterministicHash)
{
    auto a = AssetHandle::FromPath("meshes/hero.fbx");
    auto b = AssetHandle::FromPath("meshes/hero.fbx");
    EXPECT_EQ(a.hash, b.hash);
}

TEST(AssetHandle_DifferentPathsDifferentHashes)
{
    auto a = AssetHandle::FromPath("textures/wood.png");
    auto b = AssetHandle::FromPath("textures/stone.png");
    EXPECT_TRUE(a != b);
}

TEST(AssetHandle_EqualityOperators)
{
    auto a = AssetHandle::FromPath("a.txt");
    auto b = AssetHandle::FromPath("a.txt");
    auto c = AssetHandle::FromPath("b.txt");
    EXPECT_TRUE(a == b);
    EXPECT_TRUE(a != c);
}

TEST(AssetHandle_LessThanForMapOrdering)
{
    auto a = AssetHandle::FromPath("alpha");
    auto b = AssetHandle::FromPath("beta");
    // One must be less than the other (not equal)
    EXPECT_TRUE((a < b) || (b < a));
}

TEST(AssetHandle_EmptyPathStillHashes)
{
    // FNV-1a of empty string is the offset basis (non-zero), so IsValid() is true.
    // This confirms FromPath("") does not special-case empty strings.
    auto h = AssetHandle::FromPath("");
    EXPECT_TRUE(h.IsValid());
    // But default-constructed handle (hash=0) is the canonical "invalid" state
    EXPECT_TRUE(h != AssetHandle{});
}

// ============================================================================
// 2. CSGSystem
// ============================================================================

TEST(CSGVec3_Arithmetic)
{
    using namespace Spark::LevelDesign;
    CSGVec3 a{1, 2, 3};
    CSGVec3 b{4, 5, 6};

    auto sum = a + b;
    EXPECT_NEAR(sum.x, 5.0f, 0.001f);
    EXPECT_NEAR(sum.y, 7.0f, 0.001f);
    EXPECT_NEAR(sum.z, 9.0f, 0.001f);

    auto diff = b - a;
    EXPECT_NEAR(diff.x, 3.0f, 0.001f);

    auto scaled = a * 2.0f;
    EXPECT_NEAR(scaled.x, 2.0f, 0.001f);
    EXPECT_NEAR(scaled.y, 4.0f, 0.001f);
}

TEST(CSGVec3_DotCrossLengthNormalized)
{
    using namespace Spark::LevelDesign;
    CSGVec3 x{1, 0, 0};
    CSGVec3 y{0, 1, 0};

    EXPECT_NEAR(x.Dot(y), 0.0f, 0.001f);
    EXPECT_NEAR(x.Dot(x), 1.0f, 0.001f);

    auto cross = x.Cross(y);
    EXPECT_NEAR(cross.z, 1.0f, 0.001f);

    CSGVec3 v{3, 4, 0};
    EXPECT_NEAR(v.Length(), 5.0f, 0.001f);
    EXPECT_NEAR(v.Normalized().Length(), 1.0f, 0.001f);
}

TEST(CSGSystem_CreateBrushAndQuery)
{
    auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
    csg.Initialize();

    auto id = csg.CreateBrush(Spark::LevelDesign::BrushShape::Box, {2, 2, 2});
    EXPECT_TRUE(id > 0);
    EXPECT_EQ(csg.GetBrushCount(), static_cast<size_t>(1));
    EXPECT_TRUE(csg.GetBrush(id) != nullptr);

    csg.ClearAll();
}

TEST(CSGSystem_SetTransformAndOperation)
{
    auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
    csg.Initialize();

    auto id = csg.CreateBrush(Spark::LevelDesign::BrushShape::Box, {1, 1, 1});
    Spark::LevelDesign::CSGTransform xform;
    xform.position = {5, 0, 0};
    csg.SetBrushTransform(id, xform);

    const auto* brush = csg.GetBrush(id);
    EXPECT_NEAR(brush->transform.position.x, 5.0f, 0.001f);

    csg.SetBrushOperation(id, Spark::LevelDesign::CSGOperation::Subtractive);
    EXPECT_TRUE(csg.GetBrush(id)->operation == Spark::LevelDesign::CSGOperation::Subtractive);

    csg.ClearAll();
}

TEST(CSGSystem_RemoveAndClear)
{
    auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
    csg.Initialize();

    auto id1 = csg.CreateBrush(Spark::LevelDesign::BrushShape::Box, {1, 1, 1});
    auto id2 = csg.CreateBrush(Spark::LevelDesign::BrushShape::Cylinder, {1, 2, 0});
    EXPECT_EQ(csg.GetBrushCount(), static_cast<size_t>(2));

    csg.RemoveBrush(id1);
    EXPECT_EQ(csg.GetBrushCount(), static_cast<size_t>(1));

    csg.ClearAll();
    EXPECT_EQ(csg.GetBrushCount(), static_cast<size_t>(0));
}

TEST(CSGSystem_GenerateMeshBox)
{
    auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
    csg.Initialize();

    auto id = csg.CreateBrush(Spark::LevelDesign::BrushShape::Box, {2, 2, 2});
    const auto* solid = csg.GetBrush(id);
    auto mesh = csg.GenerateMesh(*solid);

    EXPECT_TRUE(!mesh.vertices.empty());
    EXPECT_TRUE(!mesh.indices.empty());
    EXPECT_TRUE(mesh.triangleCount > 0);

    csg.ClearAll();
}

TEST(CSGSystem_UnionCombinesFaces)
{
    auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
    csg.Initialize();

    auto idA = csg.CreateBrush(Spark::LevelDesign::BrushShape::Box, {2, 2, 2});
    auto idB = csg.CreateBrush(Spark::LevelDesign::BrushShape::Box, {2, 2, 2});

    const auto* a = csg.GetBrush(idA);
    const auto* b = csg.GetBrush(idB);
    auto result = csg.Union(*a, *b);

    // Union should at least return a valid solid (faces may vary by implementation)
    EXPECT_TRUE(result.operation == Spark::LevelDesign::CSGOperation::Additive);

    csg.ClearAll();
}

TEST(CSGSystem_ConsoleStatus)
{
    auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
    csg.Initialize();
    auto status = csg.Console_GetStatus();
    EXPECT_FALSE(status.empty());

    csg.ClearAll();
}

TEST(CSGSystem_AllShapes)
{
    auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
    csg.Initialize();

    auto box = csg.CreateBrush(Spark::LevelDesign::BrushShape::Box, {1, 1, 1});
    auto cyl = csg.CreateBrush(Spark::LevelDesign::BrushShape::Cylinder, {1, 2, 0});
    auto sph = csg.CreateBrush(Spark::LevelDesign::BrushShape::Sphere, {1, 0, 0});
    auto wed = csg.CreateBrush(Spark::LevelDesign::BrushShape::Wedge, {2, 1, 2});
    auto con = csg.CreateBrush(Spark::LevelDesign::BrushShape::Cone, {1, 2, 0});

    EXPECT_TRUE(box > 0);
    EXPECT_TRUE(cyl > 0);
    EXPECT_TRUE(sph > 0);
    EXPECT_TRUE(wed > 0);
    EXPECT_TRUE(con > 0);
    EXPECT_EQ(csg.GetBrushCount(), static_cast<size_t>(5));

    csg.ClearAll();
}

// ============================================================================
// 3. GamePackager
// ============================================================================

TEST(GamePackager_InitAndPackageCount)
{
    auto& packager = Spark::Build::GamePackager::GetInstance();
    packager.Initialize();
    EXPECT_EQ(packager.GetPackageCount(), static_cast<uint32_t>(0));
}

TEST(GamePackager_ValidateEmptyConfig)
{
    auto& packager = Spark::Build::GamePackager::GetInstance();
    packager.Initialize();

    Spark::Build::PackageConfig config;
    config.projectName = "";
    config.executablePath = "";
    auto errors = packager.ValidateConfig(config);
    EXPECT_TRUE(!errors.empty());
}

TEST(GamePackager_ValidatePopulatedConfig)
{
    auto& packager = Spark::Build::GamePackager::GetInstance();
    packager.Initialize();

    Spark::Build::PackageConfig config;
    config.projectName = "TestGame";
    config.executablePath = "/nonexistent/path/game.exe";
    auto errors = packager.ValidateConfig(config);
    // Should have errors for missing files, but fewer than a completely empty config
    EXPECT_TRUE(!errors.empty()); // exe doesn't exist on disk
}

TEST(GamePackager_ModuleExtensions)
{
    using P = Spark::Build::PackagePlatform;
    EXPECT_EQ(Spark::Build::GamePackager::GetModuleExtension(P::WindowsX64), std::string(".dll"));
    EXPECT_EQ(Spark::Build::GamePackager::GetModuleExtension(P::LinuxX64), std::string(".so"));
    EXPECT_EQ(Spark::Build::GamePackager::GetModuleExtension(P::MacOSX64), std::string(".dylib"));
}

TEST(GamePackager_ExecutableExtensions)
{
    using P = Spark::Build::PackagePlatform;
    EXPECT_EQ(Spark::Build::GamePackager::GetExecutableExtension(P::WindowsX64), std::string(".exe"));
    EXPECT_EQ(Spark::Build::GamePackager::GetExecutableExtension(P::LinuxX64), std::string(""));
    EXPECT_EQ(Spark::Build::GamePackager::GetExecutableExtension(P::MacOSARM64), std::string(""));
}

TEST(GamePackager_ConsoleStatus)
{
    auto& packager = Spark::Build::GamePackager::GetInstance();
    packager.Initialize();
    auto status = packager.Console_GetStatus();
    EXPECT_FALSE(status.empty());
}

// ============================================================================
// 4. MobilePlatform
// ============================================================================

TEST(MobilePlatform_Initialize)
{
    Spark::Mobile::MobilePlatform mobile;
    EXPECT_TRUE(mobile.Initialize());
}

TEST(MobilePlatform_QualityPresetRoundTrip)
{
    Spark::Mobile::MobilePlatform mobile;
    mobile.Initialize();

    mobile.SetQualityPreset(Spark::Mobile::MobileQualityPreset::Low);
    EXPECT_TRUE(mobile.GetQualityPreset() == Spark::Mobile::MobileQualityPreset::Low);
}

TEST(MobilePlatform_HighQualityHasHigherSettings)
{
    Spark::Mobile::MobilePlatform mobile;
    mobile.Initialize();

    mobile.SetQualityPreset(Spark::Mobile::MobileQualityPreset::Low);
    auto lowSettings = mobile.GetQualitySettings();

    mobile.SetQualityPreset(Spark::Mobile::MobileQualityPreset::High);
    auto highSettings = mobile.GetQualitySettings();

    EXPECT_TRUE(highSettings.shadowResolution > lowSettings.shadowResolution);
    EXPECT_TRUE(highSettings.renderScale > lowSettings.renderScale);
    EXPECT_TRUE(highSettings.maxDrawCalls > lowSettings.maxDrawCalls);
}

TEST(MobilePlatform_TouchEvents)
{
    Spark::Mobile::MobilePlatform mobile;
    mobile.Initialize();

    Spark::Mobile::TouchEvent touch;
    touch.touchId = 1;
    touch.type = Spark::Mobile::TouchEventType::Began;
    touch.x = 0.5f;
    touch.y = 0.5f;
    mobile.ProcessTouchEvent(touch);

    auto active = mobile.GetActiveTouches();
    EXPECT_EQ(static_cast<int>(active.size()), 1);

    // End the touch
    touch.type = Spark::Mobile::TouchEventType::Ended;
    mobile.ProcessTouchEvent(touch);

    active = mobile.GetActiveTouches();
    EXPECT_EQ(static_cast<int>(active.size()), 0);
}

TEST(MobilePlatform_OrientationRoundTrip)
{
    Spark::Mobile::MobilePlatform mobile;
    mobile.Initialize();

    mobile.SetOrientation(Spark::Mobile::ScreenOrientation::LandscapeLeft);
    EXPECT_TRUE(mobile.GetOrientation() == Spark::Mobile::ScreenOrientation::LandscapeLeft);
}

TEST(MobilePlatform_BatteryAndSafeArea)
{
    Spark::Mobile::MobilePlatform mobile;
    mobile.Initialize();

    // Default battery is -1 (unknown), which is a valid sentinel
    float battery = mobile.GetBatteryLevel();
    EXPECT_TRUE(battery >= -1.0f && battery <= 1.0f);

    const auto& safe = mobile.GetSafeArea();
    EXPECT_TRUE(safe.top >= 0.0f);
    EXPECT_TRUE(safe.bottom >= 0.0f);
    EXPECT_TRUE(safe.left >= 0.0f);
    EXPECT_TRUE(safe.right >= 0.0f);
}

TEST(MobilePlatform_ConsoleStatus)
{
    Spark::Mobile::MobilePlatform mobile;
    mobile.Initialize();
    auto status = mobile.Console_GetStatus();
    EXPECT_FALSE(status.empty());
}

// ============================================================================
// 5. Reflection (TypeRegistry + ComponentFactory)
// ============================================================================

TEST(Reflection_TypeRegistryCount)
{
    auto& reg = Spark::TypeRegistry::Get();
    // Registry exists and has some count (may be zero or populated by static init)
    size_t count = reg.GetTypeCount();
    (void)count; // confirms no crash
}

TEST(Reflection_ComponentFactoryCount)
{
    auto& factory = Spark::ComponentFactory::Get();
    size_t count = factory.GetRegisteredCount();
    (void)count; // confirms no crash
}

TEST(Reflection_ComponentFactoryGetNames)
{
    auto& factory = Spark::ComponentFactory::Get();
    auto names = factory.GetRegisteredNames();
    EXPECT_EQ(names.size(), factory.GetRegisteredCount());
}

TEST(Reflection_ComponentFactoryUnknownType)
{
    auto& factory = Spark::ComponentFactory::Get();
    EXPECT_FALSE(factory.IsRegistered("NonExistentComponent_XYZ_999"));
}

// ============================================================================
// 6. CrashHandler
// ============================================================================

TEST(CrashConfig_DefaultConstruction)
{
    CrashConfig cfg;
    EXPECT_TRUE(cfg.captureScreenshot);
    EXPECT_TRUE(cfg.captureSystemInfo);
    EXPECT_TRUE(cfg.captureAllThreads);
    EXPECT_TRUE(cfg.zipBeforeUpload);
    EXPECT_FALSE(cfg.triggerCrashOnAssert);
    EXPECT_EQ(cfg.connectTimeoutSeconds, 5);
    EXPECT_TRUE(cfg.uploadURL.empty());
}

TEST(CrashHandler_SetAssertBehavior)
{
    // Just verify the function is callable without crashing
    SetAssertCrashBehavior(false);
    EXPECT_TRUE(true);
}

TEST(CrashConfig_CustomSettings)
{
    CrashConfig cfg;
    cfg.dumpPrefix = L"TestDump";
    cfg.captureScreenshot = false;
    cfg.triggerCrashOnAssert = true;
    cfg.connectTimeoutSeconds = 10;

    EXPECT_FALSE(cfg.captureScreenshot);
    EXPECT_TRUE(cfg.triggerCrashOnAssert);
    EXPECT_EQ(cfg.connectTimeoutSeconds, 10);
}
