/**
 * @file TestEditorPanelsRealBackends.cpp
 * @brief Real-class tests for the editor panels that used to fabricate data.
 *
 * Each case drives the production panel class and asserts that the panel reads or
 * writes the real engine backend instead of a private simulation:
 *  - TimeOfDayPanel writes through to Spark::TimeOfDaySystem
 *  - ReplayPanel toggles recording on Spark::ReplaySystem
 *  - ModdingPanel discovers mods on disk through Spark::ModSystem
 *  - SaveSystemPanel enumerates and deletes real .spark_save files
 *  - WeatherFogPanel shows the engine weather preset and reports "not connected"
 *  - SceneStatisticsPanel counts entities in a real World (no sample constants)
 *  - SearchPanel finds live World entities (no sample entity table)
 *  - WeaponEditorPanel Reset restores the starting values
 *  - ObjectPlacementPanel placement routes to the document entity creator
 *
 * These panels are EditorPanel subclasses whose .cpp files require ImGui to link,
 * so this file belongs in the ImGui-gated test block.
 */

#include "TestFramework.h"

#include "Core/EngineContext.h"
#include "Engine/ECS/Components.h"
#include "Engine/Modding/ModSystem.h"
#include "Engine/Replay/ReplaySystem.h"
#include "Engine/SaveSystem/SaveSystem.h"
#include "Engine/World/TimeOfDaySystem.h"
#include "Graphics/WeatherSystem.h"

#include "Panels/ModdingPanel.h"
#include "Panels/ObjectPlacementPanel.h"
#include "Panels/ReplayPanel.h"
#include "Panels/SaveSystemPanel.h"
#include "Panels/SceneStatisticsPanel.h"
#include "Panels/SearchPanel.h"
#include "Panels/TimeOfDayPanel.h"
#include "Panels/WeaponEditorPanel.h"
#include "Panels/WeatherFogPanel.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    /// Unique scratch directory for one test case; removed on scope exit.
    class ScopedTempDir
    {
      public:
        explicit ScopedTempDir(const std::string& tag)
        {
            std::error_code ec;
            m_path = std::filesystem::temp_directory_path(ec) / ("spark_panels_" + tag);
            std::filesystem::remove_all(m_path, ec);
            std::filesystem::create_directories(m_path, ec);
        }

        ~ScopedTempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(m_path, ec);
        }

        ScopedTempDir(const ScopedTempDir&) = delete;
        ScopedTempDir& operator=(const ScopedTempDir&) = delete;

        const std::filesystem::path& Path() const { return m_path; }
        std::string String() const { return m_path.string(); }

      private:
        std::filesystem::path m_path;
    };

    /// @brief Installs an injected EngineContext for one scope and clears it on exit.
    ///
    /// A bare SetInjected(&stackContext) / SetInjected(nullptr) pair leaks a dangling process-wide
    /// pointer whenever a fatal assertion unwinds between them: the stack context is destroyed while
    /// the global still references it, and every later test in the binary reads freed memory.
    class ScopedInjectedContext
    {
      public:
        explicit ScopedInjectedContext(EngineContext* context) { EngineContext::SetInjected(context); }
        ~ScopedInjectedContext() { EngineContext::SetInjected(nullptr); }

        ScopedInjectedContext(const ScopedInjectedContext&) = delete;
        ScopedInjectedContext& operator=(const ScopedInjectedContext&) = delete;
    };

    /// @brief Restores the process-wide time-of-day clock however the test exits.
    class ScopedTimeOfDayState
    {
      public:
        ScopedTimeOfDayState()
            : m_hour(Spark::TimeOfDaySystem::GetInstance().GetTimeOfDay()),
              m_scale(Spark::TimeOfDaySystem::GetInstance().GetTimeScale()),
              m_paused(Spark::TimeOfDaySystem::GetInstance().IsPaused())
        {
        }

        ~ScopedTimeOfDayState()
        {
            Spark::TimeOfDaySystem& timeOfDay = Spark::TimeOfDaySystem::GetInstance();
            timeOfDay.SetTimeOfDay(m_hour);
            timeOfDay.SetTimeScale(m_scale);
            timeOfDay.SetPaused(m_paused);
        }

        ScopedTimeOfDayState(const ScopedTimeOfDayState&) = delete;
        ScopedTimeOfDayState& operator=(const ScopedTimeOfDayState&) = delete;

      private:
        float m_hour;
        float m_scale;
        bool m_paused;
    };
} // namespace

TEST(EditorPanels_TimeOfDayPanelWritesThroughToEngineSystem)
{
    Spark::TimeOfDaySystem& timeOfDay = Spark::TimeOfDaySystem::GetInstance();
    const ScopedTimeOfDayState restoreClock;

    // IsDrivingClock() is a function of process-global EngineContext state that other tests in this
    // binary install, so force the precondition instead of testing for it. An OWNED context cannot be
    // dropped without side effects, so that case is an explicit skip rather than a silent pass.
    const ScopedInjectedContext noInjectedContext(nullptr);
    if (::EngineContext::Get() != nullptr)
    {
        SKIP_TEST("an owned EngineContext is installed process-wide; the panel-drives-clock path cannot be forced");
    }

    SparkEditor::TimeOfDayPanel panel;
    ASSERT_TRUE(panel.Initialize());

    panel.SetHour(6.5f);
    EXPECT_NEAR(timeOfDay.GetTimeOfDay(), 6.5f, 0.001f);
    EXPECT_NEAR(panel.GetHour(), 6.5f, 0.001f);

    panel.SetTimeScale(120.0f);
    EXPECT_NEAR(timeOfDay.GetTimeScale(), 120.0f, 0.001f);
    EXPECT_NEAR(panel.GetTimeScale(), 120.0f, 0.001f);

    // Paused: neither the panel nor the engine clock may advance.
    panel.SetPaused(true);
    EXPECT_TRUE(timeOfDay.IsPaused());
    EXPECT_TRUE(panel.IsPaused());
    panel.Update(1.0f);
    EXPECT_NEAR(timeOfDay.GetTimeOfDay(), 6.5f, 0.001f);

    // Resumed: the panel drives the engine system itself when no engine
    // lifecycle exists in the process, and reads the hour back from it.
    ASSERT_TRUE(panel.IsDrivingClock());
    panel.SetPaused(false);
    panel.Update(1.0f);
    EXPECT_GT(timeOfDay.GetTimeOfDay(), 6.5f);
    EXPECT_NEAR(panel.GetHour(), timeOfDay.GetTimeOfDay(), 0.0001f);
}

TEST(EditorPanels_ReplayPanelDrivesEngineRecording)
{
    Spark::ReplaySystem& replay = Spark::ReplaySystem::GetInstance();
    replay.StopRecording();

    SparkEditor::ReplayPanel panel;
    ASSERT_TRUE(panel.Initialize());
    EXPECT_FALSE(panel.IsRecording());

    panel.StartRecording();
    EXPECT_TRUE(replay.IsRecording());
    EXPECT_TRUE(panel.IsRecording());

    panel.StopRecording();
    EXPECT_FALSE(replay.IsRecording());
    EXPECT_FALSE(panel.IsRecording());

    // The panel reports what the engine actually captured, so an armed-but-unfed
    // recording is visible as zero frames rather than as a successful capture.
    // Comparing the panel getter to ReplaySystem::GetFrameCount() would compare the value to
    // itself (the panel simply forwards it) and could never fail, so both sides are literals here.
    EXPECT_EQ(panel.GetCapturedFrameCount(), static_cast<size_t>(0));

    // Feed a known number of frames through the engine system and assert the panel reports THAT
    // number. StartRecording() clears the buffer, and the timestamps are spaced well beyond the
    // record interval so none of the three is throttled away.
    replay.StartRecording();
    const std::vector<Spark::ReplayEntityState> entities(1);
    replay.RecordFrame(entities, 0.0f);
    replay.RecordFrame(entities, 100.0f);
    replay.RecordFrame(entities, 200.0f);
    replay.StopRecording();
    EXPECT_EQ(panel.GetCapturedFrameCount(), static_cast<size_t>(3));
}

TEST(EditorPanels_ReplayPanelListsOnlyRealReplayFiles)
{
    ScopedTempDir temp("replays");

    SparkEditor::ReplayPanel panel;
    ASSERT_TRUE(panel.Initialize());
    EXPECT_EQ(panel.RefreshReplayFiles(temp.String()), static_cast<size_t>(0));

    {
        std::ofstream out(temp.Path() / "match01.replay", std::ios::binary);
        out << "not-a-valid-replay";
    }
    {
        std::ofstream out(temp.Path() / "notes.txt");
        out << "ignored";
    }

    EXPECT_EQ(panel.RefreshReplayFiles(temp.String()), static_cast<size_t>(1));

    // A corrupt file must fail to load rather than being reported as loaded.
    EXPECT_FALSE(panel.LoadReplayFile((temp.Path() / "match01.replay").string()));
}

TEST(EditorPanels_ModdingPanelDiscoversModsOnDisk)
{
    ScopedTempDir temp("mods");

    SparkEditor::ModdingPanel panel;
    ASSERT_TRUE(panel.Initialize());
    EXPECT_EQ(panel.ScanForMods(temp.String()), static_cast<size_t>(0));
    EXPECT_TRUE(panel.GetMods().empty());

    std::error_code ec;
    std::filesystem::create_directories(temp.Path() / "AlphaMod", ec);
    {
        std::ofstream manifest(temp.Path() / "AlphaMod" / "mod.json");
        manifest << R"({"id":"alpha","name":"Alpha Mod","author":"QA","version":"1.0","loadOrder":0})";
    }

    EXPECT_EQ(panel.ScanForMods(temp.String()), static_cast<size_t>(1));
    ASSERT_EQ(panel.GetMods().size(), static_cast<size_t>(1));
    EXPECT_STR_CONTAINS(panel.GetMods()[0].name, "Alpha Mod");
    EXPECT_FALSE(panel.GetMods()[0].enabled);

    EXPECT_TRUE(panel.SetModEnabled("alpha", true));
    ASSERT_EQ(panel.GetMods().size(), static_cast<size_t>(1));
    EXPECT_TRUE(panel.GetMods()[0].enabled);

    EXPECT_FALSE(panel.SetModEnabled("does-not-exist", true));
}

TEST(EditorPanels_SaveSystemPanelEnumeratesAndDeletesRealSaves)
{
    ScopedTempDir temp("saves");

    Spark::SaveSystem& saveSystem = Spark::SaveSystem::GetInstance();
    ASSERT_TRUE(saveSystem.Initialize(temp.String()));

    World world;
    world.CreateEntity("Saved Entity");
    Spark::SaveMetadata metadata;
    metadata.saveName = "Panel Round Trip";
    metadata.sceneName = "TestScene";
    metadata.playTime = 65.0f;
    ASSERT_TRUE(saveSystem.Save("slot_panel", world, metadata));

    SparkEditor::SaveSystemPanel panel;
    ASSERT_TRUE(panel.Initialize());
    panel.SetSaveDirectory(temp.String());

    ASSERT_EQ(panel.GetSlots().size(), static_cast<size_t>(1));
    EXPECT_EQ(panel.GetSlots()[0].slotName, std::string("slot_panel"));
    EXPECT_EQ(panel.GetSlots()[0].metadata.saveName, std::string("Panel Round Trip"));
    EXPECT_EQ(panel.GetSlots()[0].metadata.sceneName, std::string("TestScene"));
    EXPECT_GT(panel.GetSlots()[0].fileSizeBytes, static_cast<uintmax_t>(0));

    // Rename moves the real file.
    EXPECT_TRUE(panel.RenameSlot("slot_panel", "renamed_slot"));
    EXPECT_FALSE(std::filesystem::exists(temp.Path() / "slot_panel.spark_save"));
    EXPECT_TRUE(std::filesystem::exists(temp.Path() / "renamed_slot.spark_save"));
    ASSERT_EQ(panel.GetSlots().size(), static_cast<size_t>(1));

    // Delete removes the real file, not just the UI row.
    EXPECT_TRUE(panel.DeleteSlot("renamed_slot"));
    EXPECT_FALSE(std::filesystem::exists(temp.Path() / "renamed_slot.spark_save"));
    EXPECT_TRUE(panel.GetSlots().empty());

    // Loading needs a live game World; with none, the panel refuses instead of
    // reporting a load that never happened.
    EXPECT_FALSE(panel.CanLoad());
    EXPECT_FALSE(panel.LoadSlot("renamed_slot"));

    saveSystem.SetSaveDirectory("Saves");
}

TEST(EditorPanels_WeatherFogPanelShowsEnginePresetAndReportsDisconnected)
{
    // Drop whatever a previous test injected so the disconnected path is a forced precondition,
    // not an accident of test order. Wrapping the assertions in `if (Get() == nullptr)` let the
    // whole disconnected case vanish — passing while asserting nothing.
    const ScopedInjectedContext noInjectedContext(nullptr);
    if (::EngineContext::Get() != nullptr)
    {
        SKIP_TEST("an owned EngineContext is installed process-wide; the disconnected path cannot be forced");
    }

    SparkEditor::WeatherFogPanel panel;
    ASSERT_TRUE(panel.Initialize());

    panel.SetSelectedType(Spark::WeatherType::Storm);
    const Spark::WeatherState shown = panel.GetSelectedPreset();
    const Spark::WeatherState engine = Spark::GetWeatherPreset(Spark::WeatherType::Storm);
    EXPECT_NEAR(shown.precipitationRate, engine.precipitationRate, 0.0001f);
    EXPECT_NEAR(shown.windSpeed, engine.windSpeed, 0.0001f);
    EXPECT_NEAR(shown.lightningFrequency, engine.lightningFrequency, 0.0001f);

    EXPECT_FALSE(panel.IsWeatherSystemConnected());
    EXPECT_FALSE(panel.ApplySelected());
}

TEST(EditorPanels_WeatherFogPanelAppliesToRegisteredWeatherSystem)
{
    EngineContext context;
    Spark::WeatherSystem weather;
    context.SetWeather(&weather);
    const ScopedInjectedContext injected(&context);

    SparkEditor::WeatherFogPanel panel;
    ASSERT_TRUE(panel.Initialize());
    panel.SetSelectedType(Spark::WeatherType::Rain);

    EXPECT_TRUE(panel.IsWeatherSystemConnected());
    EXPECT_TRUE(panel.ApplySelected());
    // SetWeather starts a transition toward the requested type.
    EXPECT_TRUE(weather.IsTransitioning());
    EXPECT_TRUE(weather.GetTargetState().type == Spark::WeatherType::Rain);
}

TEST(EditorPanels_SceneStatisticsPanelCountsRealEntities)
{
    SparkEditor::SceneStatisticsPanel panel;
    ASSERT_TRUE(panel.Initialize());

    // No World wired in: the panel reports nothing rather than sample constants.
    EXPECT_FALSE(panel.IsWorldConnected());
    panel.CollectStats();
    EXPECT_EQ(panel.GetTotalEntities(), 0);
    EXPECT_TRUE(panel.GetComponentCounts().empty());
    EXPECT_FALSE(panel.HasRenderStats());

    World world;
    const EntityID first = world.CreateEntity("Alpha");
    const EntityID second = world.CreateEntity("Beta");
    world.AddComponent<::Transform>(first);
    world.AddComponent<::Transform>(second);
    world.AddComponent<::MeshRenderer>(second);

    panel.SetWorld(&world);
    panel.CollectStats();
    EXPECT_TRUE(panel.IsWorldConnected());
    EXPECT_EQ(panel.GetTotalEntities(), 2);

    // FPS comes from the measured delta, not a synthetic curve.
    for (int i = 0; i < 20; ++i)
    {
        panel.Update(0.02f);
    }
    EXPECT_NEAR(panel.GetCurrentFps(), 50.0f, 0.5f);

    Spark::WorldBasicRenderStats stats;
    stats.candidates = 7;
    stats.drawn = 3;
    panel.SetRenderStats(stats);
    EXPECT_TRUE(panel.HasRenderStats());
}

TEST(EditorPanels_SearchPanelSearchesTheLiveWorld)
{
    SparkEditor::SearchPanel panel;
    ASSERT_TRUE(panel.Initialize());

    // No World: no fabricated sample entities may answer the query.
    EXPECT_FALSE(panel.IsWorldConnected());
    panel.Search("Player");
    EXPECT_TRUE(panel.GetResults().empty());

    World world;
    const EntityID player = world.CreateEntity("PlayerStart");
    world.AddComponent<::Transform>(player);
    world.CreateEntity("Crate");

    panel.SetWorld(&world);
    panel.Search("PlayerStart");
    ASSERT_TRUE(!panel.GetResults().empty());
    EXPECT_STR_CONTAINS(panel.GetResults()[0].name, "PlayerStart");

    uint32_t selected = 0;
    bool selectionCalled = false;
    panel.SetSelectionHandler(
        [&](uint32_t entityId)
        {
            selected = entityId;
            selectionCalled = true;
        });
    panel.Search("Crate");
    ASSERT_TRUE(!panel.GetResults().empty());
    panel.NavigateToResult(panel.GetResults()[0]);
    EXPECT_TRUE(selectionCalled);
    EXPECT_EQ(selected, static_cast<uint32_t>(panel.GetResults()[0].entityId));

    // A name that exists in neither the World nor any demo table finds nothing.
    panel.Search("ZzQqNoSuchEntity");
    EXPECT_TRUE(panel.GetResults().empty());
}

TEST(EditorPanels_WeaponEditorResetRestoresStartingValues)
{
    SparkEditor::WeaponEditorPanel panel;
    ASSERT_TRUE(panel.Initialize());
    ASSERT_TRUE(!panel.GetWeapons().empty());

    const float originalDamage = panel.GetWeapons()[0].damage;
    panel.GetWeapons()[0].damage = originalDamage + 100.0f;
    panel.GetWeapons()[0].isModified = true;
    EXPECT_NEAR(panel.GetWeapons()[0].damage, originalDamage + 100.0f, 0.0001f);

    EXPECT_TRUE(panel.ResetSelectedWeapon());
    EXPECT_NEAR(panel.GetWeapons()[0].damage, originalDamage, 0.0001f);
    EXPECT_FALSE(panel.GetWeapons()[0].isModified);
}

TEST(EditorPanels_ObjectPlacementRoutesToDocumentCreator)
{
    SparkEditor::ObjectPlacementPanel panel;
    ASSERT_TRUE(panel.Initialize());

    // Unconnected: placement reports failure instead of logging a success.
    EXPECT_FALSE(panel.IsPlacementConnected());
    EXPECT_FALSE(panel.PlaceTemplate("Cube"));

    std::string requested;
    panel.SetEntityCreator(
        [&](const std::string& templateName)
        {
            requested = templateName;
            return templateName != "Unsupported";
        });

    EXPECT_TRUE(panel.IsPlacementConnected());
    EXPECT_TRUE(panel.PlaceTemplate("Cube"));
    EXPECT_EQ(requested, std::string("Cube"));

    // A template the document rejects must be reported as a failure.
    EXPECT_FALSE(panel.PlaceTemplate("Unsupported"));
}
