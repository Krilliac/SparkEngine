/**
 * @file TFLoadoutScreen.h
 * @brief loadout-depth wave: the real loadout editor — primary/secondary/
 *        tool/grenade/suit + optic (W11) for the class currently picked in
 *        TFSpawnScreen, reachable from the sanctuary class terminal and the
 *        spawn screen.
 *
 * OWNERSHIP: loadout-depth lane (this header + TFLoadoutScreen.cpp).
 *
 * Design:
 *  - PURE PICKER, no server truth of its own. Reads TFDataTables for the
 *    candidate list per slot (faction pool + the selected class's slot
 *    categories, TFProgressionSystem::ValidLoadoutSlotKey precedent),
 *    TFProgressionSystem::IsWeaponUnlocked/ValidGrenadeChoiceKey/
 *    ValidSuitChoiceKey/AllSuits for lock state, and GetLoadout to seed the
 *    current picks on Open(). The optic row reads/writes TFOpticsSystem's
 *    session-local selection directly (TFOpticsSystem::AllowedMask/
 *    SetSelected) — that system remains the single source of truth for
 *    optics this wave (see its header note on why persistence isn't wired
 *    in yet).
 *  - SAVE sends the existing TF_LoadoutChange (primary/secondary/tool,
 *    Net/TFNetProtocol.h, frozen 8-byte layout) AND the new
 *    TF_LoadoutExtChange (grenade/suit, Game/TFProgressionSystem.h,
 *    reserved block 0x5488-0x548B) through TFClientNet::SendMsg, which
 *    loops back in-process on authority roles (TFSpawnScreen precedent) —
 *    no IsAuthority() branch needed here. Server-side validation
 *    (TFProgressionSystem::ServerSetLoadout / ServerSetLoadoutExt) is the
 *    actual gate; this screen's lock icons are advisory (a stale/pure-client
 *    view can only produce a rejected save, never a false grant).
 *  - Server-side effect: primary/secondary/tool remain validated-and-
 *    persisted only (pre-existing W6 gap — nothing currently reads them back
 *    into TFPlayerSystem::CreatePawnEntity's equip path; NOT fixed by this
 *    wave, out of scope). Grenade + suit ARE consumed at spawn
 *    (Game/TFGrenadeSystem.cpp OnPlayerSpawned / Game/TFPlayerSystem.cpp
 *    CreatePawnEntity respectively) — see those files' loadout-depth notes.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"

#include <string>

namespace Terrafront
{

    class TFLoadoutScreen
    {
      public:
        TFLoadoutScreen();
        ~TFLoadoutScreen();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void FixedUpdate(float fixedDeltaTime);
        void Shutdown();
        void RenderDebugUI();
        void RenderUI();

        /// Open the editor for the class currently picked in TFSpawnScreen
        /// (ctx.spawnUI->SelectedClass()). Seeds every row from the player's
        /// saved TFLoadout (empty slots == class default) and the local
        /// TFOpticsSystem selection.
        void Open();
        void Close();
        bool IsOpen() const { return m_open; }

      private:
        void SeedFromSaved();
        void SendSave();

        // ImGui internals (stubbed out when !SPARK_HAS_IMGUI, TFSpawnScreen pattern).
        void DrawPanel(float panelX, float panelY, float panelW, float panelH);
        void DrawWeaponRow(const char* label, int slotIdx, float x, float y, float w, float rowH);
        void DrawGrenadeRow(float x, float y, float w, float rowH);
        void DrawSuitRow(float x, float y, float w, float rowH);
        void DrawOpticRow(float x, float y, float w, float rowH);

        TFGameContext* m_ctx{nullptr};
        TFEventBus* m_events{nullptr};
        bool m_initialized{false};
        bool m_open{false};

        // Current picks (weapons.json keys; empty == class default). Slot
        // indices match TFProgressionSystem::ValidLoadoutSlotKey: 0 primary,
        // 1 secondary, 2 tool.
        std::string m_pick[3];
        std::string m_grenade;
        std::string m_suit;

        float m_saveFlashSec{0.0f}; ///< brief "SAVED" confirmation after a successful send
    };

} // namespace Terrafront
