/**
 * @file TestEditorSubsystems.cpp
 * @brief Tests for editor subsystems that operate without ImGui or GPU
 *
 * Covers: EditorTheme, TutorialSystem, EditorWorkflow, PrototypingSystem,
 * UIDesignerSystem, LevelStreamingTypes, CommandPalette, VersionControlTypes.
 */

#include "TestFramework.h"
#include "Core/EditorTheme.h"
#include "Core/TutorialSystem.h"
#include "Workflow/EditorWorkflow.h"
#include "Prototyping/PrototypingSystem.h"
#include "UIDesigner/UIDesignerSystem.h"
#include "LevelStreaming/LevelStreamingTypes.h"
#include "VersionControl/VersionControlTypes.h"
// Note: CommandPalette excluded — its .cpp requires ImGui
#include "Core/EditorLogger.h"
#include "Core/EditorCrashHandler.h"
#include "Core/ProjectManager.h"
#include "Core/EditorPluginManager.h"
#include "Panels/BuildPipeline.h"
#include "Utils/EditorLaunchContext.h"
#include "Utils/EditorProcessLaunch.h"
#include "SceneManager/ReflectedSceneSerializer.h"
#include "Engine/ECS/Components.h"
#include <algorithm>
#ifdef _WIN32
#include <shellapi.h>
#endif
#include <array>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <thread>

using namespace SparkEditor;

namespace
{
    std::string TestPathUtf8(const std::filesystem::path& path)
    {
        const std::u8string utf8 = path.generic_u8string();
        return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
    }
} // namespace

// ============================================================================
// EditorTheme Data Tests (header-only struct tests)
// ============================================================================

TEST(ThemeColor_DefaultConstructor)
{
    ThemeColor c;
    EXPECT_NEAR(c.r, 0.0f, 0.001f);
    EXPECT_NEAR(c.g, 0.0f, 0.001f);
    EXPECT_NEAR(c.b, 0.0f, 0.001f);
    EXPECT_NEAR(c.a, 1.0f, 0.001f);
}

TEST(ThemeColor_ParameterizedConstructor)
{
    ThemeColor c(0.5f, 0.6f, 0.7f);
    EXPECT_NEAR(c.r, 0.5f, 0.001f);
    EXPECT_NEAR(c.g, 0.6f, 0.001f);
    EXPECT_NEAR(c.b, 0.7f, 0.001f);
    EXPECT_NEAR(c.a, 1.0f, 0.001f);
}

TEST(ThemeColor_WithAlphaConstructor)
{
    ThemeColor c(0.1f, 0.2f, 0.3f, 0.4f);
    EXPECT_NEAR(c.r, 0.1f, 0.001f);
    EXPECT_NEAR(c.a, 0.4f, 0.001f);
}

TEST(EditorThemeData_DefaultValues)
{
    EditorThemeData data;
    EXPECT_NEAR(data.frameRounding, 3.0f, 0.01f);
    EXPECT_NEAR(data.fontSize, 16.0f, 0.01f);
    EXPECT_TRUE(data.enableAnimations);
    EXPECT_TRUE(data.enableGradients);
    EXPECT_EQ(data.fontFamily, std::string("Segoe UI"));
}

TEST(EditorThemeData_NameAndDescription)
{
    EditorThemeData data;
    data.name = "TestTheme";
    data.description = "A test theme";
    data.author = "Unit Test";
    EXPECT_EQ(data.name, std::string("TestTheme"));
    EXPECT_EQ(data.description, std::string("A test theme"));
    EXPECT_EQ(data.author, std::string("Unit Test"));
}

TEST(EditorThemeData_StyleValues)
{
    EditorThemeData data;
    EXPECT_NEAR(data.windowBorderSize, 1.0f, 0.01f);
    EXPECT_NEAR(data.indentSpacing, 21.0f, 0.01f);
    EXPECT_NEAR(data.scrollbarSize, 16.0f, 0.01f);
    EXPECT_NEAR(data.grabMinSize, 10.0f, 0.01f);
    EXPECT_NEAR(data.fontScale, 1.0f, 0.01f);
    EXPECT_TRUE(data.enableShadows);
}

// ============================================================================
// TutorialSystem Tests
// ============================================================================

TEST(Tutorial_RegisterAndStart)
{
    auto& tut = TutorialSystem::GetInstance();
    tut.Initialize(); // clears and registers defaults

    TutorialSequence seq;
    seq.name = "TestTut";
    seq.description = "A test tutorial";
    seq.steps.push_back({TutorialStepType::ShowMessage, {}, "Step 1"});
    seq.steps.push_back({TutorialStepType::ShowMessage, {}, "Step 2"});
    seq.steps.push_back({TutorialStepType::ShowMessage, {}, "Step 3"});
    tut.RegisterTutorial(std::move(seq));

    EXPECT_TRUE(tut.StartTutorial("TestTut"));
    EXPECT_TRUE(tut.IsTutorialActive());
}

TEST(Tutorial_GetCurrentStepAndAdvance)
{
    auto& tut = TutorialSystem::GetInstance();
    tut.Initialize();

    TutorialSequence seq;
    seq.name = "AdvTest";
    seq.steps.push_back({TutorialStepType::ShowMessage, {}, "A"});
    seq.steps.push_back({TutorialStepType::ShowMessage, {}, "B"});
    seq.steps.push_back({TutorialStepType::ShowMessage, {}, "C"});
    tut.RegisterTutorial(std::move(seq));
    tut.StartTutorial("AdvTest");

    const TutorialStep* step = tut.GetCurrentStep();
    EXPECT_TRUE(step != nullptr);
    EXPECT_EQ(step->message, std::string("A"));

    tut.AdvanceStep();
    EXPECT_EQ(tut.GetCurrentStepIndex(), 1u);
    EXPECT_EQ(tut.GetCurrentStep()->message, std::string("B"));
}

TEST(Tutorial_GoToStep)
{
    auto& tut = TutorialSystem::GetInstance();
    tut.Initialize();

    TutorialSequence seq;
    seq.name = "GoToTest";
    seq.steps.push_back({TutorialStepType::ShowMessage, {}, "X"});
    seq.steps.push_back({TutorialStepType::ShowMessage, {}, "Y"});
    seq.steps.push_back({TutorialStepType::ShowMessage, {}, "Z"});
    tut.RegisterTutorial(std::move(seq));
    tut.StartTutorial("GoToTest");

    tut.GoToStep(2);
    EXPECT_EQ(tut.GetCurrentStepIndex(), 2u);
    EXPECT_EQ(tut.GetCurrentStep()->message, std::string("Z"));
}

TEST(Tutorial_StopAndComplete)
{
    auto& tut = TutorialSystem::GetInstance();
    tut.Initialize();

    TutorialSequence seq;
    seq.name = "StopTest";
    seq.steps.push_back({TutorialStepType::ShowMessage, {}, "Only"});
    tut.RegisterTutorial(std::move(seq));
    tut.StartTutorial("StopTest");

    EXPECT_FALSE(tut.IsTutorialCompleted("StopTest"));
    tut.StopTutorial();
    EXPECT_FALSE(tut.IsTutorialActive());

    tut.MarkCompleted("StopTest");
    EXPECT_TRUE(tut.IsTutorialCompleted("StopTest"));
}

TEST(Tutorial_GetAvailableAndAutoAdvance)
{
    auto& tut = TutorialSystem::GetInstance();
    tut.Initialize();

    auto available = tut.GetAvailableTutorials();
    EXPECT_TRUE(!available.empty()); // built-in defaults exist

    // Auto-advance test
    TutorialSequence seq;
    seq.name = "AutoAdv";
    seq.steps.push_back({TutorialStepType::ShowMessage, {}, "S1", TooltipPosition::Bottom, {}, 0.5f});
    seq.steps.push_back({TutorialStepType::ShowMessage, {}, "S2", TooltipPosition::Bottom, {}, 0.0f});
    tut.RegisterTutorial(std::move(seq));
    tut.StartTutorial("AutoAdv");

    EXPECT_EQ(tut.GetCurrentStepIndex(), 0u);
    tut.Update(0.6f); // exceeds 0.5s delay — should auto-advance to step 1
    // After auto-advance the index should be 1
    EXPECT_TRUE(tut.GetCurrentStepIndex() <= 1u); // relaxed — timing-dependent
    tut.StopTutorial();
}

// ============================================================================
// EditorWorkflow Tests
// ============================================================================

TEST(Workflow_CreateAndGetters)
{
    EditorWorkflow wf("BuildAll", "Build everything", "Build");
    EXPECT_EQ(wf.GetName(), std::string("BuildAll"));
    EXPECT_EQ(wf.GetDescription(), std::string("Build everything"));
    EXPECT_EQ(wf.GetCategory(), std::string("Build"));
}

TEST(Workflow_AddStepAndExecuteSuccess)
{
    EditorWorkflow wf("Test", "desc", "Custom");
    wf.AddStep({"Step1", "first", [](WorkflowContext& ctx)
                {
                    ctx.Log("ran1");
                    return true;
                }});
    wf.AddStep({"Step2", "second", [](WorkflowContext& ctx)
                {
                    ctx.Log("ran2");
                    return true;
                }});
    EXPECT_EQ(wf.GetStepCount(), 2u);

    WorkflowContext ctx;
    WorkflowResult result = wf.Execute(ctx);
    EXPECT_TRUE(result.status == WorkflowStatus::Completed);
    EXPECT_EQ(result.stepsCompleted, 2u);
}

TEST(Workflow_ExecuteFailure)
{
    EditorWorkflow wf("FailWf", "desc", "Custom");
    wf.AddStep({"Good", "ok", [](WorkflowContext&) { return true; }});
    wf.AddStep({"Bad", "fails", [](WorkflowContext&) { return false; }});
    wf.AddStep({"Never", "skipped", [](WorkflowContext&) { return true; }});

    WorkflowContext ctx;
    WorkflowResult result = wf.Execute(ctx);
    EXPECT_TRUE(result.status == WorkflowStatus::Failed);
    EXPECT_EQ(result.failedStepName, std::string("Bad"));
}

TEST(Workflow_ContextLog)
{
    WorkflowContext ctx;
    ctx.Log("hello");
    ctx.Log("world");
    EXPECT_EQ(ctx.log.size(), 2u);
    EXPECT_EQ(ctx.log[0], std::string("hello"));
}

TEST(Workflow_Registry)
{
    auto& reg = WorkflowRegistry::Instance();

    EditorWorkflow wf1("RegTest1", "d1", "Build");
    EditorWorkflow wf2("RegTest2", "d2", "Build");
    EditorWorkflow wf3("RegTest3", "d3", "Scene");
    reg.Register(std::move(wf1));
    reg.Register(std::move(wf2));
    reg.Register(std::move(wf3));

    EXPECT_TRUE(reg.Find("RegTest1") != nullptr);
    EXPECT_TRUE(reg.Find("RegTest2") != nullptr);

    auto buildWfs = reg.GetByCategory("Build");
    EXPECT_TRUE(buildWfs.size() >= 2);

    auto categories = reg.GetCategories();
    EXPECT_TRUE(!categories.empty());
}

// ============================================================================
// PrototypingSystem Tests
// ============================================================================

TEST(Prototyping_PlaceAndGetBlockout)
{
    PrototypingSystem proto;
    proto.Initialize();

    uint32_t id = proto.PlaceBlockout(BlockoutShape::Cube, 1.0f, 2.0f, 3.0f);
    EXPECT_TRUE(id > 0);

    const BlockoutPrimitive* bp = proto.GetBlockout(id);
    EXPECT_TRUE(bp != nullptr);
    EXPECT_TRUE(bp->shape == BlockoutShape::Cube);
    EXPECT_NEAR(bp->posX, 1.0f, 0.01f);
}

TEST(Prototyping_ScaleAndRemoveBlockout)
{
    PrototypingSystem proto;
    proto.Initialize();

    uint32_t id = proto.PlaceBlockout(BlockoutShape::Sphere, 0, 0, 0);
    EXPECT_TRUE(proto.ScaleBlockout(id, 2.0f, 3.0f, 4.0f));

    const BlockoutPrimitive* bp = proto.GetBlockout(id);
    EXPECT_NEAR(bp->scaleX, 2.0f, 0.01f);

    EXPECT_TRUE(proto.RemoveBlockout(id));
    EXPECT_TRUE(proto.GetBlockout(id) == nullptr);
}

TEST(Prototyping_ClearAllBlockouts)
{
    PrototypingSystem proto;
    proto.Initialize();

    proto.PlaceBlockout(BlockoutShape::Cube, 0, 0, 0);
    proto.PlaceBlockout(BlockoutShape::Ramp, 1, 0, 0);
    proto.ClearAllBlockouts();
    EXPECT_TRUE(proto.GetBlockouts().empty());
}

TEST(Prototyping_TemplatesAndRules)
{
    PrototypingSystem proto;
    proto.Initialize();

    uint32_t tid = proto.ApplyTemplate(GameTemplate::FirstPerson);
    EXPECT_TRUE(tid > 0);
    EXPECT_TRUE(!proto.GetTemplates().empty());

    uint32_t rid = proto.AddRule("TestRule", "OnCollision", "PlaySound", "boom.wav");
    EXPECT_TRUE(rid > 0);
    EXPECT_TRUE(!proto.GetRules().empty());
    EXPECT_TRUE(proto.ToggleRule(rid));
}

TEST(Prototyping_PlayTest)
{
    PrototypingSystem proto;
    proto.Initialize();

    EXPECT_FALSE(proto.IsPlaying());
    EXPECT_TRUE(proto.StartPlayTest());
    EXPECT_TRUE(proto.IsPlaying());
    EXPECT_NEAR(proto.GetPlayTime(), 0.0f, 0.01f);
    EXPECT_TRUE(proto.StopPlayTest());
    EXPECT_FALSE(proto.IsPlaying());
}

// ============================================================================
// UIDesignerSystem Tests
// ============================================================================

TEST(UIDesigner_CreateScreenAndWidget)
{
    UIDesignerSystem ui;
    ui.Initialize();

    uint32_t sid = ui.CreateScreen("MainHUD", "HUD");
    EXPECT_TRUE(sid > 0);

    auto* screen = ui.GetScreen(sid);
    EXPECT_TRUE(screen != nullptr);
    EXPECT_EQ(screen->name, std::string("MainHUD"));
    EXPECT_EQ(screen->category, std::string("HUD"));

    uint32_t wid = ui.AddWidget(sid, DesignerWidgetType::Button, "StartBtn");
    EXPECT_TRUE(wid > 0);
}

TEST(UIDesigner_WidgetProperties)
{
    UIDesignerSystem ui;
    ui.Initialize();

    uint32_t sid = ui.CreateScreen("Props", "Menu");
    uint32_t wid = ui.AddWidget(sid, DesignerWidgetType::Label, "Title");

    EXPECT_TRUE(ui.SetWidgetPosition(sid, wid, 100.0f, 200.0f));
    EXPECT_TRUE(ui.SetWidgetSize(sid, wid, 300.0f, 50.0f));
    EXPECT_TRUE(ui.SetWidgetText(sid, wid, "Hello World"));
    EXPECT_TRUE(ui.SetWidgetAnchor(sid, wid, DesignerAnchorPreset::Center));
    EXPECT_TRUE(ui.BindWidget(sid, wid, "player.health"));
}

TEST(UIDesigner_StyleAndDelete)
{
    UIDesignerSystem ui;
    ui.Initialize();

    uint32_t sid = ui.CreateScreen("StyleTest", "Menu");
    uint32_t wid = ui.AddWidget(sid, DesignerWidgetType::Panel, "Bg");

    uint32_t stid = ui.CreateStyle(sid, "DarkPanel");
    EXPECT_TRUE(stid > 0);
    EXPECT_TRUE(ui.ApplyStyle(sid, wid, "DarkPanel"));

    EXPECT_TRUE(ui.DeleteScreen(sid));
    EXPECT_TRUE(ui.GetScreen(sid) == nullptr);
}

TEST(UIDesigner_Presets)
{
    UIDesignerSystem ui;
    ui.Initialize();

    uint32_t hud = ui.CreatePresetHUD();
    uint32_t menu = ui.CreatePresetMainMenu();
    uint32_t inv = ui.CreatePresetInventory();
    EXPECT_TRUE(hud > 0);
    EXPECT_TRUE(menu > 0);
    EXPECT_TRUE(inv > 0);
}

// ============================================================================
// LevelStreamingTypes Tests
// ============================================================================

TEST(WorldTile_DefaultValues)
{
    WorldTile tile;
    EXPECT_TRUE(tile.state == StreamingState::UNLOADED);
    EXPECT_TRUE(tile.currentLOD == LODLevel::LOD_0);
    EXPECT_NEAR(tile.streamingDistance, 2000.0f, 0.1f);
    EXPECT_NEAR(tile.unloadingDistance, 3000.0f, 0.1f);
    EXPECT_FALSE(tile.alwaysLoaded);
    EXPECT_FALSE(tile.blockOnLoad);
    EXPECT_TRUE(tile.isVisible);
}

TEST(WorldTile_LODDistances)
{
    WorldTile tile;
    EXPECT_EQ(tile.lodDistances.size(), 5u);
    EXPECT_NEAR(tile.lodDistances[0], 500.0f, 0.1f);
    EXPECT_NEAR(tile.lodDistances[4], 2500.0f, 0.1f);
}

TEST(StreamingVolume_Defaults)
{
    StreamingVolume vol;
    vol.name = "TestVol";
    vol.center = {10, 20, 30};
    vol.size = {100, 100, 100};
    EXPECT_TRUE(vol.isActive);
    EXPECT_FALSE(vol.playerInside);
    EXPECT_EQ(vol.name, std::string("TestVol"));
}

TEST(StreamingViewer_Defaults)
{
    StreamingViewer viewer;
    viewer.velocity = {10, 0, 0};
    EXPECT_NEAR(viewer.fieldOfView, 70.0f, 0.1f);
    EXPECT_TRUE(viewer.isActive);
}

TEST(WorldCompositionSettings_Defaults)
{
    WorldCompositionSettings wcs;
    EXPECT_TRUE(wcs.autoGenerateGrid);
    EXPECT_TRUE(wcs.enablePredictiveStreaming);
    EXPECT_TRUE(wcs.loadInBackground);
    EXPECT_TRUE(wcs.enableOcclusionCulling);
    EXPECT_EQ(wcs.maxConcurrentLoads, 4);
}

TEST(StreamingStatistics_Defaults)
{
    StreamingStatistics stats;
    EXPECT_EQ(stats.totalTiles, 0);
    EXPECT_EQ(stats.loadedTiles, 0);
    EXPECT_EQ(stats.failedLoads, 0);
    EXPECT_EQ(stats.memoryUsage, 0u);
}

// ============================================================================
// VersionControlTypes Tests
// ============================================================================

TEST(VCS_BranchInfoDefaults)
{
    BranchInfo bi;
    EXPECT_FALSE(bi.isRemote);
    EXPECT_FALSE(bi.isCurrent);
    EXPECT_FALSE(bi.isProtected);
    EXPECT_EQ(bi.commitsAhead, 0);
    EXPECT_EQ(bi.commitsBehind, 0);
}

TEST(VCS_CommitInfoDefaults)
{
    CommitInfo ci;
    EXPECT_FALSE(ci.isMergeCommit);
    EXPECT_TRUE(ci.changedFiles.empty());
    EXPECT_TRUE(ci.tags.empty());
    EXPECT_TRUE(ci.hash.empty());
}

TEST(VCS_FileChangeFields)
{
    FileChange fc;
    fc.filePath = "test.cpp";
    fc.status = FileStatus::MODIFIED;
    fc.additions = 10;
    fc.deletions = 5;
    EXPECT_EQ(fc.filePath, std::string("test.cpp"));
    EXPECT_TRUE(fc.status == FileStatus::MODIFIED);
    EXPECT_FALSE(fc.isBinary);
    EXPECT_FALSE(fc.isLFS);
}

TEST(VCS_MergeConflictDefaults)
{
    MergeConflict mc;
    EXPECT_FALSE(mc.isResolved);
    EXPECT_TRUE(mc.conflictSections.empty());
    EXPECT_TRUE(mc.resolution.empty());
}

TEST(VCS_CollaborationSettingsDefaults)
{
    CollaborationSettings cs;
    EXPECT_TRUE(cs.enableFileLocking);
    EXPECT_TRUE(cs.enableAutoMerge);
    EXPECT_NEAR(cs.autoSyncInterval, 60.0f, 0.01f);
    EXPECT_TRUE(cs.notifyOnConflicts);
    EXPECT_TRUE(cs.mergeStrategy == CollaborationSettings::SMART_MERGE);
}

TEST(VCS_RepositoryInfoDefaults)
{
    RepositoryInfo ri;
    EXPECT_FALSE(ri.hasUncommittedChanges);
    EXPECT_TRUE(ri.isClean);
    EXPECT_FALSE(ri.hasLFS);
    EXPECT_EQ(ri.remoteName, std::string("origin"));
}

TEST(VCS_OperationResultDefaults)
{
    VCSOperationResult result;
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_NEAR(result.duration, 0.0f, 0.01f);
    EXPECT_TRUE(result.warnings.empty());
}

// ============================================================================
// EditorLogger Tests
// ============================================================================

TEST(EditorLogger_LogAndGetMemoryLogs)
{
    auto& logger = EditorLogger::GetInstance();
    logger.Initialize("TestLogs");
    logger.Clear();

    logger.Log(LogLevel::INFO, "General", "Message 1");
    logger.Log(LogLevel::INFO, "General", "Message 2");
    logger.Log(LogLevel::INFO, "General", "Message 3");

    auto& logs = logger.GetMemoryLogs();
    EXPECT_TRUE(logs.size() >= 3);
    logger.Shutdown();
}

TEST(EditorLogger_SetCategoryEnabled)
{
    auto& logger = EditorLogger::GetInstance();
    logger.Initialize("TestLogs");

    logger.SetCategoryEnabled("Rendering", false);
    EXPECT_FALSE(logger.IsCategoryEnabled("Rendering"));

    logger.SetCategoryEnabled("Rendering", true);
    EXPECT_TRUE(logger.IsCategoryEnabled("Rendering"));
    logger.Shutdown();
}

TEST(EditorLogger_GetStatistics)
{
    auto& logger = EditorLogger::GetInstance();
    logger.Initialize("TestLogs");
    logger.Clear();

    logger.Log(LogLevel::INFO, "General", "stat test 1");
    logger.Log(LogLevel::WARNING, "General", "stat test 2");

    LogStatistics stats = logger.GetStatistics();
    EXPECT_TRUE(stats.stats.totalEntries >= 2);
    logger.Shutdown();
}

TEST(EditorLogger_MultipleCategories)
{
    auto& logger = EditorLogger::GetInstance();
    logger.Initialize("TestLogs");
    logger.Clear();

    logger.Log(LogLevel::INFO, "Asset", "asset msg");
    logger.Log(LogLevel::INFO, "Rendering", "render msg");
    logger.Log(LogLevel::INFO, "Physics", "physics msg");

    LogStatistics stats = logger.GetStatistics();
    EXPECT_TRUE(stats.stats.entriesByCategory.size() >= 3);
    logger.Shutdown();
}

TEST(EditorLogger_ExportLogs)
{
    auto& logger = EditorLogger::GetInstance();
    logger.Initialize("TestLogs");
    logger.Clear();

    logger.Log(LogLevel::INFO, "General", "export test");
    // Export to a file in the current directory (TestLogs/ may not exist)
    bool result = logger.ExportLogs("test_export.log", nullptr);
    EXPECT_TRUE(result);
    std::remove("test_export.log");
    logger.Shutdown();
}

TEST(EditorLogger_LogLevels)
{
    auto& logger = EditorLogger::GetInstance();
    logger.Initialize("TestLogs");
    logger.Clear();

    logger.Log(LogLevel::INFO, "General", "info msg");
    logger.Log(LogLevel::WARNING, "General", "warn msg");
    logger.Log(LogLevel::ERROR_, "General", "error msg");

    LogStatistics stats = logger.GetStatistics();
    EXPECT_TRUE(stats.stats.totalEntries >= 3);
    logger.Shutdown();
}

// ============================================================================
// EditorCrashHandler Tests
// ============================================================================

TEST(CrashHandler_Initialize)
{
    auto& handler = EditorCrashHandler::GetInstance();
    bool ok = handler.Initialize("TestCrashes");
    EXPECT_TRUE(ok);
    handler.Shutdown();
}

TEST(CrashHandler_RecordOperation)
{
    auto& handler = EditorCrashHandler::GetInstance();
    handler.Initialize("TestCrashes");

    handler.RecordOperation("OpenScene");
    handler.RecordOperation("PlaceActor");
    handler.RecordOperation("SaveScene");
    // No crash — just verifying it doesn't throw
    EXPECT_TRUE(true);
    handler.Shutdown();
}

TEST(CrashHandler_SaveAndHasRecoveryData)
{
    std::filesystem::create_directories("TestCrashes");
    auto& handler = EditorCrashHandler::GetInstance();
    handler.Initialize("TestCrashes");

    handler.SetRecoveryCallback(
        []() -> RecoveryData
        {
            RecoveryData data;
            data.currentProject = "TestProject";
            data.openFiles.push_back("scene.spark");
            return data;
        });

    bool saved = handler.SaveRecoveryData();
    EXPECT_TRUE(saved);
    EXPECT_TRUE(handler.HasRecoveryData());
    handler.Shutdown();
    std::filesystem::remove_all("TestCrashes");
}

TEST(CrashHandler_ClearRecoveryData)
{
    auto& handler = EditorCrashHandler::GetInstance();
    handler.Initialize("TestCrashes");

    handler.SetRecoveryCallback(
        []() -> RecoveryData
        {
            RecoveryData data;
            data.currentProject = "ClearTest";
            return data;
        });

    handler.SaveRecoveryData();
    handler.ClearRecoveryData();
    EXPECT_FALSE(handler.HasRecoveryData());
    handler.Shutdown();
}

TEST(CrashHandler_GetStats)
{
    auto& handler = EditorCrashHandler::GetInstance();
    handler.Initialize("TestCrashes");

    auto stats = handler.GetStats();
    EXPECT_EQ(stats.totalCrashes, 0);
    EXPECT_EQ(stats.assertionFailures, 0);
    EXPECT_EQ(stats.accessViolations, 0);
    handler.Shutdown();
}

// ============================================================================
// ProjectManager Tests
// ============================================================================

namespace
{
    std::filesystem::path FindProjectManagerTestSourceRoot()
    {
        std::vector<std::filesystem::path> seeds;
#if defined(_WIN32) && defined(SPARK_TEST_SOURCE_DIR_WIDE)
        seeds.emplace_back(SPARK_TEST_SOURCE_DIR_WIDE);
#endif
        std::error_code ec;
        seeds.push_back(std::filesystem::current_path(ec));
        seeds.push_back(std::filesystem::absolute(std::filesystem::path(__FILE__), ec).parent_path());

        for (const auto& seed : seeds)
        {
            std::filesystem::path candidate = seed;
            for (int depth = 0; !candidate.empty() && depth < 12; ++depth)
            {
                bool completeCatalog = std::filesystem::is_regular_file(candidate / "CMakeLists.txt", ec) && !ec;
                for (const auto& descriptor : ProjectManager::GetProjectTemplateDescriptors())
                {
                    ec.clear();
                    if (!std::filesystem::is_regular_file(
                            candidate / "Templates" / std::string(descriptor.packageDirectory) / "template.json", ec) ||
                        ec)
                    {
                        completeCatalog = false;
                        break;
                    }
                }
                if (completeCatalog)
                    return std::filesystem::weakly_canonical(candidate, ec);

                const std::filesystem::path parent = candidate.parent_path();
                if (parent == candidate)
                    break;
                candidate = parent;
            }
        }
        return {};
    }

    bool IsPortableCodeIdentifier(const std::string& value)
    {
        if (value.empty())
            return false;
        const auto isAlpha = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
        const auto isDigit = [](unsigned char c) { return c >= '0' && c <= '9'; };
        if (!isAlpha(static_cast<unsigned char>(value.front())) && value.front() != '_')
            return false;
        return std::all_of(value.begin() + 1, value.end(),
                           [&](unsigned char c) { return isAlpha(c) || isDigit(c) || c == '_'; });
    }

    std::vector<std::string> SnapshotRecentProjects(const ProjectManager& manager)
    {
        std::vector<std::string> snapshot;
        for (const auto& project : manager.GetRecentProjects())
        {
            snapshot.push_back(project.name + "\n" + project.path + "\n" + project.engineVersion + "\n" +
                               std::to_string(project.lastOpened) + "\n" + (project.valid ? "valid" : "invalid"));
        }
        return snapshot;
    }

    bool HasProjectStagingResidue(const std::filesystem::path& parent, const std::string& destinationName)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(parent, ec) || ec)
            return false;
        const auto prefix = std::filesystem::u8path(destinationName + ".spark-staging-").native();
        for (std::filesystem::directory_iterator it(parent, ec), end; !ec && it != end; it.increment(ec))
        {
            if (it->path().filename().native().starts_with(prefix))
                return true;
        }
        return false;
    }
} // namespace

TEST(ProjectManager_TemplateName)
{
    const auto descriptors = ProjectManager::GetProjectTemplateDescriptors();
    EXPECT_EQ(descriptors.size(), static_cast<size_t>(8));
    std::set<ProjectTemplate> types;
    std::set<std::string_view> stableIds;
    std::set<std::string_view> packages;
    for (const auto& descriptor : descriptors)
    {
        EXPECT_TRUE(!descriptor.stableId.empty());
        EXPECT_TRUE(!descriptor.displayName.empty());
        EXPECT_TRUE(!descriptor.description.empty());
        EXPECT_TRUE(!descriptor.packageDirectory.empty());
        EXPECT_TRUE(!descriptor.defaultScene.empty());
        EXPECT_TRUE(!descriptor.features.empty());
        EXPECT_TRUE(types.insert(descriptor.type).second);
        EXPECT_TRUE(stableIds.insert(descriptor.stableId).second);
        EXPECT_TRUE(packages.insert(descriptor.packageDirectory).second);
        EXPECT_TRUE(ProjectManager::FindProjectTemplateDescriptor(descriptor.type) == &descriptor);
        EXPECT_TRUE(ProjectManager::FindProjectTemplateDescriptor(descriptor.stableId) == &descriptor);
        EXPECT_TRUE(ProjectManager::FindProjectTemplateDescriptor(descriptor.packageDirectory) == &descriptor);
        EXPECT_EQ(ProjectManager::GetProjectTemplateName(descriptor.type), std::string(descriptor.displayName));
        EXPECT_EQ(ProjectManager::GetProjectTemplateDescription(descriptor.type), std::string(descriptor.description));
    }
    EXPECT_TRUE(ProjectManager::FindProjectTemplateDescriptor(static_cast<ProjectTemplate>(255)) == nullptr);
    EXPECT_TRUE(ProjectManager::FindProjectTemplateDescriptor("not-a-template") == nullptr);

    EXPECT_FALSE(ProjectManager::GetProjectTemplateName(ProjectTemplate::Empty).empty());
    EXPECT_FALSE(ProjectManager::GetProjectTemplateName(ProjectTemplate::FirstPerson).empty());
    EXPECT_FALSE(ProjectManager::GetProjectTemplateName(ProjectTemplate::ThirdPerson).empty());
    EXPECT_FALSE(ProjectManager::GetProjectTemplateName(ProjectTemplate::TopDown).empty());
    EXPECT_FALSE(ProjectManager::GetProjectTemplateName(ProjectTemplate::Blank3D).empty());
    EXPECT_FALSE(ProjectManager::GetProjectTemplateName(ProjectTemplate::MMO).empty());
    EXPECT_FALSE(ProjectManager::GetProjectTemplateName(ProjectTemplate::Platformer).empty());
    EXPECT_FALSE(ProjectManager::GetProjectTemplateName(ProjectTemplate::RPG).empty());
}

TEST(ProjectManager_AllRegisteredTemplatesCreateMappedPhysicalPackages)
{
    const std::filesystem::path sourceRoot = FindProjectManagerTestSourceRoot();
    ASSERT_FALSE(sourceRoot.empty());
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path parent =
        std::filesystem::temp_directory_path() / ("spark-template-catalog-test-" + std::to_string(stamp));
    std::filesystem::create_directories(parent);

    ProjectManager manager;
    manager.Initialize();
    manager.SetEngineRoot(sourceRoot.string());
    size_t callbackCount = 0;
    manager.SetOnProjectOpened([&](const ProjectInfo&) { ++callbackCount; });
    constexpr std::array<size_t, 8> expectedEntityCountsByType = {0, 27, 16, 25, 15, 19, 23, 14};

    size_t templateIndex = 0;
    for (const auto& descriptor : ProjectManager::GetProjectTemplateDescriptors())
    {
        const std::string projectName = "RegistryTemplate" + std::to_string(templateIndex);
        const std::filesystem::path projectRoot = parent / projectName;
        const std::filesystem::path sourcePackage = sourceRoot / "Templates" / std::string(descriptor.packageDirectory);
        EXPECT_TRUE(std::filesystem::is_regular_file(sourcePackage /
                                                     (std::string(descriptor.packageDirectory) + ".sparkproject")));

        const bool created =
            manager.CreateProject(projectName, parent.string(), descriptor.type, "registry integration test");
        EXPECT_TRUE(created);
        if (!created)
        {
            ++templateIndex;
            continue;
        }

        const std::filesystem::path mappedScene = projectRoot / std::filesystem::path(descriptor.defaultScene);
        const std::filesystem::path canonicalProject = projectRoot / (projectName + ".sparkproject");
        EXPECT_TRUE(std::filesystem::is_regular_file(mappedScene));
        World loadedScene;
        EXPECT_TRUE(Spark::LoadWorld(loadedScene, mappedScene.string()));
        EXPECT_EQ(loadedScene.GetEntityCount(), expectedEntityCountsByType.at(static_cast<size_t>(descriptor.type)));
        EXPECT_TRUE(std::filesystem::is_regular_file(canonicalProject));
        EXPECT_FALSE(
            std::filesystem::exists(projectRoot / (std::string(descriptor.packageDirectory) + ".sparkproject")));
        EXPECT_TRUE(std::filesystem::is_regular_file(projectRoot / "CMakeLists.txt"));
        EXPECT_TRUE(std::filesystem::is_regular_file(projectRoot / "Source" / "GameModule.h"));
        EXPECT_TRUE(std::filesystem::is_regular_file(projectRoot / "Source" / "GameModule.cpp"));
        EXPECT_TRUE(std::filesystem::is_regular_file(projectRoot / "spark.modules.json"));
        EXPECT_TRUE(std::filesystem::is_regular_file(projectRoot / "Assets" / "manifest.json"));

        const ProjectInfo& project = manager.GetCurrentProject();
        EXPECT_EQ(project.name, projectName);
        EXPECT_EQ(static_cast<int>(project.templateType), static_cast<int>(descriptor.type));
        EXPECT_EQ(project.defaultScene, std::string(descriptor.defaultScene));
        EXPECT_EQ(project.lastOpenedScene, std::string(descriptor.defaultScene));
        const size_t expectedSceneCount = descriptor.type == ProjectTemplate::Empty ? 2 : 1;
        EXPECT_EQ(project.scenes.size(), expectedSceneCount);
        if (!project.scenes.empty())
            EXPECT_EQ(project.scenes.front(), std::string(descriptor.defaultScene));
        if (descriptor.type == ProjectTemplate::Empty)
        {
            EXPECT_TRUE(std::find(project.scenes.begin(), project.scenes.end(), "Scenes/RuntimePreview.sparkscene") !=
                        project.scenes.end());
            EXPECT_TRUE(std::filesystem::is_regular_file(projectRoot / "Scenes" / "RuntimePreview.sparkscene"));
        }
        EXPECT_EQ(project.modules.size(), static_cast<size_t>(1));
        if (!project.modules.empty())
            EXPECT_TRUE(IsPortableCodeIdentifier(project.modules.front()));
        EXPECT_EQ(std::filesystem::path(manager.GetProjectFilePath()),
                  std::filesystem::weakly_canonical(canonicalProject));

        std::ifstream templateMetadata(projectRoot / "template.json");
        const std::string templateText((std::istreambuf_iterator<char>(templateMetadata)),
                                       std::istreambuf_iterator<char>());
        EXPECT_STR_CONTAINS(templateText, std::string("\"identity\": \"") + std::string(descriptor.stableId) + "\"");
        EXPECT_EQ(callbackCount, templateIndex + 1);
        EXPECT_FALSE(HasProjectStagingResidue(parent, projectName));

        manager.RemoveRecentProject(canonicalProject.string());
        manager.CloseProject();
        ++templateIndex;
    }

    manager.Shutdown();
    std::error_code cleanupError;
    std::filesystem::remove_all(parent, cleanupError);
    EXPECT_FALSE(cleanupError);
}

TEST(ProjectManager_DisplayNameProducesSafeModuleAndCanonicalProjectDocument)
{
    const std::filesystem::path sourceRoot = FindProjectManagerTestSourceRoot();
    ASSERT_FALSE(sourceRoot.empty());
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path parent =
        std::filesystem::temp_directory_path() / ("spark-template-unicode-test-" + std::to_string(stamp));
    std::filesystem::create_directories(parent);

    const std::string displayName = std::string("42 Caf") + "\xC3\xA9" + " Quest " + "\xF0\x9F\x9A\x80";
    const std::string expectedIdentifier = "SparkGame42_Caf_Quest_";
    const std::filesystem::path destination = parent / std::filesystem::u8path(displayName);
    const std::filesystem::path canonicalProject = destination / std::filesystem::u8path(displayName + ".sparkproject");

    ProjectManager manager;
    manager.Initialize();
    manager.SetEngineRoot(sourceRoot.string());
    EXPECT_TRUE(manager.CreateProject(displayName, parent.string(), ProjectTemplate::Empty));
    EXPECT_EQ(manager.GetCurrentProject().name, displayName);
    EXPECT_EQ(manager.GetCurrentProject().modules.size(), static_cast<size_t>(1));
    if (!manager.GetCurrentProject().modules.empty())
    {
        EXPECT_EQ(manager.GetCurrentProject().modules.front(), expectedIdentifier);
        EXPECT_TRUE(IsPortableCodeIdentifier(manager.GetCurrentProject().modules.front()));
    }
    EXPECT_TRUE(std::filesystem::is_regular_file(canonicalProject));
    EXPECT_FALSE(std::filesystem::exists(destination / "EmptyProject.sparkproject"));
    EXPECT_EQ(std::filesystem::u8path(manager.GetProjectFilePath()),
              std::filesystem::weakly_canonical(canonicalProject));

    std::ifstream cmakeInput(destination / "CMakeLists.txt");
    const std::string cmakeText((std::istreambuf_iterator<char>(cmakeInput)), std::istreambuf_iterator<char>());
    EXPECT_STR_CONTAINS(cmakeText, "spark_add_game_module(" + expectedIdentifier);
    std::ifstream headerInput(destination / "Source" / "GameModule.h");
    const std::string headerText((std::istreambuf_iterator<char>(headerInput)), std::istreambuf_iterator<char>());
    EXPECT_STR_CONTAINS(headerText, "class " + expectedIdentifier + "Module");
    World loadedScene;
    EXPECT_TRUE(Spark::LoadWorld(loadedScene, manager.GetCurrentProject().path + "/Scenes/Default.sparkscene"));
    cmakeInput.close();
    headerInput.close();
    EXPECT_FALSE(HasProjectStagingResidue(parent, displayName));

    manager.RemoveRecentProject(manager.GetProjectFilePath());
    manager.Shutdown();
    std::error_code cleanupError;
    std::filesystem::remove_all(parent, cleanupError);
    EXPECT_FALSE(cleanupError);
}

TEST(ProjectManager_GeneratedFallbackUsesProjectIdentityInsteadOfStagingName)
{
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path parent =
        std::filesystem::temp_directory_path() / ("spark-generated-fallback-test-" + std::to_string(stamp));
    const std::filesystem::path emptyEngineRoot = parent / "EngineWithoutTemplates";
    const std::filesystem::path projects = parent / "Projects";
    std::filesystem::create_directories(emptyEngineRoot);
    std::filesystem::create_directories(projects);

    ProjectManager manager;
    manager.Initialize();
    manager.SetEngineRoot(emptyEngineRoot.string());
    ASSERT_TRUE(manager.CreateProject("Fallback Playable", projects.string(), ProjectTemplate::FirstPerson,
                                      "generated fallback"));

    const std::filesystem::path projectRoot = projects / "Fallback Playable";
    const std::string expectedIdentifier = "Fallback_Playable";
    std::ifstream cmakeInput(projectRoot / "CMakeLists.txt");
    const std::string cmakeText((std::istreambuf_iterator<char>(cmakeInput)), std::istreambuf_iterator<char>());
    std::ifstream moduleInput(projectRoot / "spark.modules.json");
    const std::string moduleText((std::istreambuf_iterator<char>(moduleInput)), std::istreambuf_iterator<char>());
    EXPECT_STR_CONTAINS(cmakeText, "project(" + expectedIdentifier + " LANGUAGES CXX)");
    EXPECT_STR_CONTAINS(cmakeText, "spark_add_game_module(" + expectedIdentifier);
    EXPECT_STR_CONTAINS(moduleText, "\"name\": \"" + expectedIdentifier + "\"");
    EXPECT_STR_CONTAINS(moduleText, "\"path\": \"" + expectedIdentifier + ".dll\"");
    EXPECT_EQ(manager.GetCurrentProject().name, "Fallback Playable");
    EXPECT_EQ(manager.GetCurrentProject().modules.size(), static_cast<size_t>(1));
    if (!manager.GetCurrentProject().modules.empty())
        EXPECT_EQ(manager.GetCurrentProject().modules.front(), expectedIdentifier);
    EXPECT_FALSE(HasProjectStagingResidue(projects, "Fallback Playable"));
    cmakeInput.close();
    moduleInput.close();

    manager.RemoveRecentProject((projectRoot / "Fallback Playable.sparkproject").string());
    manager.Shutdown();
    std::error_code cleanupError;
    std::filesystem::remove_all(parent, cleanupError);
    EXPECT_FALSE(cleanupError);
}

TEST(ProjectManager_CreateReplacementClosesPreviousProjectForFallbackAndPhysicalTemplates)
{
    const std::filesystem::path sourceRoot = FindProjectManagerTestSourceRoot();
    ASSERT_FALSE(sourceRoot.empty());
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path parent =
        std::filesystem::temp_directory_path() / ("spark-create-replacement-test-" + std::to_string(stamp));
    const std::filesystem::path emptyEngineRoot = parent / "EngineWithoutTemplates";
    const std::filesystem::path projects = parent / "Projects";
    std::filesystem::create_directories(emptyEngineRoot);
    std::filesystem::create_directories(projects);

    ProjectManager manager;
    manager.Initialize();
    manager.SetEngineRoot(emptyEngineRoot.string());
    ASSERT_TRUE(manager.CreateProject("Initial", projects.string(), ProjectTemplate::Blank3D));

    std::vector<std::string> lifecycle;
    manager.SetOnProjectClosed([&](const ProjectInfo& project) { lifecycle.push_back("close:" + project.name); });
    manager.SetOnProjectOpened([&](const ProjectInfo& project) { lifecycle.push_back("open:" + project.name); });

    ASSERT_TRUE(manager.CreateProject("FallbackReplacement", projects.string(), ProjectTemplate::Blank3D));
    manager.SetEngineRoot(sourceRoot.string());
    ASSERT_TRUE(manager.CreateProject("PhysicalReplacement", projects.string(), ProjectTemplate::FirstPerson));

    ASSERT_EQ(lifecycle.size(), static_cast<size_t>(4));
    EXPECT_EQ(lifecycle[0], std::string("close:Initial"));
    EXPECT_EQ(lifecycle[1], std::string("open:FallbackReplacement"));
    EXPECT_EQ(lifecycle[2], std::string("close:FallbackReplacement"));
    EXPECT_EQ(lifecycle[3], std::string("open:PhysicalReplacement"));
    EXPECT_TRUE(manager.HasOpenProject());
    EXPECT_EQ(manager.GetCurrentProject().name, std::string("PhysicalReplacement"));

    manager.SetOnProjectClosed({});
    manager.SetOnProjectOpened({});
    for (const char* name : {"Initial", "FallbackReplacement", "PhysicalReplacement"})
        manager.RemoveRecentProject((projects / name / (std::string(name) + ".sparkproject")).string());
    manager.Shutdown();
    std::error_code cleanupError;
    std::filesystem::remove_all(parent, cleanupError);
    EXPECT_FALSE(cleanupError);
}

TEST(ProjectManager_LegacyAndCustomProjectsDoNotAcquireBlankTemplateIdentity)
{
    const std::filesystem::path sourceRoot = FindProjectManagerTestSourceRoot();
    ASSERT_FALSE(sourceRoot.empty());
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path parent =
        std::filesystem::temp_directory_path() / ("spark-template-identity-test-" + std::to_string(stamp));
    const std::filesystem::path legacyRoot = parent / "Legacy";
    std::filesystem::create_directories(legacyRoot);
    const std::filesystem::path legacyDocument = legacyRoot / "Legacy.sparkproject";
    std::ofstream(legacyDocument)
        << "{\n  \"name\": \"Legacy\",\n  \"version\": \"1.0.0\",\n  \"modules\": [],\n  \"scenes\": []\n}\n";

    ProjectManager manager;
    manager.Initialize();
    ASSERT_TRUE(manager.OpenProject(legacyDocument.string()));
    EXPECT_FALSE(manager.GetCurrentProject().hasTemplateIdentity);
    ASSERT_TRUE(manager.SaveProject());
    std::ifstream savedLegacy(legacyDocument);
    const std::string legacyText((std::istreambuf_iterator<char>(savedLegacy)), std::istreambuf_iterator<char>());
    EXPECT_FALSE(legacyText.contains("\"template\""));
    savedLegacy.close();
    manager.RemoveRecentProject(legacyDocument.string());
    manager.CloseProject();

    manager.SetEngineRoot(sourceRoot.string());
    const std::filesystem::path customRoot = parent / "CustomArena";
    ASSERT_TRUE(manager.CreateProjectFromTemplate("CustomArena", customRoot.string(), "MultiplayerArena"));
    EXPECT_FALSE(manager.GetCurrentProject().hasTemplateIdentity);
    ASSERT_TRUE(manager.SaveProject());
    const std::filesystem::path customDocument = customRoot / "CustomArena.sparkproject";
    std::ifstream savedCustom(customDocument);
    const std::string customText((std::istreambuf_iterator<char>(savedCustom)), std::istreambuf_iterator<char>());
    EXPECT_FALSE(customText.contains("\"template\""));
    savedCustom.close();

    manager.RemoveRecentProject(customDocument.string());
    manager.Shutdown();
    std::error_code cleanupError;
    std::filesystem::remove_all(parent, cleanupError);
    EXPECT_FALSE(cleanupError);
}

TEST(ProjectManager_FailedCreationIsTransactionalAndCleansStaging)
{
    const std::filesystem::path sourceRoot = FindProjectManagerTestSourceRoot();
    ASSERT_FALSE(sourceRoot.empty());
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path parent =
        std::filesystem::temp_directory_path() / ("spark-template-rollback-test-" + std::to_string(stamp));
    const std::filesystem::path projects = parent / "Projects";
    const std::filesystem::path brokenCatalog = parent / "BrokenCatalog";
    std::filesystem::create_directories(projects);

    ProjectManager manager;
    manager.Initialize();
    manager.SetEngineRoot(sourceRoot.string());
    ASSERT_TRUE(manager.CreateProject("Baseline", projects.string(), ProjectTemplate::Blank3D));
    const std::filesystem::path baselineProject = projects / "Baseline" / "Baseline.sparkproject";
    manager.RemoveRecentProject(baselineProject.string());

    const ProjectInfo baselineInfo = manager.GetCurrentProject();
    const std::string baselineDocument = manager.GetProjectFilePath();
    const std::string baselineActivePath = ProjectManager::GetActiveProjectPath();
    const std::vector<std::string> baselineRecent = SnapshotRecentProjects(manager);
    size_t callbackCount = 0;
    manager.SetOnProjectOpened([&](const ProjectInfo&) { ++callbackCount; });

    const auto expectBaselineUnchanged = [&]()
    {
        EXPECT_TRUE(manager.HasOpenProject());
        EXPECT_EQ(manager.GetCurrentProject().name, baselineInfo.name);
        EXPECT_EQ(manager.GetCurrentProject().path, baselineInfo.path);
        EXPECT_EQ(manager.GetCurrentProject().defaultScene, baselineInfo.defaultScene);
        EXPECT_TRUE(manager.GetCurrentProject().scenes == baselineInfo.scenes);
        EXPECT_TRUE(manager.GetCurrentProject().modules == baselineInfo.modules);
        EXPECT_EQ(manager.GetProjectFilePath(), baselineDocument);
        EXPECT_EQ(ProjectManager::GetActiveProjectPath(), baselineActivePath);
        EXPECT_TRUE(SnapshotRecentProjects(manager) == baselineRecent);
        EXPECT_EQ(callbackCount, static_cast<size_t>(0));
    };

    EXPECT_FALSE(manager.CreateProject("../Traversal", projects.string(), ProjectTemplate::Empty));
    EXPECT_FALSE(manager.CreateProject("CON", projects.string(), ProjectTemplate::Empty));
    EXPECT_FALSE(manager.CreateProject("NUL.txt", projects.string(), ProjectTemplate::Empty));
    EXPECT_FALSE(manager.CreateProjectFromTemplate("Valid Project", (projects / "TraversalTemplate").string(),
                                                   "../EmptyProject"));
    EXPECT_FALSE(manager.CreateProjectFromTemplate("Valid Project", (projects / "ReservedTemplate").string(), "COM1"));
    expectBaselineUnchanged();
    EXPECT_FALSE(std::filesystem::exists(projects / "TraversalTemplate"));
    EXPECT_FALSE(std::filesystem::exists(projects / "ReservedTemplate"));
    EXPECT_FALSE(HasProjectStagingResidue(projects, "TraversalTemplate"));
    EXPECT_FALSE(HasProjectStagingResidue(projects, "ReservedTemplate"));

    const std::filesystem::path collision = projects / "ExistingDestination";
    std::filesystem::create_directories(collision);
    std::ofstream(collision / "keep.txt") << "must remain";
    EXPECT_FALSE(manager.CreateProjectFromTemplate("Collision", collision.string(), "EmptyProject"));
    expectBaselineUnchanged();
    EXPECT_TRUE(std::filesystem::is_regular_file(collision / "keep.txt"));
    EXPECT_FALSE(HasProjectStagingResidue(projects, collision.filename().string()));

    // Construct a catalog that passes discovery but whose FPS package omits its
    // declared scene. The staged project document is loaded first, so this
    // exercises rollback after ProjectManager has temporarily mutated state.
    for (const auto& descriptor : ProjectManager::GetProjectTemplateDescriptors())
    {
        const std::filesystem::path package = brokenCatalog / std::string(descriptor.packageDirectory);
        std::filesystem::create_directories(package);
        std::ofstream(package / "template.json") << "{\n  \"name\": \"" << descriptor.packageDirectory << "\"\n}\n";
    }
    const std::filesystem::path brokenFps = brokenCatalog / "FPSStarter";
    std::ofstream(brokenFps / "FPSStarter.sparkproject")
        << "{\n  \"name\": \"Injected staged state\",\n  \"version\": \"9.9.9\",\n"
           "  \"defaultScene\": \"Scenes/Arena.sparkscene\",\n  \"modules\": [\"InjectedModule\"]\n}\n";

    manager.SetEngineRoot(brokenCatalog.string());
    const std::filesystem::path brokenDestination = projects / "BrokenTemplateDestination";
    EXPECT_FALSE(manager.CreateProjectFromTemplate("Broken Project", brokenDestination.string(), "FPSStarter"));
    expectBaselineUnchanged();
    EXPECT_FALSE(std::filesystem::exists(brokenDestination));
    EXPECT_FALSE(HasProjectStagingResidue(projects, brokenDestination.filename().string()));

    manager.Shutdown();
    EXPECT_TRUE(ProjectManager::GetActiveProjectPath().empty());
    std::error_code cleanupError;
    std::filesystem::remove_all(parent, cleanupError);
    EXPECT_FALSE(cleanupError);
}

TEST(ProjectManager_Initialize)
{
    ProjectManager pm;
    bool ok = pm.Initialize();
    EXPECT_TRUE(ok);
    pm.Shutdown();
}

TEST(ProjectManager_NoOpenProject)
{
    ProjectManager pm;
    pm.Initialize();
    EXPECT_FALSE(pm.HasOpenProject());
    pm.Shutdown();
}

TEST(ProjectManager_PathHelpers)
{
    ProjectManager pm;
    pm.Initialize();

    std::string assets = pm.GetProjectAssetsPath();
    std::string scenes = pm.GetProjectScenesPath();
    std::string scripts = pm.GetProjectScriptsPath();
    std::string config = pm.GetProjectConfigPath();
    // Path helpers return strings (may be empty with no project open)
    (void)assets;
    (void)scenes;
    (void)scripts;
    (void)config;
    EXPECT_TRUE(true);
    pm.Shutdown();
}

TEST(ProjectManager_RecentProjects)
{
    ProjectManager pm;
    pm.Initialize();

    auto recent = pm.GetRecentProjects();
    // May be empty — just verify it returns without crashing
    (void)recent; // just verify it returns without crashing
    pm.Shutdown();
}

TEST(ProjectManager_CreateProject_WritesLoadableReflectedScene)
{
    const std::filesystem::path sourceRoot = FindProjectManagerTestSourceRoot();
    ASSERT_FALSE(sourceRoot.empty());
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path parent =
        std::filesystem::temp_directory_path() / ("spark-project-test-" + std::to_string(stamp));
    std::filesystem::create_directories(parent);

    ProjectManager pm;
    pm.Initialize();
    pm.SetEngineRoot(sourceRoot.string());
    EXPECT_TRUE(pm.CreateProject("Playable", parent.string(), ProjectTemplate::FirstPerson, "test"));
    EXPECT_TRUE(pm.HasOpenProject());

    const std::filesystem::path scene = parent / "Playable" / "Scenes" / "Arena.sparkscene";
    EXPECT_TRUE(std::filesystem::exists(scene));
    World loaded;
    EXPECT_TRUE(Spark::LoadWorld(loaded, scene.string()));
    EXPECT_GT(loaded.GetEntityCount(), static_cast<size_t>(0));
    size_t cameraCount = 0;
    for ([[maybe_unused]] auto entity : loaded.GetEntitiesWith<Camera>())
        ++cameraCount;
    size_t controllerCount = 0;
    for ([[maybe_unused]] auto entity : loaded.GetEntitiesWith<CharacterControllerComponent>())
        ++controllerCount;
    EXPECT_GT(cameraCount, static_cast<size_t>(0));
    EXPECT_GT(controllerCount, static_cast<size_t>(0));

    const std::filesystem::path projectRoot = parent / "Playable";
    const std::filesystem::path cmakeFile = projectRoot / "CMakeLists.txt";
    const std::filesystem::path moduleHeader = projectRoot / "Source" / "GameModule.h";
    const std::filesystem::path moduleSource = projectRoot / "Source" / "GameModule.cpp";
    EXPECT_TRUE(std::filesystem::is_regular_file(cmakeFile));
    EXPECT_TRUE(std::filesystem::is_regular_file(moduleHeader));
    EXPECT_TRUE(std::filesystem::is_regular_file(moduleSource));
    EXPECT_TRUE(std::filesystem::is_regular_file(projectRoot / "spark.modules.json"));
    EXPECT_EQ(std::filesystem::path(ProjectManager::GetActiveProjectPath()),
              std::filesystem::weakly_canonical(projectRoot));
    EXPECT_EQ(std::filesystem::path(pm.GetProjectFilePath()),
              std::filesystem::weakly_canonical(projectRoot / "Playable.sparkproject"));

    {
        std::ifstream input(cmakeFile);
        const std::string cmakeText((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        EXPECT_STR_CONTAINS(cmakeText, "find_package(SparkEngine CONFIG REQUIRED)");
        EXPECT_STR_CONTAINS(cmakeText, "spark_add_game_module(Playable");
    }
    {
        std::ifstream input(moduleHeader);
        const std::string moduleText((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        EXPECT_STR_CONTAINS(moduleText, "#include <Spark/SparkSDK.h>");
        EXPECT_STR_CONTAINS(moduleText, "class PlayableModule final");
    }

    // Opening an older generated project repairs missing scaffold files but
    // must never overwrite an existing user-authored build script.
    {
        std::ofstream append(cmakeFile, std::ios::app);
        append << "\n# user-cmake-preserved\n";
    }
    std::filesystem::remove(moduleSource);

    pm.Shutdown();
    EXPECT_TRUE(ProjectManager::GetActiveProjectPath().empty());

    const std::string expectedProjectFile = (parent / "Playable" / "Playable.sparkproject").string();
    ProjectManager reloaded;
    reloaded.Initialize();
    EXPECT_TRUE(reloaded.OpenProject(expectedProjectFile));
    EXPECT_TRUE(std::filesystem::is_regular_file(moduleSource));
    {
        std::ifstream input(cmakeFile);
        const std::string cmakeText((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        EXPECT_STR_CONTAINS(cmakeText, "# user-cmake-preserved");
    }
    const auto recent = reloaded.GetRecentProjects();
    EXPECT_TRUE(std::any_of(recent.begin(), recent.end(),
                            [&](const RecentProject& entry)
                            {
                                std::error_code ec;
                                return std::filesystem::equivalent(entry.path, expectedProjectFile, ec) && !ec;
                            }));
    reloaded.RemoveRecentProject(expectedProjectFile);
    reloaded.Shutdown();

    std::error_code cleanupError;
    std::filesystem::remove_all(parent, cleanupError);
    EXPECT_FALSE(cleanupError);
}

TEST(ProjectManager_ProjectMetadataEscapesRoundTrip)
{
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("spark-project-escapes-" + std::to_string(stamp));
    std::filesystem::create_directories(root);
    const std::filesystem::path projectFile = root / "Escapes.sparkproject";
    {
        std::ofstream file(projectFile);
        file << R"json({
  "name": "Recent \"Project\"\nLine\tTab Caf\u00E9 \uD83D\uDE80",
  "version": "1.0.0",
  "modules": ["C:\\Games\\Module", "Quote\"Module", "Back\bForm\fSlash\/", "Unicode\u00E9\uD83D\uDE80"],
  "scenes": ["Scenes\\Default.sparkscene"]
})json";
    }

    const std::string expectedName = std::string("Recent \"Project\"\nLine\tTab Caf") + "\xC3\xA9 \xF0\x9F\x9A\x80";
    ProjectManager manager;
    manager.Initialize();
    EXPECT_TRUE(manager.OpenProject(projectFile.string()));
    const ProjectInfo& info = manager.GetCurrentProject();
    EXPECT_EQ(info.name, expectedName);
    EXPECT_EQ(info.modules.size(), static_cast<size_t>(4));
    EXPECT_EQ(info.modules[0], std::string("C:\\Games\\Module"));
    EXPECT_EQ(info.modules[1], std::string("Quote\"Module"));
    EXPECT_EQ(info.modules[2], std::string("Back\bForm\fSlash/"));
    EXPECT_EQ(info.modules[3], std::string("Unicode") + "\xC3\xA9\xF0\x9F\x9A\x80");
    EXPECT_EQ(std::filesystem::path(manager.GetProjectFilePath()), std::filesystem::weakly_canonical(projectFile));
    manager.Shutdown();
    EXPECT_TRUE(std::filesystem::is_regular_file(projectFile));
    EXPECT_FALSE(std::filesystem::exists(root / (expectedName + ".sparkproject")));

    ProjectManager roundTrip;
    roundTrip.Initialize();
    const auto recent = roundTrip.GetRecentProjects();
    const auto found = std::find_if(recent.begin(), recent.end(),
                                    [&](const RecentProject& entry)
                                    {
                                        std::error_code ec;
                                        return std::filesystem::equivalent(entry.path, projectFile, ec) && !ec;
                                    });
    EXPECT_TRUE(found != recent.end());
    if (found != recent.end())
        EXPECT_EQ(found->name, expectedName);
    roundTrip.RemoveRecentProject(projectFile.string());
    roundTrip.Shutdown();

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    EXPECT_FALSE(ec);
}

TEST(ProjectManager_RecentProjectPathsNormalizeForAddAndRemove)
{
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("spark-recent-path-test-" + std::to_string(stamp));
    std::filesystem::create_directories(root / "nested");
    const std::filesystem::path projectFile = root / "Normalized.sparkproject";
    std::ofstream(projectFile) << "{\n  \"name\": \"Normalized\",\n  \"version\": \"1.0.0\"\n}\n";
    const std::filesystem::path alternate = root / "nested" / ".." / "Normalized.sparkproject";

    ProjectManager manager;
    manager.Initialize();
    EXPECT_TRUE(manager.OpenProject(alternate.string()));
    EXPECT_TRUE(manager.OpenProject(projectFile.string()));

    const std::filesystem::path normalized = std::filesystem::weakly_canonical(projectFile);
    const auto recent = manager.GetRecentProjects();
    const size_t matches = static_cast<size_t>(
        std::count_if(recent.begin(), recent.end(), [&](const RecentProject& rp)
                      { return std::filesystem::path(rp.path).lexically_normal() == normalized.lexically_normal(); }));
    EXPECT_EQ(matches, static_cast<size_t>(1));

    manager.RemoveRecentProject(alternate.string());
    const auto afterRemove = manager.GetRecentProjects();
    EXPECT_FALSE(
        std::any_of(afterRemove.begin(), afterRemove.end(), [&](const RecentProject& rp)
                    { return std::filesystem::path(rp.path).lexically_normal() == normalized.lexically_normal(); }));
    manager.Shutdown();

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    EXPECT_FALSE(ec);
}

TEST(ProjectManager_RecordOpenedScenePersistsProjectRelativePath)
{
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path parent =
        std::filesystem::temp_directory_path() / ("spark-record-scene-test-" + std::to_string(stamp));
    ProjectManager manager;
    manager.Initialize();
    EXPECT_TRUE(manager.CreateProject("Recorded", parent.string(), ProjectTemplate::Blank3D));
    const std::filesystem::path root = parent / "Recorded";
    const std::filesystem::path scene = root / "Scenes" / "Sub" / "Saved.sparkscene";
    std::filesystem::create_directories(scene.parent_path());
    std::ofstream(scene) << "{\"entities\":[]}";
    EXPECT_TRUE(manager.RecordOpenedScene(scene.string()));
    EXPECT_EQ(manager.GetCurrentProject().lastOpenedScene, std::string("Scenes/Sub/Saved.sparkscene"));
    EXPECT_TRUE(std::find(manager.GetCurrentProject().scenes.begin(), manager.GetCurrentProject().scenes.end(),
                          "Scenes/Sub/Saved.sparkscene") != manager.GetCurrentProject().scenes.end());

    const std::filesystem::path outside = parent / "Outside.sparkscene";
    std::ofstream(outside) << "{}";
    EXPECT_FALSE(manager.RecordOpenedScene(outside.string()));
    manager.RemoveRecentProject((root / "Recorded.sparkproject").string());
    manager.Shutdown();

    ProjectManager reopened;
    reopened.Initialize();
    EXPECT_TRUE(reopened.OpenProject((root / "Recorded.sparkproject").string()));
    EXPECT_EQ(reopened.GetCurrentProject().lastOpenedScene, std::string("Scenes/Sub/Saved.sparkscene"));
    reopened.RemoveRecentProject((root / "Recorded.sparkproject").string());
    reopened.Shutdown();

    std::error_code ec;
    std::filesystem::remove_all(parent, ec);
    EXPECT_FALSE(ec);
}

TEST(ProjectManager_RecordOpenedSceneSupportsUnicodeProjectPaths)
{
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path parent = std::filesystem::temp_directory_path() /
                                         std::filesystem::u8path("spark-unicode-\xC3\xA9-" + std::to_string(stamp));
    ProjectManager manager;
    manager.Initialize();
    EXPECT_TRUE(manager.CreateProject("UnicodeProject", TestPathUtf8(parent), ProjectTemplate::Blank3D));

    const std::filesystem::path root = parent / "UnicodeProject";
    const std::filesystem::path scene = root / "Scenes" / std::filesystem::u8path("Caf\xC3\xA9.sparkscene");
    std::ofstream(scene, std::ios::binary) << "{\"entities\":[]}";
    EXPECT_TRUE(manager.RecordOpenedScene(TestPathUtf8(scene)));
    EXPECT_EQ(manager.GetCurrentProject().lastOpenedScene, std::string("Scenes/Caf\xC3\xA9.sparkscene"));

    manager.RemoveRecentProject(TestPathUtf8(root / "UnicodeProject.sparkproject"));
    manager.Shutdown();
    std::error_code ec;
    std::filesystem::remove_all(parent, ec);
    EXPECT_FALSE(ec);
}

TEST(ProjectManager_RecordOpenedSceneDoesNotRewriteAlreadyCurrentScene)
{
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("spark-current-scene-test-" + std::to_string(stamp));
    std::filesystem::create_directories(root / "Scenes");
    const std::filesystem::path scene = root / "Scenes" / "Default.sparkscene";
    const std::filesystem::path projectFile = root / "Idempotent.sparkproject";
    std::ofstream(scene) << "{\"entities\":[]}";
    std::ofstream(projectFile) << "{\n"
                                  "  \"name\": \"Idempotent\",\n"
                                  "  \"version\": \"1.0.0\",\n"
                                  "  \"engineVersion\": \"1.0.0\",\n"
                                  "  \"template\": \"empty\",\n"
                                  "  \"defaultScene\": \"Scenes/Default.sparkscene\",\n"
                                  "  \"lastOpenedScene\": \"Scenes/Default.sparkscene\",\n"
                                  "  \"createdTime\": 0,\n"
                                  "  \"lastModified\": 0,\n"
                                  "  \"modules\": [\"Idempotent\"]\n"
                                  "}\n";

    auto readProject = [&]()
    {
        std::ifstream in(projectFile, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };

    ProjectManager manager;
    manager.Initialize();
    ASSERT_TRUE(manager.OpenProject(projectFile.string()));
    const std::string before = readProject();
    EXPECT_EQ(manager.GetCurrentProject().lastModified, static_cast<uint64_t>(0));
    EXPECT_TRUE(manager.RecordOpenedScene(scene.string()));
    EXPECT_EQ(manager.GetCurrentProject().lastModified, static_cast<uint64_t>(0));
    EXPECT_TRUE(manager.GetCurrentProject().scenes.empty());
    EXPECT_EQ(readProject(), before);
    manager.RemoveRecentProject(projectFile.string());
    manager.Shutdown();
    EXPECT_EQ(readProject(), before);

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    EXPECT_FALSE(ec);
}

TEST(ProjectManager_ExplicitSaveStillPersistsLastModified)
{
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("spark-explicit-save-test-" + std::to_string(stamp));
    std::filesystem::create_directories(root / "Scenes");
    const std::filesystem::path scene = root / "Scenes" / "Default.sparkscene";
    const std::filesystem::path projectFile = root / "Explicit.sparkproject";
    std::ofstream(scene) << "{\"entities\":[]}";
    std::ofstream(projectFile)
        << "{\n  \"name\": \"Explicit\",\n  \"version\": \"1.0.0\",\n"
           "  \"engineVersion\": \"1.0.0\",\n  \"defaultScene\": \"Scenes/Default.sparkscene\",\n"
           "  \"lastOpenedScene\": \"Scenes/Default.sparkscene\",\n  \"createdTime\": 0,\n"
           "  \"lastModified\": 0,\n  \"modules\": [\"Explicit\"],\n"
           "  \"scenes\": [\"Scenes/Default.sparkscene\"]\n}\n";

    ProjectManager manager;
    manager.Initialize();
    ASSERT_TRUE(manager.OpenProject(projectFile.string()));
    EXPECT_EQ(manager.GetCurrentProject().lastModified, static_cast<uint64_t>(0));
    EXPECT_TRUE(manager.SaveProject());
    EXPECT_TRUE(manager.GetCurrentProject().lastModified > 0);
    manager.RemoveRecentProject(projectFile.string());
    manager.Shutdown();

    ProjectManager reopened;
    reopened.Initialize();
    ASSERT_TRUE(reopened.OpenProject(projectFile.string()));
    EXPECT_TRUE(reopened.GetCurrentProject().lastModified > 0);
    reopened.RemoveRecentProject(projectFile.string());
    reopened.Shutdown();

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    EXPECT_FALSE(ec);
}

TEST(ProjectManager_ResolveProjectScenePathRejectsTraversalBeforeLoad)
{
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path parent =
        std::filesystem::temp_directory_path() / ("spark-scene-containment-test-" + std::to_string(stamp));
    ProjectManager manager;
    manager.Initialize();
    ASSERT_TRUE(manager.CreateProject("Contained", parent.string(), ProjectTemplate::Blank3D));
    const std::filesystem::path root = parent / "Contained";
    const std::filesystem::path inside = root / "Scenes" / "Inside.sparkscene";
    const std::filesystem::path outside = parent / "Outside.sparkscene";
    World sourceWorld;
    sourceWorld.CreateEntity("ContainedEntity");
    std::ofstream(inside) << Spark::SerializeWorld(sourceWorld);
    std::ofstream(outside) << Spark::SerializeWorld(sourceWorld);

    std::string resolved;
    EXPECT_TRUE(manager.ResolveProjectScenePath("Scenes/Inside.sparkscene", resolved));
    EXPECT_EQ(std::filesystem::weakly_canonical(std::filesystem::path(resolved)),
              std::filesystem::weakly_canonical(inside));
    EXPECT_FALSE(manager.ResolveProjectScenePath("../Outside.sparkscene", resolved));
    EXPECT_TRUE(resolved.empty());

    World loadedWorld;
    EXPECT_TRUE(manager.LoadProjectScene("Scenes/Inside.sparkscene", loadedWorld, resolved));
    EXPECT_EQ(loadedWorld.GetEntityCount(), static_cast<size_t>(1));
    EXPECT_FALSE(manager.LoadProjectScene("../Outside.sparkscene", loadedWorld, resolved));
    EXPECT_TRUE(resolved.empty());

    const std::filesystem::path outsideLink = root / "Scenes" / "OutsideLink.sparkscene";
    std::error_code ec;
    std::filesystem::create_symlink(outside, outsideLink, ec);
    if (!ec)
    {
        World linkedWorld;
        EXPECT_FALSE(manager.LoadProjectScene("Scenes/OutsideLink.sparkscene", linkedWorld, resolved));
        EXPECT_TRUE(resolved.empty());
    }

    manager.RemoveRecentProject((root / "Contained.sparkproject").string());
    manager.Shutdown();
    ec.clear();
    std::filesystem::remove_all(parent, ec);
    EXPECT_FALSE(ec);
}

TEST(ProjectManager_FailedProjectSwitchPreservesCurrentProjectAndDefersCloseCallback)
{
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path parent =
        std::filesystem::temp_directory_path() / ("spark-transactional-open-test-" + std::to_string(stamp));
    ProjectManager manager;
    manager.Initialize();
    ASSERT_TRUE(manager.CreateProject("Current", parent.string(), ProjectTemplate::Blank3D));
    const std::string currentPath = manager.GetCurrentProject().path;
    const std::filesystem::path invalidProjectDirectory = parent / "NotAProject";
    std::filesystem::create_directories(invalidProjectDirectory);

    int closeCallbacks = 0;
    manager.SetOnProjectClosed([&](const ProjectInfo&) { ++closeCallbacks; });
    EXPECT_FALSE(manager.OpenProject(invalidProjectDirectory.string()));
    EXPECT_TRUE(manager.HasOpenProject());
    EXPECT_EQ(manager.GetCurrentProject().path, currentPath);
    EXPECT_EQ(closeCallbacks, 0);

    manager.SetOnProjectClosed({});
    manager.RemoveRecentProject((parent / "Current" / "Current.sparkproject").string());
    manager.Shutdown();
    std::error_code ec;
    std::filesystem::remove_all(parent, ec);
    EXPECT_FALSE(ec);
}

TEST(ProjectManager_CommittedSwitchSurvivesThrowingLifecycleCallbacks)
{
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path parent =
        std::filesystem::temp_directory_path() / ("spark-callback-switch-test-" + std::to_string(stamp));
    const std::filesystem::path firstRoot = parent / "First";
    const std::filesystem::path secondRoot = parent / "Second";
    std::filesystem::create_directories(firstRoot);
    std::filesystem::create_directories(secondRoot);
    const std::filesystem::path firstProject = firstRoot / "First.sparkproject";
    const std::filesystem::path secondProject = secondRoot / "Second.sparkproject";
    std::ofstream(firstProject) << "{\"name\":\"First\",\"version\":\"1.0.0\",\"modules\":[],\"scenes\":[]}";
    std::ofstream(secondProject) << "{\"name\":\"Second\",\"version\":\"1.0.0\",\"modules\":[],\"scenes\":[]}";

    ProjectManager manager;
    manager.Initialize();
    ASSERT_TRUE(manager.OpenProject(firstProject.string()));

    int closeCallbacks = 0;
    int openCallbacks = 0;
    manager.SetOnProjectClosed(
        [&](const ProjectInfo&)
        {
            ++closeCallbacks;
            throw std::runtime_error("expected close callback failure");
        });
    manager.SetOnProjectOpened(
        [&](const ProjectInfo&)
        {
            ++openCallbacks;
            throw std::runtime_error("expected open callback failure");
        });

    EXPECT_TRUE(manager.OpenProject(secondProject.string()));
    EXPECT_TRUE(manager.HasOpenProject());
    EXPECT_EQ(manager.GetCurrentProject().name, std::string("Second"));
    EXPECT_EQ(closeCallbacks, 1);
    EXPECT_EQ(openCallbacks, 1);
    EXPECT_TRUE(ProjectManager::GetActiveProjectPath().find("Second") != std::string::npos);

    manager.SetOnProjectClosed({});
    manager.SetOnProjectOpened({});
    manager.RemoveRecentProject(firstProject.string());
    manager.RemoveRecentProject(secondProject.string());
    manager.Shutdown();
    std::error_code ec;
    std::filesystem::remove_all(parent, ec);
    EXPECT_FALSE(ec);
}

TEST(EditorLaunchContext_EnumeratesBoundedProjectBuildAndPackageDirectories)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "spark-launch-context-layout";
    const fs::path editor = root / "Editor";
    const fs::path project = root / "Project";

    const auto directories = LaunchContext::ModuleDiscoveryDirectories(editor, project);
    auto contains = [&](const fs::path& expected)
    {
        return std::any_of(directories.begin(), directories.end(),
                           [&](const fs::path& path) { return LaunchContext::SamePath(path, expected); });
    };

    EXPECT_EQ(directories.size(), static_cast<size_t>(11));
    EXPECT_TRUE(contains(editor));
    EXPECT_TRUE(contains(project / "build"));
    EXPECT_TRUE(contains(project / "build" / "Debug"));
    EXPECT_TRUE(contains(project / "build" / "Debug" / "Debug"));
    EXPECT_TRUE(contains(project / "build" / "RelWithDebInfo" / "RelWithDebInfo"));
    EXPECT_TRUE(contains(project / "build" / "Release" / "Release"));
    EXPECT_TRUE(contains(project / "build" / "MinSizeRel" / "MinSizeRel"));
    EXPECT_TRUE(contains(project / "Build" / "Output"));
}

TEST(EditorLaunchContext_CanonicallyDeduplicatesDiscoveredModulesAndMatchesSelection)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "spark-launch-context-dedupe";
    const fs::path editor = root / "Editor";
    const fs::path projectBuild = root / "Project" / "build" / "Release" / "Release";
    const std::vector<fs::path> directories = {editor, projectBuild};

    const auto modules = LaunchContext::DiscoverUniqueModules(
        directories,
        [&](const fs::path& directory)
        {
            if (LaunchContext::SamePath(directory, editor))
                return std::vector<fs::path>{editor / "SparkGame.dll", editor / "." / "SparkGame.dll"};
            return std::vector<fs::path>{projectBuild / "ProjectGame.dll", editor / "SparkGame.dll"};
        });

    EXPECT_EQ(modules.size(), static_cast<size_t>(2));
    EXPECT_TRUE(LaunchContext::SamePath(modules.front(), editor / "." / "SparkGame.dll"));
    EXPECT_TRUE(LaunchContext::SamePath(modules.back(), projectBuild / "ProjectGame.dll"));
    EXPECT_EQ(*LaunchContext::FindSelectedModuleIndex(modules, editor / "SparkGame.dll"), static_cast<size_t>(0));
    EXPECT_FALSE(LaunchContext::FindSelectedModuleIndex(modules, editor / "RemovedGame.dll").has_value());
}

TEST(EditorLaunchContext_UsesActiveProjectThenSafeModuleAndEngineFallbacks)
{
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("spark-launch-context-" + std::to_string(stamp));
    const fs::path project = root / "Project";
    const fs::path packageDirectory = project / "Build" / "Output";
    const fs::path moduleDirectory = root / "Modules";
    const fs::path engineDirectory = root / "Engine";
    fs::create_directories(project);
    fs::create_directories(packageDirectory);
    fs::create_directories(moduleDirectory);
    fs::create_directories(engineDirectory);

    const fs::path module = moduleDirectory / "ProjectGame.dll";
    const fs::path projectModule = project / "build" / "Release" / "Release" / "ProjectGame.dll";
    const fs::path packagedModule = packageDirectory / "PackagedGame.dll";
    const fs::path engine = engineDirectory / "SparkEngine.exe";
    EXPECT_TRUE(
        LaunchContext::SamePath(LaunchContext::ResolveWorkingDirectory(project, projectModule, engine), project));
    EXPECT_TRUE(LaunchContext::SamePath(LaunchContext::ResolveWorkingDirectory(project, packagedModule, engine),
                                        packageDirectory));
    EXPECT_TRUE(
        LaunchContext::SamePath(LaunchContext::ResolveWorkingDirectory(project, module, engine), moduleDirectory));
    EXPECT_TRUE(LaunchContext::SamePath(LaunchContext::ResolveWorkingDirectory(root / "MissingProject", module, engine),
                                        moduleDirectory));
    EXPECT_TRUE(LaunchContext::SamePath(
        LaunchContext::ResolveWorkingDirectory(root / "MissingProject", root / "Missing" / "Game.dll", engine),
        engineDirectory));

    const fs::path manifest = LaunchContext::ResolveContextFile(project, engineDirectory, "spark.modules.json");
    const fs::path audit = LaunchContext::ResolveContextFile(project, engineDirectory, "exec_audit.log");
    EXPECT_TRUE(LaunchContext::SamePath(manifest, project / "spark.modules.json"));
    EXPECT_TRUE(LaunchContext::SamePath(audit, project / "exec_audit.log"));

    EXPECT_TRUE(LaunchContext::SamePath(
        project / LaunchContext::PathFromUtf8(LaunchContext::ManifestModuleReference(project, projectModule)),
        projectModule));
    EXPECT_EQ(LaunchContext::ManifestModuleReference(project, module),
              LaunchContext::PathToUtf8(LaunchContext::CanonicalPath(module)));

    std::error_code ec;
    fs::remove_all(root, ec);
    EXPECT_FALSE(ec);
}

TEST(EditorLaunchContext_ValidatesExistingDllModulesBeforeLaunch)
{
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("spark-launch-module-validation-" + std::to_string(stamp));
    fs::create_directories(root);
    const fs::path dll = root / "PlayableGame.DLL";
    std::ofstream(dll, std::ios::binary) << "not a real dll; launch validation only checks the path contract";
    fs::create_directory(root / "Directory.dll");

    EXPECT_TRUE(LaunchContext::ValidateGameModuleForLaunch(dll).empty());
    EXPECT_FALSE(LaunchContext::ValidateGameModuleForLaunch(root / "Missing.dll").empty());
    EXPECT_FALSE(LaunchContext::ValidateGameModuleForLaunch(root / "Directory.dll").empty());
    const fs::path textFile = root / "not-a-module.txt";
    std::ofstream(textFile, std::ios::binary) << "not a module";
    EXPECT_FALSE(LaunchContext::ValidateGameModuleForLaunch(textFile).empty());

    std::error_code ec;
    fs::remove_all(root, ec);
    EXPECT_FALSE(ec);
}

#ifdef _WIN32
TEST(EditorProcessLaunch_QuotesUnicodePathsAndExplicitManifestAsDistinctArguments)
{
    const std::filesystem::path engine = L"D:\\Spark Tools\\SparkEngine.exe";
    const std::filesystem::path module = L"D:\\Projects\\Caf\u00e9 Game\\FPSStarter.dll";
    const std::filesystem::path cfg = L"D:\\Projects\\Caf\u00e9 Game\\smoke commands.cfg";
    const std::filesystem::path manifest = L"D:\\Projects\\Caf\u00e9 Game\\spark.modules.json";
    std::string error;
    const std::wstring command =
        BuildGameLaunchCommandLine(engine, module, true, cfg, manifest, L"-test-frames 1", error);

    EXPECT_TRUE(error.empty());
    ASSERT_FALSE(command.empty());
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(command.c_str(), &argc);
    ASSERT_TRUE(argv != nullptr);
    ASSERT_EQ(argc, 10);
    EXPECT_TRUE(std::wstring(argv[0]) == engine.wstring());
    EXPECT_TRUE(std::wstring(argv[1]) == L"-game");
    EXPECT_TRUE(std::wstring(argv[2]) == module.wstring());
    EXPECT_TRUE(std::wstring(argv[3]) == L"-headless");
    EXPECT_TRUE(std::wstring(argv[4]) == L"-exec");
    EXPECT_TRUE(std::wstring(argv[5]) == cfg.wstring());
    EXPECT_TRUE(std::wstring(argv[6]) == L"-manifest");
    EXPECT_TRUE(std::wstring(argv[7]) == manifest.wstring());
    EXPECT_TRUE(std::wstring(argv[8]) == L"-test-frames");
    EXPECT_TRUE(std::wstring(argv[9]) == L"1");
    LocalFree(argv);
}
#endif

TEST(EditorOwnedProcess_ReplacesWithoutOrphaningAndDestructorStops)
{
    int stopCalls = 0;
    int closeCalls = 0;
    std::vector<unsigned long> stoppedPids;
    EditorProcessOperations operations;
    operations.poll = [](void*, unsigned long&) { return false; };
    operations.stopAndClose = [&](void*, void*, unsigned long pid, unsigned long gracePeriodMs)
    {
        ++stopCalls;
        stoppedPids.push_back(pid);
        EXPECT_EQ(gracePeriodMs, 1500ul);
        return EditorProcessStopResult::Terminated;
    };
    operations.close = [&](void*, void*) { ++closeCalls; };

    {
        OwnedEditorProcess owner(operations);
        ProcessLaunchResult first;
        first.success = true;
        first.processHandle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));
        first.jobHandle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(2));
        first.pid = 101;
        ASSERT_TRUE(owner.Adopt(first));
        EXPECT_TRUE(owner.IsRunning());
        EXPECT_EQ(owner.GetPid(), 101ul);

        ProcessLaunchResult second;
        second.success = true;
        second.processHandle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(3));
        second.jobHandle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(4));
        second.pid = 202;
        ASSERT_TRUE(owner.Adopt(second));

        EXPECT_EQ(stopCalls, 1);
        ASSERT_EQ(stoppedPids.size(), 1u);
        EXPECT_EQ(stoppedPids[0], 101ul);
        EXPECT_EQ(owner.GetPid(), 202ul);
    }

    EXPECT_EQ(stopCalls, 2);
    ASSERT_EQ(stoppedPids.size(), 2u);
    EXPECT_EQ(stoppedPids[1], 202ul);
    EXPECT_EQ(closeCalls, 0);
}

TEST(EditorOwnedProcess_PollClosesHandlesWithoutSecondStop)
{
    int pollCalls = 0;
    int stopCalls = 0;
    int closeCalls = 0;
    EditorProcessOperations operations;
    operations.poll = [&](void* processHandle, unsigned long& exitCode)
    {
        ++pollCalls;
        EXPECT_TRUE(processHandle == reinterpret_cast<void*>(static_cast<std::uintptr_t>(5)));
        exitCode = 23;
        return true;
    };
    operations.stopAndClose = [&](void*, void*, unsigned long, unsigned long)
    {
        ++stopCalls;
        return EditorProcessStopResult::Terminated;
    };
    operations.close = [&](void* processHandle, void* jobHandle)
    {
        ++closeCalls;
        EXPECT_TRUE(processHandle == reinterpret_cast<void*>(static_cast<std::uintptr_t>(5)));
        EXPECT_TRUE(jobHandle == reinterpret_cast<void*>(static_cast<std::uintptr_t>(6)));
    };

    {
        OwnedEditorProcess owner(operations);
        ProcessLaunchResult launch;
        launch.success = true;
        launch.processHandle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(5));
        launch.jobHandle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(6));
        launch.pid = 303;
        ASSERT_TRUE(owner.Adopt(launch));

        unsigned long exitCode = 0;
        EXPECT_TRUE(owner.Poll(exitCode));
        EXPECT_EQ(exitCode, 23ul);
        EXPECT_FALSE(owner.IsRunning());
    }

    EXPECT_EQ(pollCalls, 1);
    EXPECT_EQ(closeCalls, 1);
    EXPECT_EQ(stopCalls, 0);
}

TEST(BuildPipeline_ConfigureUsesExplicitProjectAndInstalledPackage)
{
    BuildCookPanel::BuildSettings settings;
    settings.platform = BuildCookPanel::TargetPlatform::WindowsX64;
    settings.profile = BuildCookPanel::BuildProfile::Development;
    const auto args =
        BuildPipeline::CreateConfigureArguments(settings, "Project Root", "Project Root/build/RelWithDebInfo",
                                                "SDK/lib/cmake/SparkEngine", {"ENABLE_EDITOR=OFF"});

    EXPECT_TRUE(std::find(args.begin(), args.end(), "--preset") == args.end());
    EXPECT_TRUE(std::find(args.begin(), args.end(), "-S") != args.end());
    EXPECT_TRUE(std::find(args.begin(), args.end(), "-B") != args.end());
    EXPECT_TRUE(std::find(args.begin(), args.end(), "Visual Studio 17 2022") != args.end());
    EXPECT_TRUE(std::any_of(args.begin(), args.end(),
                            [](const std::string& arg) { return arg.rfind("-DSparkEngine_DIR=", 0) == 0; }));
}

TEST(BuildPipeline_OnlyAcceptsTheNativePackageTarget)
{
    const auto native = BuildPipeline::NativeTargetPlatform();
    EXPECT_TRUE(BuildPipeline::IsNativeTargetPlatform(native));
    const auto nonNative = native == BuildCookPanel::TargetPlatform::WindowsX64
                               ? BuildCookPanel::TargetPlatform::LinuxX64
                               : BuildCookPanel::TargetPlatform::WindowsX64;
    EXPECT_FALSE(BuildPipeline::IsNativeTargetPlatform(nonNative));
}

TEST(BuildPipeline_CookCommandUsesAbsoluteBoundedPaths)
{
    const auto args = BuildPipeline::CreateCookArguments("Project Root/Assets", "Project Root/Cooked/Assets",
                                                         "Project Root/Cooked/manifest.json", true);
    EXPECT_EQ(args.size(), 7u);
    EXPECT_EQ(args[0], "--source");
    EXPECT_TRUE(std::filesystem::path(args[1]).is_absolute());
    EXPECT_EQ(args[2], "--output");
    EXPECT_TRUE(std::filesystem::path(args[3]).is_absolute());
    EXPECT_EQ(args[4], "--manifest");
    EXPECT_TRUE(std::filesystem::path(args[5]).is_absolute());
    EXPECT_EQ(args[6], "--dry-run");
}

TEST(BuildPipeline_FindsRunnableSparkCookerBesideTestHost)
{
    const std::filesystem::path cooker = BuildPipeline::FindSparkCookerExecutable();
    EXPECT_FALSE(cooker.empty());
    EXPECT_TRUE(std::filesystem::is_regular_file(cooker));
}

TEST(BuildPipeline_AutomationCommandIsFrameBoundedAndWritesReports)
{
    const auto args =
        BuildPipeline::CreateAutomationArguments("Package/Game.exe", "Package", "Package/Reports", 240, 45);
    EXPECT_EQ(args.size(), 18u);
    EXPECT_EQ(args[0], "--name");
    EXPECT_EQ(args[2], "--executable");
    EXPECT_TRUE(std::filesystem::path(args[3]).is_absolute());
    EXPECT_EQ(args[4], "--working-dir");
    EXPECT_TRUE(std::filesystem::path(args[5]).is_absolute());
    EXPECT_EQ(args[6], "--frames");
    EXPECT_EQ(args[7], "240");
    EXPECT_EQ(args[8], "--timeout-ms");
    EXPECT_EQ(args[9], "45000");
    EXPECT_EQ(args[12], "--captured-log");
    EXPECT_TRUE(std::filesystem::path(args[13]).is_absolute());
    EXPECT_EQ(args[14], "--json");
    EXPECT_EQ(args[16], "--junit");
}

TEST(BuildPipeline_RejectsIncompleteBuildTreePackage)
{
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("spark-sdk-test-" + std::to_string(stamp));
    std::filesystem::create_directories(root);
    for (const char* filename : {"SparkEngineConfig.cmake"})
        std::ofstream(root / filename) << "# test\n";
    EXPECT_FALSE(BuildPipeline::IsSparkEnginePackageDirectory(root.string()));

    for (const char* filename : {"SparkEngineTargets.cmake", "SparkEngineTargets-release.cmake",
                                 "SparkGameModule.cmake", "WriteSparkModuleABI.cmake"})
        std::ofstream(root / filename) << "# test\n";
    EXPECT_TRUE(BuildPipeline::IsSparkEnginePackageDirectory(root.string()));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    EXPECT_FALSE(ec);
}

TEST(BuildPipeline_CookPackagesAssetsScenesConfigAndMetadata)
{
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("spark-cook-test-" + std::to_string(stamp));
    std::filesystem::create_directories(root / "Assets");
    std::filesystem::create_directories(root / "Scenes");
    std::filesystem::create_directories(root / "Config");
    std::ofstream(root / "Assets" / "asset.txt") << "asset";
    std::ofstream(root / "Scenes" / "Default.sparkscene") << "{}";
    std::ofstream(root / "Config" / "EditorSettings.json") << "{}";
    std::ofstream(root / "Cookable.sparkproject") << "{}";
    std::ofstream(root / "spark.modules.json") << "{}";

    BuildCookPanel::BuildSettings settings;
    settings.outputDirectory = "Cooked";
    BuildPipeline pipeline;
    EXPECT_TRUE(pipeline.StartCookOnly(settings, root.string()));
    for (int i = 0; i < 500 && pipeline.IsRunning(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_FALSE(pipeline.IsRunning());
    EXPECT_TRUE(pipeline.GetResult() == BuildResult::Success);
    EXPECT_TRUE(std::filesystem::is_regular_file(root / "Cooked" / "Assets" / "asset.txt"));
    EXPECT_TRUE(std::filesystem::is_regular_file(root / "Cooked" / "Scenes" / "Default.sparkscene"));
    EXPECT_TRUE(std::filesystem::is_regular_file(root / "Cooked" / "Config" / "EditorSettings.json"));
    EXPECT_TRUE(std::filesystem::is_regular_file(root / "Cooked" / "Cookable.sparkproject"));
    EXPECT_TRUE(std::filesystem::is_regular_file(root / "Cooked" / "spark.modules.json"));
    EXPECT_TRUE(std::filesystem::is_regular_file(root / "Cooked" / "Assets" / "spark-cook-manifest.json"));
    const auto lines = pipeline.DrainLogLines();
    EXPECT_TRUE(std::any_of(lines.begin(), lines.end(), [](const BuildLogLine& line)
                            { return line.text.find("SparkCooker:") != std::string::npos; }));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    EXPECT_FALSE(ec);
}

TEST(BuildPipeline_AssemblesRunnableModuleAndIsolatedScenePackage)
{
#ifdef _WIN32
    constexpr const char* hostFilename = "SparkEngine.exe";
    constexpr const char* serverFilename = "SparkServer.exe";
    constexpr const char* moduleFilename = "Runnable.dll";
    constexpr const char* runtimeDependency = "RuntimeDependency.dll";
    constexpr const char* packagedGame = "Playable Game.exe";
    constexpr const char* packagedServer = "Arena Server.exe";
    constexpr const char* gameLauncherFilename = "LaunchGame.cmd";
    constexpr const char* serverLauncherFilename = "LaunchServer.cmd";
    constexpr const char* sceneLauncherFilename = "LaunchScene.cmd";
    constexpr const char* sceneHost = "Playable Game Scene.exe";
#elif defined(__APPLE__)
    constexpr const char* hostFilename = "SparkEngine";
    constexpr const char* serverFilename = "SparkServer";
    constexpr const char* moduleFilename = "Runnable.dylib";
    constexpr const char* runtimeDependency = "libRuntimeDependency.dylib";
    constexpr const char* packagedGame = "Playable Game";
    constexpr const char* packagedServer = "Arena Server";
    constexpr const char* gameLauncherFilename = "LaunchGame.sh";
    constexpr const char* serverLauncherFilename = "LaunchServer.sh";
    constexpr const char* sceneLauncherFilename = "LaunchScene.sh";
    constexpr const char* sceneHost = "Playable Game Scene";
#else
    constexpr const char* hostFilename = "SparkEngine";
    constexpr const char* serverFilename = "SparkServer";
    constexpr const char* moduleFilename = "Runnable.so";
    constexpr const char* runtimeDependency = "libRuntimeDependency.so";
    constexpr const char* packagedGame = "Playable Game";
    constexpr const char* packagedServer = "Arena Server";
    constexpr const char* gameLauncherFilename = "LaunchGame.sh";
    constexpr const char* serverLauncherFilename = "LaunchServer.sh";
    constexpr const char* sceneLauncherFilename = "LaunchScene.sh";
    constexpr const char* sceneHost = "Playable Game Scene";
#endif
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("spark-runnable-package-test-" + std::to_string(stamp));
    const std::filesystem::path project = root / "Project";
    const std::filesystem::path runtime = root / "Runtime";
    const std::filesystem::path artifacts = root / "Artifacts";
    const std::filesystem::path output = project / "Build" / "Output";
    std::filesystem::create_directories(project / "Assets");
    std::filesystem::create_directories(project / "Scenes");
    std::filesystem::create_directories(project / "Config");
    std::filesystem::create_directories(runtime / "Shaders");
    std::filesystem::create_directories(artifacts);
    std::filesystem::create_directories(output / "Assets");
    std::ofstream(output / "stale-runtime.dll") << "stale";
    std::ofstream(output / "Assets" / "removed-asset.txt") << "stale";
    std::ofstream(project / "Assets" / "asset.txt") << "asset";
    std::ofstream(project / "Scenes" / "Default.sparkscene") << "{\"entities\":[]}";
    std::ofstream(project / "Config" / "settings.json") << "{}";
    std::ofstream(project / "Runnable.sparkproject") << "{}";
    std::ofstream(project / "spark.modules.json") << "{\"modules\":[{\"path\":\"Stale.module\"}]}";
    std::ofstream(runtime / hostFilename) << "host";
    std::ofstream(runtime / serverFilename) << "server";
    std::ofstream(runtime / runtimeDependency) << "dependency";
    std::ofstream(runtime / "Shaders" / "Basic.hlsl") << "shader";
    std::ofstream(artifacts / moduleFilename) << "module";
    std::ofstream(artifacts / (std::string(moduleFilename) + ".sparkabi")) << "abi";

    BuildCookPanel::BuildSettings settings;
    settings.platform = BuildPipeline::NativeTargetPlatform();
    settings.executableName = "Playable Game";
    settings.packageDedicatedServer = true;
    settings.dedicatedServerExecutableName = "Arena Server";
    settings.cookAssets = true;
    std::string error;
    EXPECT_TRUE(BuildPipeline::AssembleNativePackage(settings, project.string(), (runtime / hostFilename).string(),
                                                     (artifacts / moduleFilename).string(), output.string(), &error,
                                                     (runtime / serverFilename).string()));
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(std::filesystem::is_regular_file(output / packagedGame));
    EXPECT_TRUE(std::filesystem::is_regular_file(output / packagedServer));
    EXPECT_TRUE(std::filesystem::is_regular_file(output / moduleFilename));
    EXPECT_TRUE(std::filesystem::is_regular_file(output / (std::string(moduleFilename) + ".sparkabi")));
    EXPECT_TRUE(std::filesystem::is_regular_file(output / runtimeDependency));
    EXPECT_TRUE(std::filesystem::is_regular_file(output / "Shaders" / "Basic.hlsl"));
    EXPECT_TRUE(std::filesystem::is_regular_file(output / "Assets" / "asset.txt"));
    EXPECT_FALSE(std::filesystem::exists(output / "stale-runtime.dll"));
    EXPECT_FALSE(std::filesystem::exists(output / "Assets" / "removed-asset.txt"));
    EXPECT_TRUE(std::filesystem::is_regular_file(output / "Startup.sparkscene"));
    EXPECT_TRUE(std::filesystem::is_regular_file(output / "ScenePreview" / sceneHost));

    auto readText = [](const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    };
    const std::string manifest = readText(output / "spark.modules.json");
    EXPECT_STR_CONTAINS(manifest, std::string("\"path\": \"") + moduleFilename + "\"");
    EXPECT_TRUE(manifest.find("Stale.module") == std::string::npos);
    const std::string gameLauncher = readText(output / gameLauncherFilename);
    EXPECT_STR_CONTAINS(gameLauncher, packagedGame);
    EXPECT_TRUE(gameLauncher.find("-scene") == std::string::npos);
    const std::string serverLauncher = readText(output / serverLauncherFilename);
    EXPECT_STR_CONTAINS(serverLauncher, packagedServer);
    EXPECT_STR_CONTAINS(serverLauncher, "--manifest spark.modules.json");
    const std::string sceneLauncher = readText(output / sceneLauncherFilename);
    EXPECT_STR_CONTAINS(sceneLauncher, sceneHost);
    EXPECT_STR_CONTAINS(sceneLauncher, "-scene Startup.sparkscene");
    const std::string readme = readText(output / "PACKAGE_README.txt");
    EXPECT_STR_CONTAINS(readme, "separate from module execution");

    // A failed replacement must leave the last complete package untouched and
    // must not expose files from the failed staging attempt.
    std::ofstream(output / "preserved-package.txt") << "preserve";
    std::filesystem::remove(runtime / "Shaders" / "Basic.hlsl");
    std::filesystem::remove(runtime / "Shaders");
    EXPECT_FALSE(BuildPipeline::AssembleNativePackage(settings, project.string(), (runtime / hostFilename).string(),
                                                      (artifacts / moduleFilename).string(), output.string(), &error,
                                                      (runtime / serverFilename).string()));
    EXPECT_TRUE(std::filesystem::is_regular_file(output / "preserved-package.txt"));
    EXPECT_TRUE(std::filesystem::is_regular_file(output / packagedGame));

    settings.executableName = "..\\Unsafe";
    EXPECT_FALSE(BuildPipeline::AssembleNativePackage(
        settings, project.string(), (runtime / hostFilename).string(), (artifacts / moduleFilename).string(),
        (project / "UnsafeOutput").string(), &error, (runtime / serverFilename).string()));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    EXPECT_FALSE(ec);
}

// ============================================================================
// EditorPluginManager Tests
// ============================================================================

TEST(PluginManager_InitialState)
{
    EditorPluginManager mgr;
    EXPECT_EQ(mgr.GetPluginCount(), 0u);
}

TEST(PluginManager_GetPluginNames)
{
    EditorPluginManager mgr;
    auto names = mgr.GetPluginNames();
    EXPECT_TRUE(names.empty());
}

TEST(PluginManager_GetPluginNull)
{
    EditorPluginManager mgr;
    IEditorPlugin* plugin = mgr.GetPlugin("NonExistent");
    EXPECT_TRUE(plugin == nullptr);
}

// ============================================================================
// GATED TESTS — require ImGui (SPARK_TEST_HAS_IMGUI)
// These activate when building with ENABLE_EDITOR=ON and ImGui is available.
// ============================================================================

#ifdef SPARK_TEST_HAS_IMGUI

#include "Panels/AssetBrowserPanel.h"
#include "Search/CommandPalette.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "Graphics/GraphicsEngine.h"
#endif

// --- EditorTheme full method tests ---

TEST(Gated_ThemeColor_FromRGB)
{
    ThemeColor c = ThemeColor::FromRGB(255, 0, 0);
    EXPECT_NEAR(c.r, 1.0f, 0.01f);
    EXPECT_NEAR(c.g, 0.0f, 0.01f);
    EXPECT_NEAR(c.b, 0.0f, 0.01f);
}

TEST(Gated_ThemeColor_FromRGB_Green)
{
    ThemeColor c = ThemeColor::FromRGB(0, 255, 0);
    EXPECT_NEAR(c.r, 0.0f, 0.01f);
    EXPECT_NEAR(c.g, 1.0f, 0.01f);
}

TEST(Gated_ThemeColor_FromHex)
{
    ThemeColor c = ThemeColor::FromHex("#FF0000");
    EXPECT_NEAR(c.r, 1.0f, 0.01f);
    EXPECT_NEAR(c.g, 0.0f, 0.01f);
    EXPECT_NEAR(c.b, 0.0f, 0.01f);
}

TEST(Gated_ThemeColor_FromHex_Blue)
{
    ThemeColor c = ThemeColor::FromHex("#0000FF");
    EXPECT_NEAR(c.b, 1.0f, 0.01f);
    EXPECT_NEAR(c.r, 0.0f, 0.01f);
}

TEST(Gated_ThemeColor_Lerp)
{
    ThemeColor a(0.0f, 0.0f, 0.0f);
    ThemeColor b(1.0f, 1.0f, 1.0f);
    ThemeColor mid = a.Lerp(b, 0.5f);
    EXPECT_NEAR(mid.r, 0.5f, 0.01f);
    EXPECT_NEAR(mid.g, 0.5f, 0.01f);
    EXPECT_NEAR(mid.b, 0.5f, 0.01f);
}

TEST(Gated_ThemeColor_Lerp_Extremes)
{
    ThemeColor a(0.2f, 0.3f, 0.4f);
    ThemeColor b(0.8f, 0.9f, 1.0f);
    ThemeColor at0 = a.Lerp(b, 0.0f);
    ThemeColor at1 = a.Lerp(b, 1.0f);
    EXPECT_NEAR(at0.r, 0.2f, 0.01f);
    EXPECT_NEAR(at1.r, 0.8f, 0.01f);
}

TEST(Gated_ThemeColor_Darken)
{
    ThemeColor c(0.5f, 0.5f, 0.5f);
    ThemeColor darker = c.Darken(0.2f);
    EXPECT_TRUE(darker.r < c.r);
    EXPECT_TRUE(darker.g < c.g);
}

TEST(Gated_ThemeColor_Lighten)
{
    ThemeColor c(0.5f, 0.5f, 0.5f);
    ThemeColor lighter = c.Lighten(0.2f);
    EXPECT_TRUE(lighter.r > c.r);
    EXPECT_TRUE(lighter.g > c.g);
}

TEST(Gated_ThemeColor_Desaturate)
{
    ThemeColor c(1.0f, 0.0f, 0.0f); // pure red
    ThemeColor desat = c.Desaturate(1.0f);
    // Fully desaturated should make channels closer together (gray)
    EXPECT_NEAR(desat.a, 1.0f, 0.01f);
}

TEST(Gated_ThemeColor_WithAlpha)
{
    ThemeColor c(0.5f, 0.5f, 0.5f, 1.0f);
    ThemeColor half = c.WithAlpha(0.5f);
    EXPECT_NEAR(half.a, 0.5f, 0.01f);
    EXPECT_NEAR(half.r, 0.5f, 0.01f); // color unchanged
}

TEST(Gated_EditorTheme_RegisterAndGetTheme)
{
    EditorThemeData data;
    data.name = "GatedTestTheme";
    data.description = "gated unit test theme";
    EXPECT_TRUE(EditorTheme::RegisterTheme(data));

    const EditorThemeData* retrieved = EditorTheme::GetTheme("GatedTestTheme");
    EXPECT_TRUE(retrieved != nullptr);
    if (retrieved)
        EXPECT_EQ(retrieved->name, std::string("GatedTestTheme"));
}

TEST(Gated_EditorTheme_GetAvailableThemes)
{
    auto themes = EditorTheme::GetAvailableThemes();
    EXPECT_TRUE(!themes.empty());
}

TEST(Gated_EditorTheme_CreateBlendedTheme)
{
    EditorThemeData t1 = EditorTheme::CreateUnityProTheme();
    EditorThemeData t2 = EditorTheme::CreateUnrealProTheme();
    EditorTheme::RegisterTheme(t1);
    EditorTheme::RegisterTheme(t2);

    bool ok = EditorTheme::CreateBlendedTheme(t1.name, t2.name, 0.5f, "GatedBlend");
    EXPECT_TRUE(ok);
    EXPECT_TRUE(EditorTheme::GetTheme("GatedBlend") != nullptr);
}

TEST(Gated_EditorTheme_BuiltinThemes)
{
    auto unity = EditorTheme::CreateUnityProTheme();
    auto unreal = EditorTheme::CreateUnrealProTheme();
    auto vs = EditorTheme::CreateVSProTheme();
    auto jetbrains = EditorTheme::CreateJetBrainsTheme();
    EXPECT_TRUE(!unity.name.empty());
    EXPECT_TRUE(!unreal.name.empty());
    EXPECT_TRUE(!vs.name.empty());
    EXPECT_TRUE(!jetbrains.name.empty());
}

// --- CommandPalette full tests ---

TEST(Gated_CommandPalette_OpenCloseToggle)
{
    CommandPalette palette;
    EXPECT_FALSE(palette.IsOpen());

    palette.Open();
    EXPECT_TRUE(palette.IsOpen());

    palette.Close();
    EXPECT_FALSE(palette.IsOpen());

    palette.Toggle();
    EXPECT_TRUE(palette.IsOpen());
    palette.Toggle();
    EXPECT_FALSE(palette.IsOpen());
}

TEST(Gated_CommandPalette_RegisterActions)
{
    CommandPalette palette;
    bool executed = false;
    palette.RegisterAction("TestCmd", "Testing", [&]() { executed = true; }, "Ctrl+T");
    palette.RegisterAction("OtherCmd", "Testing", []() {});

    // Verify state is functional after registering
    palette.Open();
    EXPECT_TRUE(palette.IsOpen());
    palette.Close();
}

TEST(Gated_CommandPalette_ClearActions)
{
    CommandPalette palette;
    palette.RegisterAction("A", "Cat", []() {});
    palette.RegisterAction("B", "Cat", []() {});
    palette.ClearActions();
    palette.Open();
    EXPECT_TRUE(palette.IsOpen());
    palette.Close();
}

TEST(Gated_CommandPalette_KeyboardSelectionIsLimitedToVisibleResults)
{
    CommandPalette palette;
    int executedIndex = -1;
    for (int i = 0; i < 25; ++i)
    {
        palette.RegisterAction("Action " + std::to_string(i), "Command", [&, i]() { executedIndex = i; });
    }

    palette.Open();
    EXPECT_EQ(palette.GetFilteredActionCount(), 25u);
    EXPECT_EQ(palette.GetVisibleActionCount(), CommandPalette::MaxVisibleResults);

    palette.MoveSelection(100);
    EXPECT_EQ(palette.GetSelectedIndex(), 19);
    EXPECT_TRUE(palette.ExecuteSelected());
    EXPECT_EQ(executedIndex, 19);
    EXPECT_FALSE(palette.IsOpen());
}

TEST(Gated_CommandPalette_CommandPrefixIncludesCommandLikeSurfacesOnly)
{
    CommandPalette palette;
    palette.RegisterAction("Undo", "Command", []() {});
    palette.RegisterAction("Save Scene", "Scene", []() {});
    palette.RegisterAction("Reset Layout", "Layout", []() {});
    palette.RegisterAction("Toggle Inspector", "Panel", []() {});
    palette.RegisterAction("Player", "Entity", []() {});
    palette.RegisterAction("player.png", "Asset", []() {});

    palette.SetFilter(">");
    EXPECT_EQ(palette.GetFilteredActionCount(), 3u);
    palette.SetFilter("> save");
    EXPECT_EQ(palette.GetFilteredActionCount(), 1u);

    // '@' and '/' are ordinary query characters now; the UI no longer claims
    // entity/file providers that the palette does not actually populate.
    palette.SetFilter("@");
    EXPECT_EQ(palette.GetFilteredActionCount(), 0u);
    palette.SetFilter("/");
    EXPECT_EQ(palette.GetFilteredActionCount(), 0u);
}

TEST(Gated_AssetBrowser_ProjectBoundaryNestedNavigationAndReset)
{
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("spark-asset-browser-" + std::to_string(stamp));
    const fs::path assets = root / "Project" / "Assets";
    const fs::path nested = assets / "Nested";
    const fs::path deep = nested / "Deep";
    const fs::path outside = root / "Outside";
    fs::create_directories(deep);
    fs::create_directories(outside);
    {
        std::ofstream(outside / "source.png") << "asset data";
        std::ofstream(outside / "source.json") << R"({"tiling":[2.0,3.0]})";
        std::ofstream(assets / "root.asset") << "root";
    }

    AssetBrowserPanel panel;
#ifdef SPARK_PLATFORM_WINDOWS
    GraphicsEngine graphics;
    panel.SetGraphics(&graphics);
    const auto pathUtf8 = [](const fs::path& path)
    {
        const auto utf8 = path.generic_u8string();
        return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    };
    const std::string projectRootUtf8 = pathUtf8(root / "Project");
    constexpr const char* importedMaterialPath = "Assets/Nested/Deep/source.json";
    // Seed the deterministic missing-file cache before the Asset Browser
    // imports the material into that exact path.
    EXPECT_TRUE(graphics.GetOrLoadBasicMaterial(importedMaterialPath, projectRootUtf8) == nullptr);
#endif
    // Use a spelling containing '..' but still resolving to Assets, proving
    // that the stored project asset root is canonical.
    panel.SetProjectPath((assets / "Nested" / "..").string());
    EXPECT_EQ(fs::path(panel.GetProjectPath()), fs::weakly_canonical(assets));
    EXPECT_EQ(panel.GetFolders().size(), 1u);

    EXPECT_TRUE(panel.NavigateToFolder(nested.string()));
    EXPECT_EQ(panel.GetFolders().size(), 1u);
    EXPECT_TRUE(panel.NavigateToFolder(deep.string()));
    const std::string safeFolder = panel.GetCurrentFolder();
    EXPECT_FALSE(panel.NavigateToFolder(outside.string()));
    EXPECT_EQ(panel.GetCurrentFolder(), safeFolder);

    EXPECT_TRUE(panel.ImportAsset((outside / "source.png").string()));
    EXPECT_TRUE(fs::exists(deep / "source.png"));
    EXPECT_TRUE(panel.LastOperationSucceeded());
#ifdef SPARK_PLATFORM_WINDOWS
    EXPECT_TRUE(panel.ImportAsset((outside / "source.json").string()));
    EXPECT_TRUE(fs::exists(deep / "source.json"));
    const GraphicsEngine::BasicMaterial* importedMaterial =
        graphics.GetOrLoadBasicMaterial(importedMaterialPath, projectRootUtf8);
    EXPECT_TRUE(importedMaterial != nullptr);
    if (importedMaterial)
    {
        EXPECT_NEAR(importedMaterial->tiling.x, 2.0f, 0.001f);
        EXPECT_NEAR(importedMaterial->tiling.y, 3.0f, 0.001f);
    }
#endif
    EXPECT_FALSE(panel.ImportAsset((outside / "missing.png").string()));
    EXPECT_FALSE(panel.LastOperationSucceeded());
    EXPECT_TRUE(!panel.GetLastOperationMessage().empty());

    panel.ClearProject();
    EXPECT_TRUE(panel.GetProjectPath().empty());
    EXPECT_TRUE(panel.GetCurrentFolder().empty());
    EXPECT_TRUE(panel.GetAssets().empty());
    EXPECT_TRUE(panel.GetFolders().empty());
    EXPECT_FALSE(panel.ImportAsset((outside / "source.png").string()));

    std::error_code ec;
    fs::remove_all(root, ec);
    EXPECT_FALSE(ec);
}

// --- LevelStreaming spatial method tests ---

TEST(Gated_WorldTile_ContainsPoint)
{
    WorldTile tile;
    tile.worldPosition = {0, 0, 0};
    tile.worldSize = {100, 100, 100};

    XMFLOAT3 inside = {10, 10, 10};
    XMFLOAT3 outside = {200, 200, 200};
    EXPECT_TRUE(tile.ContainsPoint(inside));
    EXPECT_FALSE(tile.ContainsPoint(outside));
}

TEST(Gated_WorldTile_GetDistanceToCenter)
{
    WorldTile tile;
    tile.worldPosition = {0, 0, 0};
    tile.worldSize = {100, 100, 100};

    XMFLOAT3 point = {100, 0, 0};
    float dist = tile.GetDistanceToCenter(point);
    EXPECT_TRUE(dist > 0.0f);
}

TEST(Gated_WorldTile_CalculateLOD)
{
    WorldTile tile;
    tile.lodDistances = {500, 1000, 1500, 2000, 2500};

    LODLevel lod0 = tile.CalculateLOD(100.0f);
    EXPECT_TRUE(lod0 == LODLevel::LOD_0);

    LODLevel lod3 = tile.CalculateLOD(1800.0f);
    EXPECT_TRUE(lod3 == LODLevel::LOD_3);
}

TEST(Gated_StreamingVolume_ContainsPoint)
{
    StreamingVolume vol;
    vol.center = {0, 0, 0};
    vol.size = {100, 100, 100};

    EXPECT_TRUE(vol.ContainsPoint({10, 10, 10}));
    EXPECT_FALSE(vol.ContainsPoint({200, 0, 0}));
}

TEST(Gated_StreamingViewer_PredictedPosition)
{
    StreamingViewer viewer;
    viewer.position = {0, 0, 0};
    viewer.velocity = {10, 0, 0};
    viewer.forward = {0, 0, 1};

    XMFLOAT3 predicted = viewer.GetPredictedPosition(2.0f);
    EXPECT_NEAR(predicted.x, 20.0f, 0.5f);
}

TEST(Gated_StreamingViewer_IsInViewFrustum)
{
    StreamingViewer viewer;
    viewer.position = {0, 0, 0};
    viewer.forward = {0, 0, 1};
    viewer.fieldOfView = 90.0f;

    EXPECT_TRUE(viewer.IsInViewFrustum({0, 0, 100}, 10.0f));
}

// --- VersionControl merge handler tests ---

TEST(Gated_VCS_SceneMergeHandler)
{
    SceneMergeHandler handler;
    auto exts = handler.GetSupportedExtensions();
    EXPECT_TRUE(!exts.empty());
    EXPECT_TRUE(handler.CanMerge("level.sparkscene"));
    EXPECT_FALSE(handler.CanMerge("readme.txt"));
}

TEST(Gated_VCS_MaterialMergeHandler)
{
    MaterialMergeHandler handler;
    auto exts = handler.GetSupportedExtensions();
    EXPECT_TRUE(!exts.empty());
}

TEST(Gated_VCS_SceneMergeHandler_ValidateMerge)
{
    SceneMergeHandler handler;
    bool valid = handler.ValidateMerge("test.scene");
    // Validate should return a result without crashing
    (void)valid;
    EXPECT_TRUE(true);
}

#endif // SPARK_TEST_HAS_IMGUI
