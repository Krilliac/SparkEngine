/**
 * @file TFPingUI.h
 * @brief Ping input + screen-space presentation (W11): Q-tap context ping,
 *        Q-hold ping wheel, distance labels over the 3D markers, and the
 *        place/receive UI bleeps.
 *
 * OWNERSHIP: ping-system lane (this header + TFPingUI.cpp, alongside
 * Game/TFPingSystem.h/.cpp which owns wire + validation + the 3D markers).
 *
 * Input model (client-side; suppressed while any fullscreen UI owns input —
 * same gate set as TFGrenadeSystem::ClientPollThrowKey):
 *  - Q tap (released before kTFPingHoldSec): context ping. A camera ray is
 *    resolved against physics statics/vehicles/deployables (RaycastFiltered,
 *    kTFPingMaxRangeM) AND the analytic terrain heightfield (physics never
 *    contains the terrain — TFWorldCollision keeps it analytic, so the ray is
 *    marched against TerrainHeightAt and the nearer hit wins). An enemy pawn
 *    inside the nameplate aim cone (kTFNameplateAimConeCos, TFNameplates.h)
 *    that is not occluded by the world hit wins over both -> Enemy ping (red);
 *    otherwise the world point -> Location ping (yellow).
 *  - Q hold (>= kTFPingHoldSec): a small centered wheel appears while Q stays
 *    down; pressing 1 / 2 / 3 places Location / Enemy / Need-Support and
 *    closes it; releasing Q without a selection closes it silently. Selection
 *    is by number key because the gameplay cursor stays captured while alive
 *    (mouse-radial selection would need TFClientNet input changes — contended).
 *  - The key is a runtime seam (SetPingKey, default 'Q') so the ui-keys lane
 *    can route it through TFKeybinds without touching this lane
 *    (TFGrenadeSystem::SetThrowKey precedent).
 *
 * Presentation:
 *  - Distance labels ("<type> NNm") are drawn on the ImGui foreground
 *    drawlist under each live marker, projected through the SAME matrices the
 *    frame renders with (TFNameplates ProjectToScreen mirror — see the
 *    MUST-MATCH note in the .cpp).
 *  - Audio: terminal_bleep_02.wav — brighter/louder on place, softer on
 *    receiving a squadmate's ping (new-id detection over the mirror, so it
 *    behaves identically on listen host and pure clients).
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

#include <cstdint>
#include <unordered_set>

namespace Terrafront
{

    class TFPingSystem;
    enum class TFPingType : uint8_t;

    /// Q must be held this long for the wheel (a shorter press is a tap).
    constexpr float kTFPingHoldSec = 0.35f;

    class TFPingUI
    {
      public:
        TFPingUI();
        ~TFPingUI();

        /// 3-arg Initialize (TFOutfitPanel precedent): takes the ping system by
        /// reference so this UI never depends on the ctx publish order.
        bool Initialize(TFGameContext& ctx, TFEventBus& events, TFPingSystem& pings);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();
        void RenderUI();

        /// Rebind seam for the ui-keys lane (TFKeybinds::BindKey pattern —
        /// callable at runtime, no compile-time coupling). VK code; default 'Q'.
        void SetPingKey(int vk) { m_pingVk = vk; }

        /// True while the hold-wheel is showing (input-suppression queries).
        bool IsWheelOpen() const { return m_wheelOpen; }

      private:
        bool FullscreenUiOpen() const; ///< same gate set as the grenade key poll
        bool LocalPawnAlive() const;
        void PollPingKey(float dt);
        void PollWheelSelection();
        void ResetInputState();

        /// Camera ray vs physics statics + analytic terrain; nearest hit.
        bool ResolveWorldPoint(const float camPos[3], const float fwd[3], float outPos[3], float& outDist) const;
        /// Enemy pawn in the aim cone, un-occluded by `worldDist`. 0 = none.
        EntityId ResolveConeEnemy(const float camPos[3], const float fwd[3], float worldDist, float outPos[3]) const;
        void PlaceContextPing();
        void PlaceWheelPing(TFPingType type);

        void SweepReceiveAudio(); ///< new-id detection over the mirror -> bleeps

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        TFPingSystem* m_pings{nullptr};
        bool m_initialized{false};

        int m_pingVk{'Q'};
        bool m_armed{false}; ///< key went down in-world (tap/hold pending)
        float m_holdSec{0.0f};
        bool m_wheelOpen{false};

        /// Mirror ids already heard (receive-bleep dedup). Pruned against the
        /// live mirror every sweep so it never grows past the live set. Own
        /// pings never receive-bleep — the optimistic place bleep covers them.
        std::unordered_set<uint16_t> m_heard;

        // debug counters
        uint32_t m_tapPings{0}, m_wheelPings{0}, m_noTarget{0};
    };

} // namespace Terrafront
