/**
 * @file SparkGameMMOFPS.h
 * @brief TERRAFRONT — massively-multiplayer combined-arms FPS module.
 *
 * Three factions (Meridian Accord, Aurum Combine, Helix Covenant) fight a
 * persistent territory war over the Cindral Wastes. See DESIGN.md for the
 * full frozen spec.
 *
 * Implements Spark::IModule. Owns every TF* system and the shared
 * TFGameContext through which the systems cooperate.
 */

#pragma once

#include "Spark/SparkSDK.h"
#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include <memory>

namespace Terrafront { class TFBotSystem; }

class TerrafrontModule : public Spark::IModule
{
  public:
    TerrafrontModule();
    ~TerrafrontModule() override;

    // --- Spark::IModule interface ---
    Spark::ModuleInfo GetModuleInfo() const override;
    bool OnLoad(Spark::IEngineContext* context) override;
    void OnUnload() override;
    void OnUpdate(float deltaTime) override;
    void OnFixedUpdate(float fixedDeltaTime) override;
    void OnRender() override;
    void OnResize(int width, int height) override;
    void OnPause() override;
    void OnResume() override;
    void OnImGui() override;

    Terrafront::TFGameContext& Ctx()      { return m_ctx; }
    Terrafront::TFEventBus&    Events()   { return m_events; }

  private:
    void RegisterConsoleCommands();   // Console/TFCommands.cpp

    Spark::IEngineContext* m_context{nullptr};
    bool m_initialized{false};
    bool m_paused{false};

    Terrafront::TFGameContext m_ctx;
    Terrafront::TFEventBus    m_events;

    // Boot order == declaration order (see Main.cpp); reverse on shutdown.
    std::unique_ptr<Terrafront::TFDataTables>        m_data;
    std::unique_ptr<Terrafront::TFWorldSetup>        m_world;
    std::unique_ptr<Terrafront::TFReplication>       m_replication;
    std::unique_ptr<Terrafront::TFServerSim>         m_serverSim;
    std::unique_ptr<Terrafront::TFClientNet>         m_clientNet;
    std::unique_ptr<Terrafront::TFRegionSystem>      m_regions;
    std::unique_ptr<Terrafront::TFPlayerSystem>      m_players;
    std::unique_ptr<Terrafront::TFWeaponSystem>      m_weapons;
    std::unique_ptr<Terrafront::TFDamageSystem>      m_damage;
    std::unique_ptr<Terrafront::TFVehicleSystem>     m_vehicles;
    std::unique_ptr<Terrafront::TFColossusSystem>    m_colossus;
    std::unique_ptr<Terrafront::TFDeployableSystem>  m_deployables;
    std::unique_ptr<Terrafront::TFProgressionSystem> m_progression;
    std::unique_ptr<Terrafront::TFSquadSystem>       m_squads;
    std::unique_ptr<Terrafront::TFBotSystem>         m_bots;
    std::unique_ptr<Terrafront::TFHUD>               m_hud;
    std::unique_ptr<Terrafront::TFMapScreen>         m_map;
    std::unique_ptr<Terrafront::TFSpawnScreen>       m_spawnUI;
    std::unique_ptr<Terrafront::TFScoreboard>        m_scoreboard;
};
