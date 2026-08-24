/**
 * @file TFProgressionSystemPersist.cpp
 * @brief TFProgressionSystem JSON persistence: read-modify-write of the
 *        "progression" key inside terrafront_state.<continent-key>.json,
 *        written under the shared TERRAFRONT save root atomically via tmp+rename,
 *        plus the durable per-character flush through TFCharacterSystem /
 *        TFDatabase. Split from TFProgressionSystem.cpp; shared helpers live
 *        in TFProgressionSystemInternal.h.
 */
#include "Game/TFProgressionSystem.h"

#include "Account/TFCharacterSystem.h" // W5 onboarding (Task 6): re-key persistence
#include "Data/TFDataTables.h"
#include "Game/TFProgressionSystemInternal.h"
#include "Net/TFServerSim.h"        // W5 onboarding (Task 6): ActiveCharacterOf
#include "Persistence/TFDatabase.h" // W6: SaveCharacterMeta flush target
#include "Persistence/TFSavePaths.h"
#include "Persistence/TFWorldSave.h"
#include "Utils/JsonUtils.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace Terrafront
{

    using namespace ProgressionDetail;

    // ---------------------------------------------------------------------------
    // Persistence (continent-qualified terrafront_state JSON; "progression" key only)
    // ---------------------------------------------------------------------------

    bool TFProgressionSystem::LoadFromDisk()
    {
        using WorldSave::ReadStatus;
        if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded())
        {
            m_persistenceBlocked = true;
            return false;
        }
        const ContinentDef& continent = m_ctx->data->GetContinent();
        const std::filesystem::path saveFile = SavePaths::ContinentFile("terrafront_state", continent.key);
        if (saveFile.empty())
        {
            m_persistenceBlocked = true;
            return false;
        }

        Spark::Json::Value root;
        std::string detail;
        ReadStatus status = WorldSave::ReadJson(saveFile, continent.key, continent.name, false, root, detail);
        bool migratingLegacy = false;
        std::filesystem::path source = saveFile;
        if (status == ReadStatus::Missing)
        {
            const std::filesystem::path candidates[] = {SavePaths::File("terrafront_state.json"),
                                                        SavePaths::LegacyExecutableFile("terrafront_state.json")};
            for (const std::filesystem::path& candidate : candidates)
            {
                if (candidate.empty() || candidate == saveFile || candidate == source)
                    continue;
                Spark::Json::Value legacyRoot;
                std::string legacyDetail;
                const ReadStatus legacyStatus =
                    WorldSave::ReadJson(candidate, continent.key, continent.name, true, legacyRoot, legacyDetail);
                if (legacyStatus == ReadStatus::Loaded)
                {
                    root = std::move(legacyRoot);
                    source = candidate;
                    status = ReadStatus::Loaded;
                    migratingLegacy = true;
                    break;
                }
                if (legacyStatus != ReadStatus::Missing && legacyStatus != ReadStatus::WrongContinent)
                    SPARK_LOG_WARN(Spark::LogCategory::Game,
                                   "[TF] legacy progression %s was not migrated (%s); original preserved",
                                   SavePaths::Utf8ForLog(candidate).c_str(), legacyDetail.c_str());
            }
        }
        if (status == ReadStatus::Missing)
            return false;
        if (status == ReadStatus::WrongContinent)
        {
            m_persistenceBlocked = true;
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "[TF] qualified progression %s belongs to another continent; writes latched off",
                            SavePaths::Utf8ForLog(saveFile).c_str());
            return false;
        }
        if (status != ReadStatus::Loaded)
        {
            m_persistenceBlocked = true;
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] progression %s unreadable/corrupt (%s); writes latched off",
                            SavePaths::Utf8ForLog(saveFile).c_str(), detail.c_str());
            return false;
        }

        const Spark::Json::Value& plist = root["progression"]["players"];
        if (!root["progression"].IsObject() || !plist.IsArray())
        {
            m_persistenceBlocked = !migratingLegacy;
            return false;
        }

        std::unordered_map<PlayerId, Prog> loaded;
        for (size_t i = 0; i < plist.Size(); ++i)
        {
            const Spark::Json::Value& row = plist[i];
            uint32_t rawId = 0;
            uint32_t xp = 0;
            uint32_t persistedRank = 0;
            uint32_t flux = 0;
            if (!row.IsObject() || !WorldSave::ReadUint32(row["id"], rawId) || !WorldSave::ReadUint32(row["xp"], xp) ||
                !WorldSave::ReadUint32(row["rank"], persistedRank) || !WorldSave::ReadUint32(row["flux"], flux) ||
                rawId == kInvalidPlayer || flux > kFluxWalletCap || persistedRank != RankForXP(xp) ||
                loaded.contains(static_cast<PlayerId>(rawId)))
            {
                m_persistenceBlocked = !migratingLegacy;
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] progression row %zu in %s is malformed; %s", i,
                                SavePaths::Utf8ForLog(source).c_str(),
                                migratingLegacy ? "legacy preserved" : "writes latched off");
                return false;
            }
            const auto id = static_cast<PlayerId>(rawId);
            Prog rec;
            rec.xp = xp;
            rec.flux = flux;
            rec.rank = static_cast<uint16_t>(persistedRank);
            loaded[id] = rec;
        }
        m_players = std::move(loaded);

        if (migratingLegacy)
        {
            m_dirty = true;
            if (!SaveNow())
            {
                m_persistenceBlocked = true;
                return false;
            }
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] progression loaded: %zu players from %s", m_players.size(),
                       SavePaths::Utf8ForLog(source).c_str());
        return true;
    }

    bool TFProgressionSystem::SaveNow()
    {
        if (m_persistenceBlocked || !m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded())
            return false;
        const ContinentDef& continent = m_ctx->data->GetContinent();
        const std::filesystem::path saveFile = SavePaths::ContinentFile("terrafront_state", continent.key);
        if (saveFile.empty())
            return false;

        Spark::Json::Value root;
        std::string detail;
        const WorldSave::ReadStatus read =
            WorldSave::ReadJson(saveFile, continent.key, continent.name, false, root, detail);
        if (read == WorldSave::ReadStatus::Missing)
            root = Spark::Json::Value::MakeObject();
        else if (read != WorldSave::ReadStatus::Loaded)
        {
            m_persistenceBlocked = true;
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "[TF] progression save refused because %s is unreadable/corrupt (%s)",
                            SavePaths::Utf8ForLog(saveFile).c_str(), detail.c_str());
            return false;
        }
        root["continentKey"] = Spark::Json::Value(continent.key);
        root["continent"] = Spark::Json::Value(continent.name);

        Spark::Json::Value plist = Spark::Json::Value::MakeArray();
        bool characterWritesOk = true;
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
                    characterWritesOk =
                        m_ctx->characters->PersistProgress(charId, rec.xp, rec.rank, rec.flux) && characterWritesOk;
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
        if (m_ctx->db)
            characterWritesOk = m_meta.PersistAllDirty(*m_ctx->db) && characterWritesOk;
        if (!characterWritesOk)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] progression save failed: character database flush failed");
            return false;
        }

        if (!WorldSave::WriteJson(saveFile, root, detail))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] progression save failed for %s (%s)",
                            SavePaths::Utf8ForLog(saveFile).c_str(), detail.c_str());
            return false;
        }

        m_dirty = false;
        m_sinceSave = 0.0f;
        ++m_saves;
        return true;
    }

} // namespace Terrafront
