/**
 * @file HeadlessArena.cpp
 * @brief SparkGameFPS no-render arena simulation for the headless (NullRHI) lifecycle.
 *
 * The headless host exposes no GraphicsEngine or InputManager, so the module
 * cannot build its renderable Game. It still loads the authored arena through
 * the data-only SceneManager path, binds the RespawnSystem and GameMode to the
 * authored default spawns, and ticks both every OnUpdate. At unload it prints
 * one SPARK_FPS_HEADLESS_ARENA record that cmake/RunSparkFPSHeadlessArena.cmake
 * checks against an independent parse of the same scene file.
 */

#include "SparkGameFPS.h"
#include "Game/FPSAssetPaths.h"
#include "Game/GameMechanics.h"
#include "Game/GameMode.h"
#include "SceneManager/SceneManager.h"
#include "Utils/LogMacros.h"

#include <cstdio>

namespace
{
    constexpr const wchar_t* kHeadlessArenaScene = L"Scenes/level1.scene";
} // namespace

bool SparkGameModule::LoadHeadlessArena()
{
    if (!Spark::FPSAssets::RootExists())
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Game, "SparkGameFPS headless arena: asset root '%s' does not exist",
                        Spark::FPSAssets::Root().string().c_str());
        return false;
    }

    // Null graphics and input select the data-only load: nodes are parsed and
    // validated, and every object slot stays null because nothing is rendered.
    SceneManager scene(nullptr, nullptr);
    const std::wstring scenePath = Spark::FPSAssets::Resolve(kHeadlessArenaScene);
    if (!scene.LoadScene(scenePath))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Game, "SparkGameFPS headless arena: failed to load '%s'",
                        Spark::FPSAssets::ResolveUtf8("Scenes/level1.scene").c_str());
        return false;
    }

    const std::vector<Spark::RespawnPoint> authoredSpawns = Spark::RespawnSystem::CollectAuthoredSpawnPoints(scene);
    if (authoredSpawns.empty())
    {
        // The respawn fallback would keep the match playable, but it is not the
        // authored arena, so a headless run must not report it as one.
        SPARK_LOG_ERROR(Spark::LogCategory::Game, "SparkGameFPS headless arena: scene has no default spawn points");
        return false;
    }

    auto respawn = std::make_unique<Spark::RespawnSystem>();
    respawn->Initialize();
    respawn->SetEventBus(m_context->GetEventBus());
    const int boundSpawns = respawn->BindSpawnPoints(authoredSpawns);

    auto mode = std::make_unique<Spark::GameMode>();
    if (!mode->Initialize(Spark::GameMode::GetPreset(Spark::GameModeType::Deathmatch)))
    {
        SPARK_LOG_ERROR(Spark::LogCategory::Game, "SparkGameFPS headless arena: Deathmatch rules were rejected");
        return false;
    }
    for (const Spark::RespawnPoint& authored : authoredSpawns)
    {
        Spark::SpawnPoint spawn(authored.position.x, authored.position.y, authored.position.z);
        spawn.rotation = authored.rotation;
        spawn.name = authored.name;
        mode->AddSpawnPoint(spawn);
    }
    mode->AddPlayer("Player1");
    mode->StartMatch();

    m_headlessArenaObjects = scene.GetNodeCount();
    m_headlessArenaSpawns = static_cast<int>(authoredSpawns.size());
    m_headlessArenaBoundSpawns = boundSpawns;
    m_headlessArenaTicks = 0;
    m_headlessRespawn = std::move(respawn);
    m_headlessMode = std::move(mode);

    SPARK_LOG_INFO(Spark::LogCategory::Game, "SparkGameFPS headless arena loaded: %d scene nodes, %d authored spawns",
                   m_headlessArenaObjects, m_headlessArenaSpawns);
    return true;
}

void SparkGameModule::ShutdownHeadlessArena()
{
    if (!m_headlessMode || !m_headlessRespawn)
        return;

    std::fprintf(stdout, "SPARK_FPS_HEADLESS_ARENA objects=%d spawns=%d bound=%d mode_spawns=%zu ticks=%llu match=%d\n",
                 m_headlessArenaObjects, m_headlessArenaSpawns, m_headlessArenaBoundSpawns,
                 m_headlessMode->GetSpawnPoints().size(), static_cast<unsigned long long>(m_headlessArenaTicks),
                 m_headlessMode->IsMatchActive() ? 1 : 0);
    std::fflush(stdout);

    m_headlessMode.reset();
    m_headlessRespawn.reset();
    m_headlessArenaObjects = 0;
    m_headlessArenaSpawns = 0;
    m_headlessArenaBoundSpawns = 0;
    m_headlessArenaTicks = 0;
}
