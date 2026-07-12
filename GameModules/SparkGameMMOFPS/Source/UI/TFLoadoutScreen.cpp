/**
 * @file TFLoadoutScreen.cpp
 * @brief loadout-depth wave: TFLoadoutScreen implementation. See the header
 *        for the full design; this is a pure picker over TFDataTables /
 *        TFProgressionSystem / TFOpticsSystem query surfaces, no server
 *        truth of its own.
 */
#include "UI/TFLoadoutScreen.h"

#include "Data/TFDataTables.h"
#include "Game/TFOpticsSystem.h"
#include "Game/TFProgressionSystem.h"
#include "Net/TFClientNet.h"
#include "Net/TFNetProtocol.h"
#include "UI/TFMapScreen.h"
#include "UI/TFSpawnScreen.h"
#include "UI/TFUiCommon.h"

#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace Terrafront
{

    namespace
    {

        struct Candidate
        {
            std::string key;  // weapons.json / suits.json key; "" == class default / no passive
            std::string name; // display name
            bool locked = false;
        };

        // ValidLoadoutSlotKey's own category sets (Game/TFProgressionSystem.cpp) —
        // mirrored here so the picker only ever offers what the server would
        // actually accept.
        bool SlotMatches(int slotIdx, const std::string& slot)
        {
            switch (slotIdx)
            {
            case 0: // primary
                return slot == "rifle" || slot == "carbine" || slot == "lmg" || slot == "sniper" ||
                       slot == "shotgun" || slot == "launcher";
            case 1: // secondary
                return slot == "pistol";
            default: // tool
                return slot == "tool";
            }
        }

        int FindIndex(const std::vector<Candidate>& list, const std::string& key)
        {
            for (size_t i = 0; i < list.size(); ++i)
                if (list[i].key == key)
                    return static_cast<int>(i);
            return 0; // key not found among current candidates (e.g. faction switched): fall back to default
        }

        /// Advances `idx` by `dir` (+-1), wrapping, skipping locked entries when
        /// at least one unlocked entry exists — so the UI can never cycle INTO
        /// a pick the server would reject.
        void StepIndex(const std::vector<Candidate>& list, int& idx, int dir)
        {
            if (list.empty())
                return;
            const int n = static_cast<int>(list.size());
            const bool anyUnlocked = std::any_of(list.begin(), list.end(), [](const Candidate& c) { return !c.locked; });
            for (int tries = 0; tries < n; ++tries)
            {
                idx = ((idx + dir) % n + n) % n;
                if (!anyUnlocked || !list[idx].locked)
                    return;
            }
        }

    } // namespace

    TFLoadoutScreen::TFLoadoutScreen() = default;
    TFLoadoutScreen::~TFLoadoutScreen()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFLoadoutScreen::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFLoadoutScreen initialized");
        return true;
    }

    void TFLoadoutScreen::FixedUpdate(float) {}

    void TFLoadoutScreen::Shutdown()
    {
        m_open = false;
        m_initialized = false;
    }

    void TFLoadoutScreen::RenderDebugUI() {}

    void TFLoadoutScreen::Update(float deltaTime)
    {
        if (!m_initialized)
            return;
        m_saveFlashSec = std::max(0.0f, m_saveFlashSec - deltaTime);
        // Closed elsewhere (e.g. TFSpawnScreen taking over the panel, or a
        // fullscreen UI opening on top) — nothing to poll; RenderUI just won't
        // draw while !m_open.
    }

    void TFLoadoutScreen::Open()
    {
        if (m_open || !m_initialized)
            return;
        m_open = true;
        SeedFromSaved();
    }

    void TFLoadoutScreen::Close()
    {
        m_open = false;
    }

    void TFLoadoutScreen::SeedFromSaved()
    {
        m_pick[0] = m_pick[1] = m_pick[2] = {};
        m_grenade.clear();
        m_suit.clear();
        if (!m_ctx || !m_ctx->progression || m_ctx->localPlayer == kInvalidPlayer)
            return;
        if (const TFLoadout* lo = m_ctx->progression->GetLoadout(m_ctx->localPlayer))
        {
            m_pick[0] = lo->primary;
            m_pick[1] = lo->secondary;
            m_pick[2] = lo->tool;
            m_grenade = lo->grenade;
            m_suit = lo->suit;
        }
    }

    void TFLoadoutScreen::SendSave()
    {
        if (!m_ctx || !m_ctx->clientNet || !m_ctx->clientNet->IsConnected() || !m_ctx->data || !m_ctx->data->IsLoaded())
            return;

        // Existing frozen triad (Net/TFNetProtocol.h TF_LoadoutChange). Empty
        // pick == class default == kInvalidWeapon on the wire (TFServerSim's
        // decode already treats kInvalidWeapon as "" — see its LoadoutChange
        // case).
        auto idOf = [&](const std::string& key) -> uint16_t
        {
            if (key.empty())
                return static_cast<uint16_t>(kInvalidWeapon);
            const WeaponDef* def = m_ctx->data->GetWeaponByKey(key);
            return def ? static_cast<uint16_t>(def->id) : static_cast<uint16_t>(kInvalidWeapon);
        };
        TF_LoadoutChange lc{};
        lc.classId = static_cast<uint8_t>(m_ctx->spawnUI ? m_ctx->spawnUI->SelectedClass() : ClassId::Striker);
        lc.primary = idOf(m_pick[0]);
        lc.secondary = idOf(m_pick[1]);
        lc.tool = idOf(m_pick[2]);
        m_ctx->clientNet->SendMsg(TFMsg::LoadoutChange, &lc, sizeof(lc));

        // loadout-depth wave: grenade + suit (reserved block 0x5488-0x548B).
        TF_LoadoutExtChange ext{};
        std::memset(&ext, 0, sizeof(ext));
        std::snprintf(ext.grenadeKey, sizeof(ext.grenadeKey), "%s", m_grenade.c_str());
        std::snprintf(ext.suitKey, sizeof(ext.suitKey), "%s", m_suit.c_str());
        m_ctx->clientNet->SendMsg(static_cast<TFMsg>(kTFMsgLoadoutExtChange), &ext, sizeof(ext));

        m_saveFlashSec = 1.6f;
    }

    // ---------------------------------------------------------------------------
    // Rendering
    // ---------------------------------------------------------------------------

#ifdef SPARK_HAS_IMGUI

    void TFLoadoutScreen::RenderUI()
    {
        if (!m_open || !m_initialized || !m_ctx || !m_ctx->HasLocalPlayer())
            return;
        if (!m_ctx->data || !m_ctx->data->IsLoaded())
            return;
        // Yield to any other fullscreen UI already on top (map/spawn/scoreboard
        // precedent, TFSpawnScreen::RenderUI's map-open check).
        if (m_ctx->map && m_ctx->map->IsOpen())
            return;

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        if (!ImGui::Begin("##TFLoadoutScreen", nullptr, flags))
        {
            ImGui::End();
            return;
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(6, 8, 12, 215));

        const float panelW = std::min(760.0f, vp->Size.x * 0.90f);
        const float panelH = std::min(560.0f, vp->Size.y * 0.86f);
        const float panelX = vp->Pos.x + (vp->Size.x - panelW) * 0.5f;
        const float panelY = vp->Pos.y + (vp->Size.y - panelH) * 0.5f;
        dl->AddRectFilled(ImVec2(panelX, panelY), ImVec2(panelX + panelW, panelY + panelH), IM_COL32(16, 19, 25, 235),
                          6.0f);
        dl->AddRect(ImVec2(panelX, panelY), ImVec2(panelX + panelW, panelY + panelH),
                    TFUi::FactionCol(m_ctx->localFaction, 0.55f), 6.0f, 0, 2.0f);

        DrawPanel(panelX, panelY, panelW, panelH);

        ImGui::End();
    }

    void TFLoadoutScreen::DrawPanel(float panelX, float panelY, float panelW, float panelH)
    {
        using namespace TFUi;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        const ClassId cls = m_ctx->spawnUI ? m_ctx->spawnUI->SelectedClass() : ClassId::Striker;
        const ClassDef* cd = m_ctx->data->GetClass(cls);

        char buf[160];
        std::snprintf(buf, sizeof(buf), "LOADOUT  -  %s", cd ? cd->name.c_str() : "?");
        AddTextCentered(dl, 24.0f, ImVec2(panelX + panelW * 0.5f, panelY + 30.0f), IM_COL32(230, 230, 235, 235), buf);

        float rowY = panelY + 66.0f;
        const float rowH = 58.0f;
        const float x = panelX + 24.0f;
        const float w = panelW - 48.0f;

        DrawWeaponRow("PRIMARY", 0, x, rowY, w, rowH);
        rowY += rowH + 8.0f;
        DrawWeaponRow("SECONDARY", 1, x, rowY, w, rowH);
        rowY += rowH + 8.0f;
        DrawWeaponRow("TOOL", 2, x, rowY, w, rowH);
        rowY += rowH + 8.0f;
        DrawGrenadeRow(x, rowY, w, rowH);
        rowY += rowH + 8.0f;
        DrawSuitRow(x, rowY, w, rowH);
        rowY += rowH + 8.0f;
        DrawOpticRow(x, rowY, w, rowH);
        rowY += rowH + 8.0f;

        const float footY = panelY + panelH - 56.0f;
        ImGui::SetCursorScreenPos(ImVec2(x, footY));
        if (m_saveFlashSec > 0.0f)
            ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.55f, 1.0f), "Saved - takes effect on your next deploy");
        else
            ImGui::TextDisabled("Locked picks are shown greyed; SAVE stores your legal picks only.");

        ImGui::SetCursorScreenPos(ImVec2(panelX + panelW - 300.0f, footY - 4.0f));
        if (ImGui::Button("CLOSE", ImVec2(130.0f, 40.0f)))
            Close();
        ImGui::SetCursorScreenPos(ImVec2(panelX + panelW - 160.0f, footY - 4.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.42f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.56f, 0.26f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.24f, 0.68f, 0.30f, 1.0f));
        if (ImGui::Button("SAVE", ImVec2(130.0f, 40.0f)))
            SendSave();
        ImGui::PopStyleColor(3);
    }

    void TFLoadoutScreen::DrawWeaponRow(const char* label, int slotIdx, float x, float y, float w, float rowH)
    {
        using namespace TFUi;
        (void)rowH;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        std::vector<Candidate> list;
        list.push_back({"", "Class Default", false});
        const FactionId myFaction = m_ctx->localFaction;
        const PlayerId me = m_ctx->localPlayer;
        for (const WeaponDef& wd : m_ctx->data->AllWeapons())
        {
            if (!SlotMatches(slotIdx, wd.slot))
                continue;
            if (wd.faction != FactionId::None && wd.faction != myFaction)
                continue;
            const bool locked = m_ctx->progression && !m_ctx->progression->IsWeaponUnlocked(me, wd.id);
            list.push_back({wd.key, wd.name, locked});
        }

        int idx = FindIndex(list, m_pick[slotIdx]);
        AddTextCentered(dl, 14.0f, ImVec2(x + 78.0f, y + 12.0f), IM_COL32(170, 174, 180, 220), label);

        ImGui::SetCursorScreenPos(ImVec2(x, y + 26.0f));
        ImGui::PushID(slotIdx);
        if (ImGui::ArrowButton("##prev", ImGuiDir_Left))
        {
            StepIndex(list, idx, -1);
            m_pick[slotIdx] = list[idx].key;
        }
        ImGui::SameLine();
        const Candidate& cur = list[idx];
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s%s", cur.name.c_str(), cur.locked ? "  (locked)" : "");
        ImGui::TextColored(cur.locked ? ImVec4(0.75f, 0.45f, 0.40f, 1.0f) : ImVec4(0.90f, 0.92f, 0.95f, 1.0f), "%s",
                           buf);
        ImGui::SameLine(w - 40.0f);
        if (ImGui::ArrowButton("##next", ImGuiDir_Right))
        {
            StepIndex(list, idx, 1);
            m_pick[slotIdx] = list[idx].key;
        }
        ImGui::PopID();
    }

    void TFLoadoutScreen::DrawGrenadeRow(float x, float y, float w, float rowH)
    {
        using namespace TFUi;
        (void)rowH;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        std::vector<Candidate> list;
        list.push_back({"", "Frag Grenade", false});
        const PlayerId me = m_ctx->localPlayer;
        for (const char* key : {"smoke_grenade", "flash_grenade"})
        {
            const WeaponDef* wd = m_ctx->data->GetWeaponByKey(key);
            if (!wd)
                continue;
            const bool locked = !m_ctx->progression || !m_ctx->progression->ValidGrenadeChoiceKey(me, key) ||
                                !m_ctx->progression->IsWeaponUnlocked(me, wd->id);
            list.push_back({wd->key, wd->name, locked});
        }

        int idx = FindIndex(list, m_grenade);
        AddTextCentered(dl, 14.0f, ImVec2(x + 78.0f, y + 12.0f), IM_COL32(170, 174, 180, 220), "GRENADE");

        ImGui::SetCursorScreenPos(ImVec2(x, y + 26.0f));
        ImGui::PushID("grenade");
        if (ImGui::ArrowButton("##prev", ImGuiDir_Left))
        {
            StepIndex(list, idx, -1);
            m_grenade = list[idx].key;
        }
        ImGui::SameLine();
        const Candidate& cur = list[idx];
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s%s", cur.name.c_str(), cur.locked ? "  (locked)" : "");
        ImGui::TextColored(cur.locked ? ImVec4(0.75f, 0.45f, 0.40f, 1.0f) : ImVec4(0.90f, 0.92f, 0.95f, 1.0f), "%s",
                           buf);
        ImGui::SameLine(w - 40.0f);
        if (ImGui::ArrowButton("##next", ImGuiDir_Right))
        {
            StepIndex(list, idx, 1);
            m_grenade = list[idx].key;
        }
        ImGui::PopID();
    }

    void TFLoadoutScreen::DrawSuitRow(float x, float y, float w, float rowH)
    {
        using namespace TFUi;
        (void)rowH;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        std::vector<Candidate> list;
        list.push_back({"", "None", false});
        const PlayerId me = m_ctx->localPlayer;
        if (m_ctx->progression)
        {
            // Stable order for a stable cycling experience (unordered_map
            // iteration order is not guaranteed) — sort by key.
            std::vector<const TFSuitDef*> defs;
            for (const auto& kv : m_ctx->progression->AllSuits())
                defs.push_back(&kv.second);
            std::sort(defs.begin(), defs.end(), [](const TFSuitDef* a, const TFSuitDef* b) { return a->key < b->key; });
            for (const TFSuitDef* def : defs)
                list.push_back({def->key, def->name, !m_ctx->progression->ValidSuitChoiceKey(me, def->key)});
        }

        int idx = FindIndex(list, m_suit);
        AddTextCentered(dl, 14.0f, ImVec2(x + 78.0f, y + 12.0f), IM_COL32(170, 174, 180, 220), "SUIT");

        ImGui::SetCursorScreenPos(ImVec2(x, y + 26.0f));
        ImGui::PushID("suit");
        if (ImGui::ArrowButton("##prev", ImGuiDir_Left))
        {
            StepIndex(list, idx, -1);
            m_suit = list[idx].key;
        }
        ImGui::SameLine();
        const Candidate& cur = list[idx];
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s%s", cur.name.c_str(), cur.locked ? "  (locked)" : "");
        ImGui::TextColored(cur.locked ? ImVec4(0.75f, 0.45f, 0.40f, 1.0f) : ImVec4(0.90f, 0.92f, 0.95f, 1.0f), "%s",
                           buf);
        ImGui::SameLine(w - 40.0f);
        if (ImGui::ArrowButton("##next", ImGuiDir_Right))
        {
            StepIndex(list, idx, 1);
            m_suit = list[idx].key;
        }
        ImGui::PopID();
    }

    void TFLoadoutScreen::DrawOpticRow(float x, float y, float w, float rowH)
    {
        using namespace TFUi;
        (void)rowH;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        AddTextCentered(dl, 14.0f, ImVec2(x + 78.0f, y + 12.0f), IM_COL32(170, 174, 180, 220), "OPTIC (primary)");

        // The primary row above may be "Class Default" (m_pick[0] empty) — the
        // optic table is keyed by weapons.json key, so resolve the concrete
        // default weapon for a meaningful row instead of silently no-op'ing.
        std::string primaryKey = m_pick[0];
        if (primaryKey.empty())
        {
            const ClassId cls = m_ctx->spawnUI ? m_ctx->spawnUI->SelectedClass() : ClassId::Striker;
            const ClassDef* cd = m_ctx->data->GetClass(cls);
            if (cd && !cd->primarySlots.empty())
            {
                for (const WeaponDef& wd : m_ctx->data->AllWeapons())
                    if (wd.slot == cd->primarySlots.front() &&
                        (wd.faction == FactionId::None || wd.faction == m_ctx->localFaction))
                    {
                        primaryKey = wd.key;
                        break;
                    }
            }
        }

        TFOpticsSystem& optics = TFOpticsSystem::Get();
        const uint8_t mask = primaryKey.empty() ? 0u : optics.AllowedMask(primaryKey);
        static const char* kNames[3] = {"Iron Sights", "Red Dot", "4x Scope"};

        ImGui::SetCursorScreenPos(ImVec2(x, y + 26.0f));
        ImGui::PushID("optic");
        if (mask == 0u || primaryKey.empty())
        {
            ImGui::TextDisabled("(pick a primary first)");
        }
        else
        {
            int cur = static_cast<int>(optics.ActiveOptic());
            // ActiveOptic() reflects whatever is currently HELD in-world; while
            // browsing the menu (no live weapon context) it simply stays at its
            // last value — good enough for a "current sight" preview, and B
            // still cycles live in-game exactly as before (W11 unchanged).
            if (ImGui::ArrowButton("##prev", ImGuiDir_Left))
            {
                int n = cur;
                for (int tries = 0; tries < 3; ++tries)
                {
                    n = (n + 2) % 3; // -1 mod 3
                    if (mask & (1u << n))
                        break;
                }
                optics.SetSelected(primaryKey, static_cast<TFOpticKind>(n));
            }
            ImGui::SameLine();
            ImGui::Text("%s", (mask & (1u << cur)) ? kNames[cur] : "Iron Sights");
            ImGui::SameLine(w - 40.0f);
            if (ImGui::ArrowButton("##next", ImGuiDir_Right))
            {
                int n = cur;
                for (int tries = 0; tries < 3; ++tries)
                {
                    n = (n + 1) % 3;
                    if (mask & (1u << n))
                        break;
                }
                optics.SetSelected(primaryKey, static_cast<TFOpticKind>(n));
            }
        }
        ImGui::PopID();
    }

#else // !SPARK_HAS_IMGUI — headless: screen is state-only

    void TFLoadoutScreen::RenderUI() {}
    void TFLoadoutScreen::DrawPanel(float, float, float, float) {}
    void TFLoadoutScreen::DrawWeaponRow(const char*, int, float, float, float, float) {}
    void TFLoadoutScreen::DrawGrenadeRow(float, float, float, float) {}
    void TFLoadoutScreen::DrawSuitRow(float, float, float, float) {}
    void TFLoadoutScreen::DrawOpticRow(float, float, float, float) {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
