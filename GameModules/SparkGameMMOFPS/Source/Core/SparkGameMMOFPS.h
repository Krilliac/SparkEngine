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
    class TFNameplates;      // W10 nameplates lane (UI/TFNameplates.h)
    class TFMedalSystem;     // W10 medals-scoreboard lane (Game/TFMedalSystem.h)
    class TFFootsteps;       // audio-wave-2 lane (W10): footstep audio
    class TFSquadHUD;        // W11 squad-v2 lane (UI/TFSquadHUD.h)
    class TFPingUI;          // W11 ping-system lane (UI/TFPingUI.h; TFPingSystem fwd-decl comes from TFTypes.h)
    class TFAlertSystem;     // W11 alerts lane (World/TFAlertSystem.h)
    class TFDeathRecap;      // W11 death-recap lane (UI/TFDeathRecap.h)
    class TFDayNight;        // W12 time-of-day lane (World/TFDayNight.h)
    class TFSpectator;       // W12 spectator-mode lane (Game/TFSpectator.h)
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
    std::unique_ptr<Terrafront::TFFootsteps> m_footsteps; // audio-wave-2 lane (W10)
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

    // grenades lane (W10): booted after abilities, before bots (bot AI may
    // call CanThrowGrenade/ServerBotThrowGrenade). Fwd-decl from TFTypes.h.
    std::unique_ptr<Terrafront::TFGrenadeSystem> m_grenades;

    // W10 medals-scoreboard lane: event-driven medal detection + score rows.
    std::unique_ptr<Terrafront::TFMedalSystem> m_medals;

    // W11 ping-system lane: squad-scoped pings + Q-key input/label layer.
    std::unique_ptr<Terrafront::TFPingSystem> m_pingSystem;
    std::unique_ptr<Terrafront::TFPingUI> m_pingUI;

    // W11 alerts lane: continent timed events (Territory Rush / Facility
    // Control) — scheduler + scoring + TF_AlertState wire + HUD banner.
    std::unique_ptr<Terrafront::TFAlertSystem> m_alerts;

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

    // W10 nameplates lane (additive): booted after the W8 ui-polish systems.
    std::unique_ptr<Terrafront::TFNameplates> m_nameplates;

    // W11 squad-v2 lane (additive): squad list HUD + waypoint beacon.
    std::unique_ptr<Terrafront::TFSquadHUD> m_squadHUD;

    // W11 death-recap lane (additive): client recap panel — pure consumer of
    // TF_DeathRecap / TFDamageSystem's local mirror; booted after nameplates.
    std::unique_ptr<Terrafront::TFDeathRecap> m_deathRecap;

    // W12 time-of-day lane (additive): server-authoritative day/night clock +
    // TF_TimeOfDay sync (0x5478) + basic-path lighting drive. Main.cpp wiring
    // applied by the integrator (see W12 wiringNotes).
    std::unique_ptr<Terrafront::TFDayNight> m_dayNight;

    // W12 spectator-mode lane (additive): client-only dead-time camera —
    // squadmate follow-cam + bounded free-fly fallback. No wire messages, no
    // server state. Main.cpp wiring applied by the integrator (see W12
    // spectator-mode wiringNotes).
    std::unique_ptr<Terrafront::TFSpectator> m_spectator;
};
