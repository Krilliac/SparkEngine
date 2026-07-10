/**
 * @file TFMapScreen.h
 * @brief Continent hex map: ownership, lattice, capture progress, deploy.
 *
 * OWNERSHIP: this header + TFMapScreen.cpp belong to ONE implementation agent.
 * The lifecycle below is the frozen module contract (called from Main.cpp) —
 * extend this class freely, but do not change the lifecycle signatures.
 *
 * W2 implementation:
 *  - Fullscreen ImGui overlay (toggle key M, Escape closes) rendering the 13
 *    Cindral Wastes regions as a pointy-top axial hex grid from
 *    RegionDef.hexQ/hexR, auto-fitted to the viewport.
 *  - Fill = owner faction color (TFRegionSystem::OwnerOf), contested regions
 *    pulse, capture progress renders as a ring in the capturing faction's
 *    color, conduit lines connect neighbors, skyanchors get a home marker.
 *  - Legend with per-faction region counts (TFRegionSystem::RegionsHeld).
 *  - Click a region while DEAD and TFRegionSystem::CanSpawnAt says yes ->
 *    sends TF_SpawnRequest{spawnKind=1} (class from TFSpawnScreen::
 *    SelectedClass) and closes; while ALIVE a click stores a deploy hint
 *    (DeployHintRegion — W3 redeploy consumes it).
 *  - Hover tooltip: name / tier / owner / flux per tick / capture state.
 *
 * W7 (ui-map-keys, PlanetSide-spirit pass):
 *  - Keybinds go through UI/TFKeybinds.h (Action::OpenMap / CloseOverlay)
 *    instead of local VK constants; chat-open suppresses the M toggle.
 *  - Squad-member badges per region (TFSquadSystem::IsLocalSquadMember over
 *    the replicated alive-pawn set) next to the W6 friendly-presence badges.
 *  - Click a region while ALIVE -> inspect panel (owner / occupants /
 *    capture progress from replicated state) with a REDEPLOY button:
 *    friendly non-contested regions only (TFRedeployRules::CanRedeploy, the
 *    same predicate the server re-runs), client countdown
 *    (kTFRedeployCountdownSec), then TF_RedeployRequest; the server-validated
 *    TF_RedeployReply lands in OnRedeployReply (TFClientNet wiring). Death or
 *    entering a vehicle cancels the countdown. Disabled while seated.
 *  - tf_redeploy <regionId> console command (exec-harness/headless path for
 *    the same state machine).
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Net/TFRedeployProtocol.h"

namespace Terrafront
{

    class TFMapScreen
    {
      public:
        TFMapScreen();
        ~TFMapScreen();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();
        void RenderUI();

        // --- W2 public surface (consumed by TFSpawnScreen / console commands) ---
        void Open();
        void Close();
        void Toggle();
        bool IsOpen() const { return m_open; }

        /// Region clicked while alive ("rally here"); kInvalidRegion when unset.
        /// W3 redeploy consumes this; W2 renders it as a highlighted hex.
        RegionId DeployHintRegion() const { return m_deployHint; }
        void ClearDeployHint() { m_deployHint = kInvalidRegion; }

        // --- W7 ui-map-keys: redeploy + input suppression ----------------------
        /// TFClientNet routes TF_RedeployReply here (see lane wiringNotes).
        void OnRedeployReply(const TF_RedeployReply& rep);
        /// True while the fullscreen map wants gameplay input (WASD/mouse-look)
        /// suppressed — consumed by TFClientNet::PumpInput (wiringNotes).
        bool SuppressesGameInput() const { return m_open; }
        /// Redeploy countdown state (TFHUD renders a readout while the map is closed).
        bool RedeployPending() const { return m_redeployPending; }
        float RedeployCountdown() const { return m_redeployCountdown; }
        RegionId RedeployTarget() const { return m_redeployTarget; }
        /// Client-side pre-check: kTFRedeployOk (0) or a kTFRedeploy* denial reason.
        /// Baseline + extra rule via TFRedeployRules; server-cooldown mirrored locally.
        uint8_t CanRedeployTo(RegionId region) const;

      private:
        bool LocalPawnAlive() const;
        bool LocalSeated() const;
        void SendRegionSpawnRequest(RegionId region);

        // W7 redeploy state machine (works headless — no ImGui dependency).
        void StartRedeploy(RegionId region);
        void CancelRedeploy(const char* why);
        void SendRedeployRequest(RegionId region);
        void TickRedeploy(float dt);
        void SetStatus(const char* text, bool error);

        // Drawing internals live in the .cpp (no ImGui types leak into headers);
        // per-frame hex layout is cached in plain floats.
        void DrawMapContents();  ///< everything between the overlay Begin/End
        void DrawInspectPanel(); ///< W7: selected-region side panel (own window)

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};

        bool m_open{false};
        RegionId m_deployHint{kInvalidRegion};
        RegionId m_hovered{kInvalidRegion}; ///< refreshed every rendered frame
        float m_time{0.0f};                 ///< contested-pulse clock

        // W7 inspect + redeploy
        RegionId m_selected{kInvalidRegion}; ///< inspect-panel region (alive click)
        bool m_redeployPending{false};
        RegionId m_redeployTarget{kInvalidRegion};
        float m_redeployCountdown{0.0f}; ///< seconds until the request goes out
        float m_cooldownLeft{0.0f};      ///< local mirror of the server interval
        char m_status[96]{};             ///< last redeploy status/denial line
        float m_statusTTL{0.0f};
        bool m_statusIsError{false};
        bool m_redeployCmd{false}; ///< tf_redeploy registered by this instance

        // Per-frame layout cache (screen-space): hex size + axial origin.
        float m_hexSize{44.0f};
        float m_originX{0.0f};
        float m_originY{0.0f};
    };

} // namespace Terrafront
