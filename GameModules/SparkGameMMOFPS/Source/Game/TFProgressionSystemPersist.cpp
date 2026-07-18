/**
 * @file TFProgressionSystemPersist.cpp
 * @brief TFProgressionSystem JSON persistence: read-modify-write of the
 *        "progression" key inside Saves/terrafront_state.json (the file is
 *        shared with territory state), written atomically via tmp+rename,
 *        plus the durable per-character flush through TFCharacterSystem /
 *        TFDatabase. Split from TFProgressionSystem.cpp; shared helpers live
 *        in TFProgressionSystemInternal.h.
 */
#include "Game/TFProgressionSystem.h"

#include "Account/TFCharacterSystem.h" // W5 onboarding (Task 6): re-key persistence
#include "Game/TFProgressionSystemInternal.h"
#include "Net/TFServerSim.h"        // W5 onboarding (Task 6): ActiveCharacterOf
#include "Persistence/TFDatabase.h" // W6: SaveCharacterMeta flush target
#include "Utils/JsonUtils.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace Terrafront
{

    using namespace ProgressionDetail;

    namespace
    {

        constexpr const char* kSaveDir = "Saves";
        constexpr const char* kSaveFile = "Saves/terrafront_state.json";
        constexpr const char* kTmpFile = "Saves/terrafront_state.prog.tmp";

    } // namespace

    // ---------------------------------------------------------------------------
    // Persistence (shared Saves/terrafront_state.json; "progression" key only)
    // ---------------------------------------------------------------------------

    bool TFProgressionSystem::LoadFromDisk()
    {
        std::string text;
        if (!ReadAllText(kSaveFile, text))
            return false; // first boot: nothing saved yet

        const Spark::Json::Value root = Spark::Json::Parse(text);
        if (!root.IsObject() || !root.HasKey("progression"))
            return false;

        const Spark::Json::Value& plist = root["progression"]["players"];
        if (!plist.IsArray())
            return false;

        for (size_t i = 0; i < plist.Size(); ++i)
        {
            const Spark::Json::Value& row = plist[i];
            if (!row.IsObject())
                continue;
            const auto id = static_cast<PlayerId>(row["id"].AsNumber(0.0));
            if (id == kInvalidPlayer)
                continue;
            Prog rec;
            rec.xp = static_cast<uint32_t>(row["xp"].AsNumber(0.0));
            rec.flux = std::min(kFluxWalletCap, static_cast<uint32_t>(row["flux"].AsNumber(0.0)));
            rec.rank = RankForXP(rec.xp); // rank derives from xp, not the file
            m_players[id] = rec;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] progression loaded: %zu players from %s", m_players.size(),
                       kSaveFile);
        return true;
    }

    bool TFProgressionSystem::SaveNow()
    {
        namespace fs = std::filesystem;

        // Read-modify-write so co-resident sections (territory) are preserved.
        Spark::Json::Value root;
        std::string text;
        if (ReadAllText(kSaveFile, text))
            root = Spark::Json::Parse(text);
        if (!root.IsObject())
            root = Spark::Json::Value::MakeObject();

        Spark::Json::Value plist = Spark::Json::Value::MakeArray();
        for (const auto& [id, rec] : m_players)
        {
            Spark::Json::Value row = Spark::Json::Value::MakeObject();
            row["id"] = Spark::Json::Value(static_cast<double>(id));
            row["xp"] = Spark::Json::Value(static_cast<double>(rec.xp));
            row["rank"] = Spark::Json::Value(static_cast<int>(rec.rank));
            row["flux"] = Spark::Json::Value(static_cast<double>(rec.flux));
            plist.PushBack(std::move(row));

            // W5 onboarding (Task 6): re-key progression to the entered
            // character. The in-session runtime state above stays PlayerId-keyed
            // (unchanged, low risk); TFCharacterSystem/TFDatabase become the
            // durable per-character store, keyed by the character bound at
            // enter-world (TFServerSim::HandleEnterWorld). Players who never
            // completed onboarding (ActiveCharacterOf==0, e.g. bots or a
            // pre-Task-6 session) simply are not persisted here.
            if (m_ctx->characters && m_ctx->serverSim)
            {
                const uint64_t charId = m_ctx->serverSim->ActiveCharacterOf(id);
                if (charId != 0)
                    m_ctx->characters->PersistProgress(charId, rec.xp, rec.rank, rec.flux);
            }
        }
        Spark::Json::Value prog = Spark::Json::Value::MakeObject();
        prog["note"] = Spark::Json::Value("PlayerIds are session-scoped in W2");
        prog["players"] = std::move(plist);
        root["progression"] = std::move(prog);

        // W6: flush dirty unlock/loadout/stat meta to the character db on the
        // same cadence. Swept over the whole store (not the m_players loop) so a
        // player whose only mutation was meta (e.g. a loadout save before first
        // spawn) is not skipped. Character-bound records only; charId==0 rows
        // (bots, standalone sessions) are session-scoped by design.
        if (m_ctx && m_ctx->db)
            m_meta.PersistAllDirty(*m_ctx->db);

        std::error_code ec;
        fs::create_directories(kSaveDir, ec);

        {
            std::ofstream out(kTmpFile, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] progression save failed: cannot open %s", kTmpFile);
                return false;
            }
            out << Spark::Json::StringifyPretty(root);
            if (!out.good())
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] progression save failed: short write to %s", kTmpFile);
                return false;
            }
        }

        fs::rename(kTmpFile, kSaveFile, ec); // atomic replace (MoveFileEx semantics)
        if (ec)
        {
            fs::remove(kSaveFile, ec);
            fs::rename(kTmpFile, kSaveFile, ec);
            if (ec)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] progression save failed: rename -> %s (%s)", kSaveFile,
                                ec.message().c_str());
                return false;
            }
        }

        m_dirty = false;
        m_sinceSave = 0.0f;
        ++m_saves;
        return true;
    }

} // namespace Terrafront
