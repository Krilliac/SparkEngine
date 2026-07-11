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

namespace Terrafront
{
    class TFBotSystem;
    class TFOutfitPanel;     // outfits lane (TFOutfitSystem fwd-decl comes from TFTypes.h)
    class TFTravelSystem;    // continents lane
    class TFDirectivePanel;  // W8 ui-polish lane (UI/TFDirectivePanel.h)
    class TFVehicleTerminal; // W8 ui-polish lane (Game/TFVehicleTerminal.h)
    class TFAudioAmbience;   // audio-polish lane (W8): zone beds + distant combat layer
} // namespace Terrafront

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

    Terrafront::TFGameContext& Ctx() { return m_ctx; }
    Terrafront::TFEventBus& Events() { return m_events; }

  private:
    void RegisterConsoleCommands(); // Console/TFCommands.cpp

    Spark::IEngineContext* m_context{nullptr};
    bool m_initialized{false};
    bool m_paused{false};

  public:
    /// Master toggle for the aggregated "TERRAFRONT Debug" ImGui window
    /// (tf_debug panels). Off by default so the HUD is unobstructed in-game;
    /// the bare-CollapsingHeader system panels only render when this is on.
    bool m_debugPanels{false};

  private:
    Terrafront::TFGameContext m_ctx;
    Terrafront::TFEventBus m_events;

    // Boot order == declaration order (see Main.cpp); reverse on shutdown.
    std::unique_ptr<Terrafront::TFDataTables> m_data;
    std::unique_ptr<Terrafront::TFWorldSetup> m_world;
    std::unique_ptr<Terrafront::TFReplication> m_replication;
    std::unique_ptr<Terrafront::TFServerSim> m_serverSim;
    std::unique_ptr<Terrafront::TFClientNet> m_clientNet;
    std::unique_ptr<Terrafront::TFRegionSystem> m_regions;
    std::unique_ptr<Terrafront::TFPlayerSystem> m_players;
    std::unique_ptr<Terrafront::TFWeaponSystem> m_weapons;
    std::unique_ptr<Terrafront::TFDamageSystem> m_damage;
    std::unique_ptr<Terrafront::TFVehicleSystem> m_vehicles;
    std::unique_ptr<Terrafront::TFColossusSystem> m_colossus;
    std::unique_ptr<Terrafront::TFDeployableSystem> m_deployables;
    std::unique_ptr<Terrafront::TFProgressionSystem> m_progression;
    std::unique_ptr<Terrafront::TFDirectiveSystem> m_directives;
    std::unique_ptr<Terrafront::TFSquadSystem> m_squads;
    std::unique_ptr<Terrafront::TFBotSystem> m_bots;
    // audio-polish lane (W8): booted after bots (needs weapons + data live);
    // Main.cpp wiring applied by the integrator (see W8 wiring notes).
    std::unique_ptr<Terrafront::TFAudioAmbience> m_ambience;
    std::unique_ptr<Terrafront::TFHUD> m_hud;
    std::unique_ptr<Terrafront::TFMapScreen> m_map;
    std::unique_ptr<Terrafront::TFSpawnScreen> m_spawnUI;
    std::unique_ptr<Terrafront::TFScoreboard> m_scoreboard;
    // continents lane: sanctuary/warpgate travel (booted after scoreboard).
    std::unique_ptr<Terrafront::TFTravelSystem> m_travel;
    // Outfits lane: system after squads (boot order, see Main.cpp), panel
    // after the other UI screens.
    std::unique_ptr<Terrafront::TFOutfitSystem> m_outfits;
    std::unique_ptr<Terrafront::TFOutfitPanel> m_outfitPanel;

    // class-abilities lane (W9): booted after outfits, before bots (bot AI
    // calls CanUseAbility/UseAbility). Fwd-decl comes from TFTypes.h.
    std::unique_ptr<Terrafront::TFAbilitySystem> m_abilities;

    // W5 onboarding (Task 6, additive): booted after every W1-W4 system above
    // (db -> account -> characters -> loginFlow), per DESIGN.md "W5 —
    // Onboarding". TFDatabase/TFAccountSystem/TFCharacterSystem are plain
    // core-logic classes (no uniform Initialize(ctx,events) lifecycle — see
    // Main.cpp); only TFLoginFlow follows the usual system lifecycle.
    std::unique_ptr<Terrafront::TFDatabase> m_db;
    std::unique_ptr<Terrafront::TFAccountSystem> m_account;
    std::unique_ptr<Terrafront::TFCharacterSystem> m_characters;
    std::unique_ptr<Terrafront::TFLoginFlow> m_loginFlow;

    // chat-social lane (additive): booted after loginFlow (see Main.cpp).
    std::unique_ptr<Terrafront::TFSocialSystem> m_social;
    std::unique_ptr<Terrafront::TFChatWindow> m_chatWindow;
    std::unique_ptr<Terrafront::TFSocialPanel> m_socialPanel;

    // W8 ui-polish lane (additive): booted after the chat-social systems.
    std::unique_ptr<Terrafront::TFDirectivePanel> m_directivePanel;
    std::unique_ptr<Terrafront::TFVehicleTerminal> m_vehicleTerminals;
};
