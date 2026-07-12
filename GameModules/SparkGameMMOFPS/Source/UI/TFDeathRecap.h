/**
 * @file TFDeathRecap.h
 * @brief Death recap v2 panel (W11 death-recap lane): timeline of the hits
 *        that killed you, killer summary (name/outfit/class/HP left/distance)
 *        and the "damage dealt to killer" line.
 *
 * OWNERSHIP: death-recap lane (this header + TFDeathRecap.cpp, plus the
 * damage-log section of Game/TFDamageSystem.h/.cpp). Reserved TFMsg block
 * 0x5474-0x5477 — constants + packed structs live in Game/TFDamageSystem.h
 * (TFMedalSystem in-lane-header precedent).
 *
 * Design (client mirror of a server-only sender, TFMedalSystem pattern):
 *  - SERVER side lives entirely in TFDamageSystem: a per-pawn rolling damage
 *    log (last kTFDeathRecapWindowSec) recorded where damage applies; on death
 *    the victim — and only the victim — gets TF_DeathRecap (reliable).
 *  - CLIENT (this class): the kTFMsgDeathRecap handler self-registers when
 *    the socket connects (TFSquadSystem/TFMedalSystem EnsureClientHandlers
 *    pattern) — no TFClientNet edits. Listen-host/standalone victims are fed
 *    by TFDamageSystem's direct mirror hook (SetDeathRecapMirror, installed
 *    in Initialize; context pointers are published before any Initialize so
 *    boot order is irrelevant — TFClientNet::SetChatUI self-wiring pattern).
 *  - RenderUI EXTENDS the existing death screen without touching TFHUD
 *    (contended): TFHUD::DrawRespawnOverlay owns the center overlay text and
 *    DrawDeathActions the DEPLOY/MAP buttons at center+96 px; this panel
 *    renders below them (center+150 px, top-anchored) and hides while the
 *    map or spawn screens are open (DrawDeathActions precedent). It clears
 *    itself on the dead->alive edge of the local pawn (TFHUD m_wasViewAlive
 *    pattern).
 *  - Wire stays small: entries carry ids/amounts only. Weapon name + icon
 *    resolve client-side from WeaponId (WeaponDef.name / ui/128/wpn_<key>.png),
 *    killer name via the TFSocialSystem roster mirror + outfit tag
 *    (TFNameplates precedent), class name/icon via ClassDef + class_<key>.png.
 *
 *  - KILLCAM HOOK (killcam lane, Game/TFSpectator.h/.cpp + this lane's
 *    reserved TFMsg block 0x5484-0x5487): no new wire message. The killcam
 *    reuses TF_DeathRecap.killerPlayer (already delivered here, victim-only,
 *    reliable) as its "who" and the killer's live PawnInfo (already broadcast
 *    to ALL clients with no interest culling — see TFReplication) as its
 *    "where"; TFNetProtocol.h documents the block as reserved-but-unused for
 *    this reason. SetKillcamNotify below is a push callback (TFClientNet::
 *    SetChatUI self-wiring pattern) so TFSpectator starts the killcam on
 *    arrival instead of polling; wired from Main.cpp because neither system
 *    lives on TFGameContext.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Game/TFDamageSystem.h" // TF_DeathRecap wire structs + kTFMsgDeathRecap (this lane)

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

namespace Terrafront
{

    class TFDeathRecap
    {
      public:
        TFDeathRecap();
        ~TFDeathRecap();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderUI(); ///< recap panel on the death screen (local player only)
        void RenderDebugUI();

        /// Net-handler + local-mirror entry point: stash the recap; RenderUI
        /// draws it until the local pawn respawns.
        void ClientHandleRecap(const TF_DeathRecap& recap);

        /// Killcam lane hook: fires once per recap arrival with the killer id
        /// (kInvalidPlayer for an environment/unknown kill). Set nullptr to
        /// uninstall (TFSpectator does this in its own Shutdown before `this`
        /// goes away — SetDeathRecapMirror(nullptr) precedent below).
        using KillcamNotifyFn = std::function<void(PlayerId killer)>;
        void SetKillcamNotify(KillcamNotifyFn fn) { m_killcamNotify = std::move(fn); }

      private:
        PlayerId LocalPlayer() const;
        bool LocalPawnAlive() const;

        /// Killer/attacker display label: roster name (TFSocialSystem mirror)
        /// with the scoreboard "HOST"/"p<n>" fallback, outfit tag prepended.
        void ResolvePlayerLabel(PlayerId id, char* out, size_t outSize) const;

#ifdef ENABLE_NETWORKING
        bool ClientNetActive() const;
        void EnsureClientHandlers();
        void ReleaseClientHandlers();
#endif

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};
        bool m_clientHandlers{false};
        bool m_wasAlive{false}; ///< dead->alive edge clears the stashed recap

        TF_DeathRecap m_recap{};
        bool m_valid{false};

        // killcam lane hook (push notify on each recap arrival)
        KillcamNotifyFn m_killcamNotify;

        // debug counters
        uint32_t m_recapsReceived{0};
        uint32_t m_badPackets{0};
    };

} // namespace Terrafront
