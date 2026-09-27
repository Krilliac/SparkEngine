/**
 * @file MMOPersistenceSystem.cpp
 * @brief Database schema, prepared statements, and save/load implementation
 */

#include "MMOPersistenceSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_EDITOR
#include <imgui.h>
#endif

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>

namespace MMO
{

    using namespace Spark::Persistence;

    namespace
    {
        /// Upper bound on a stored inventory's slot count; larger values are corrupt.
        constexpr int kMaxInventorySlots = 512;

        constexpr PreparedStatementID Sid(MMOStmtId id)
        {
            return static_cast<PreparedStatementID>(id);
        }

        uint64_t GetTimestamp()
        {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count());
        }

        PreparedStatementParam MakeInt(int64_t v)
        {
            return PreparedStatementParam{QueryValue{v}};
        }

        PreparedStatementParam MakeDouble(double v)
        {
            return PreparedStatementParam{QueryValue{v}};
        }

        PreparedStatementParam MakeString(const std::string& v)
        {
            return PreparedStatementParam{QueryValue{v}};
        }

        // SQLiteConnection::Execute single-quotes string parameters (escaping ' as '')
        // before substitution, and the key-value store keeps that text verbatim.
        // Loads must invert the transformation to recover the original string.
        std::string UnquoteStoredString(std::string s)
        {
            if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'')
            {
                s = s.substr(1, s.size() - 2);
            }
            size_t pos = 0;
            while ((pos = s.find("''", pos)) != std::string::npos)
            {
                s.erase(pos, 1);
                ++pos;
            }
            return s;
        }

        /// Whole-string integer parse; nullopt on empty, trailing text or overflow.
        template <typename T> std::optional<T> ParseInteger(std::string_view text)
        {
            T value{};
            const char* last = text.data() + text.size();
            const auto [ptr, ec] = std::from_chars(text.data(), last, value);
            if (text.empty() || ec != std::errc() || ptr != last)
                return std::nullopt;
            return value;
        }

        std::optional<float> ParseFiniteFloat(const std::string& text)
        {
            try
            {
                size_t used = 0;
                const float value = std::stof(text, &used);
                if (used != text.size() || !std::isfinite(value))
                    return std::nullopt;
                return value;
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }
        }

        std::vector<std::string> Split(const std::string& text, char separator)
        {
            std::vector<std::string> parts;
            std::istringstream stream(text);
            std::string part;
            while (std::getline(stream, part, separator))
                parts.push_back(part);
            return parts;
        }

        /// First column of a single-row KV result as a string, or nullptr.
        const std::string* FirstString(const QueryResult& result)
        {
            if (!result.success || result.rows.empty() || result.rows[0].columns.empty())
                return nullptr;
            return std::get_if<std::string>(&result.rows[0].columns[0]);
        }

        /// Parse a stored character row into the identity/location/stat fields of @p out.
        bool ParseCharacterRecord(const std::string& stored, CharacterSaveData& out)
        {
            // UpdateCharacter stores the whole blob as one quoted string;
            // InsertCharacter quotes only the leading name token.
            const std::vector<std::string> tokens = Split(UnquoteStoredString(stored), '|');
            if (tokens.size() < 14)
                return false;
            try
            {
                out.name = UnquoteStoredString(tokens[0]);
                // Version 2 stores accountId after the name. Continue to
                // accept legacy 14-field records written by earlier builds.
                const size_t valueOffset = tokens.size() >= 15 ? 2 : 1;
                if (tokens.size() >= 15)
                    out.accountId = static_cast<uint32_t>(std::stoul(tokens[1]));
                out.level = std::stoi(tokens[valueOffset]);
                out.xp = std::stoi(tokens[valueOffset + 1]);
                out.areaId = static_cast<uint32_t>(std::stoul(tokens[valueOffset + 2]));
                out.posX = std::stof(tokens[valueOffset + 3]);
                out.posY = std::stof(tokens[valueOffset + 4]);
                out.posZ = std::stof(tokens[valueOffset + 5]);
                out.rotY = std::stof(tokens[valueOffset + 6]);
                out.health = std::stof(tokens[valueOffset + 7]);
                out.maxHealth = std::stof(tokens[valueOffset + 8]);
                out.mana = std::stof(tokens[valueOffset + 9]);
                out.maxMana = std::stof(tokens[valueOffset + 10]);
                out.playTime = std::stof(tokens[valueOffset + 11]);
                out.inventory.currency = std::stoi(tokens[valueOffset + 12]);
            }
            catch (const std::exception&)
            {
                return false;
            }
            return true;
        }

        /// "1|maxSlots|maxWeight|slotCount|slot:itemDefId:count,..." — every slot
        /// keeps its index, so empty gaps and the slot count survive a reload.
        std::string EncodeInventoryRecord(const InventoryData& inv)
        {
            std::string text = std::format("1|{}|{}|{}|", inv.maxSlots, inv.maxWeight, inv.slots.size());
            bool first = true;
            for (size_t i = 0; i < inv.slots.size(); ++i)
            {
                const ItemStack& slot = inv.slots[i];
                if (slot.IsEmpty())
                    continue;
                text += std::format("{}{}:{}:{}", first ? "" : ",", i, slot.itemDefId, slot.count);
                first = false;
            }
            return text;
        }

        bool ParseInventoryRecord(const std::string& text, InventoryData& out)
        {
            const std::vector<std::string> fields = Split(text, '|');
            if ((fields.size() != 4 && fields.size() != 5) || fields[0] != "1")
                return false;
            const auto maxSlots = ParseInteger<int>(fields[1]);
            const auto maxWeight = ParseFiniteFloat(fields[2]);
            const auto slotCount = ParseInteger<int>(fields[3]);
            if (!maxSlots || *maxSlots < 0 || *maxSlots > kMaxInventorySlots || !maxWeight || !slotCount ||
                *slotCount < 0 || *slotCount > kMaxInventorySlots)
                return false;

            out.maxSlots = *maxSlots;
            out.maxWeight = *maxWeight;
            out.slots.assign(static_cast<size_t>(*slotCount), ItemStack{});
            if (fields.size() == 4)
                return true;
            for (const std::string& entry : Split(fields[4], ','))
            {
                const std::vector<std::string> parts = Split(entry, ':');
                if (parts.size() != 3)
                    return false;
                const auto index = ParseInteger<int>(parts[0]);
                const auto itemDefId = ParseInteger<uint32_t>(parts[1]);
                const auto count = ParseInteger<int>(parts[2]);
                if (!index || *index < 0 || *index >= *slotCount || !itemDefId || *itemDefId == 0 || !count ||
                    *count <= 0)
                    return false;
                ItemStack& slot = out.slots[static_cast<size_t>(*index)];
                if (!slot.IsEmpty())
                    return false; // the same slot listed twice
                slot.itemDefId = *itemDefId;
                slot.count = *count;
            }
            return true;
        }

        /// Rebuild the bound parameters (after the character ID) that produced a
        /// stored key suffix: '_'-separated integers (slot, faction, dungeon and
        /// difficulty) or one string Execute() substituted as a quoted literal
        /// (achievement stat names).
        std::optional<std::vector<PreparedStatementParam>> KeySuffixParams(uint32_t charId, const std::string& suffix)
        {
            std::vector<PreparedStatementParam> params{MakeInt(charId)};
            if (suffix.size() >= 2 && suffix.front() == '\'' && suffix.back() == '\'')
            {
                params.push_back(MakeString(UnquoteStoredString(suffix)));
                return params;
            }
            for (const std::string& piece : Split(suffix, '_'))
            {
                const auto value = ParseInteger<int64_t>(piece);
                if (!value)
                    return std::nullopt;
                params.push_back(MakeInt(*value));
            }
            if (params.size() == 1)
                return std::nullopt;
            return params;
        }
    } // namespace

    // =========================================================================
    // Initialization
    // =========================================================================

    bool MMOPersistenceSystem::Initialize(Spark::IEngineContext* context, const std::string& dbPath)
    {
        if (m_initialized || m_db)
            Shutdown();

        m_context = context;
        m_db = std::make_unique<AsyncDatabasePool>();

        // One worker: every query already serializes on the pool's single shared
        // connection, and a single worker also runs queued saves in submission
        // order, so an older auto-save can never land on top of a newer one.
        if (!m_db->Open(dbPath, 1))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "Failed to open MMO database: %s", dbPath.c_str());
            Spark::SimpleConsole::GetInstance().LogError("[MMO] Failed to open database: " + dbPath);
            return false;
        }

        RegisterPreparedStatements();
        CreateSchema();
        SeedCharacterIdCounter();

        m_initialized = true;
        m_autoSaveTimer = m_autoSaveInterval;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "MMO persistence system initialized (db: %s)", dbPath.c_str());
        Spark::SimpleConsole::GetInstance().LogInfo("[MMO] Persistence system initialized (db: " + dbPath + ")");
        return true;
    }

    void MMOPersistenceSystem::Shutdown()
    {
        if (m_db)
        {
            // Close drains every queued save before the workers stop.
            m_db->Close();
            m_db.reset();
        }
        m_context = nullptr;
        m_autoSaveTimer = 0.0f;
        m_initialized = false;
        m_nextCharacterId = 1;
    }

    void MMOPersistenceSystem::Update(float dt)
    {
        if (!m_initialized || !m_db)
            return;

        // Process async callback results
        m_db->ProcessCallbacks();

        // Auto-save timer (module is responsible for triggering the actual save)
        if (dt > 0.0f)
            m_autoSaveTimer -= dt;
    }

    void MMOPersistenceSystem::RegisterPreparedStatements()
    {
        // Character table
        m_db->PrepareStatement(Sid(MMOStmtId::CreateCharacterTable),
                               "SET __schema_characters CREATE TABLE characters (id INTEGER PRIMARY KEY, "
                               "account_id INTEGER, name TEXT, level INTEGER, xp INTEGER, "
                               "area_id INTEGER, pos_x REAL, pos_y REAL, pos_z REAL, rot_y REAL, "
                               "health REAL, max_health REAL, mana REAL, max_mana REAL, "
                               "play_time REAL, currency INTEGER, "
                               "created_at INTEGER, last_login INTEGER, last_save INTEGER)");

        // Placeholders use the engine's zero-based "?N" syntax — the only form
        // SQLiteConnection::Execute substitutes (AsyncDatabase.cpp).
        m_db->PrepareStatement(Sid(MMOStmtId::InsertCharacter),
                               "SET character_?0 ?1|?2|1|0|1|0.0|1.0|0.0|0.0|100.0|100.0|50.0|50.0|0.0|0");

        // ?1 is the full pipe-delimited character blob built by the save paths.
        m_db->PrepareStatement(Sid(MMOStmtId::UpdateCharacter), "SET character_?0 ?1");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadCharacter), "GET character_?0");
        m_db->PrepareStatement(Sid(MMOStmtId::DeleteCharacter), "DELETE character_?0");
        m_db->PrepareStatement(Sid(MMOStmtId::ListCharacters), "KEYS character_");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadNextCharacterId), "GET meta_next_character_id");
        m_db->PrepareStatement(Sid(MMOStmtId::SaveNextCharacterId), "SET meta_next_character_id ?0");

        // Inventory (?1 is the record built by EncodeInventoryRecord)
        m_db->PrepareStatement(Sid(MMOStmtId::SaveInventory), "SET inventory_?0 ?1");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadInventory), "GET inventory_?0");
        m_db->PrepareStatement(Sid(MMOStmtId::DeleteInventory), "DELETE inventory_?0");
        m_db->PrepareStatement(Sid(MMOStmtId::ListLegacyInventorySlots), "KEYS inv_?0_");
        m_db->PrepareStatement(Sid(MMOStmtId::DeleteLegacyInventorySlot), "DELETE inv_?0_?1");
        m_db->PrepareStatement(Sid(MMOStmtId::SaveCurrency), "SET currency_?0 ?1");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadCurrency), "GET currency_?0");
        m_db->PrepareStatement(Sid(MMOStmtId::DeleteCurrency), "DELETE currency_?0");

        // Reputation
        m_db->PrepareStatement(Sid(MMOStmtId::SaveReputation), "SET rep_?0_?1 ?2");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadReputation), "KEYS rep_?0_");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadReputationValue), "GET rep_?0_?1");
        m_db->PrepareStatement(Sid(MMOStmtId::DeleteReputation), "DELETE rep_?0_?1");

        // Achievements
        m_db->PrepareStatement(Sid(MMOStmtId::SaveAchievement), "SET ach_?0_?1 1");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadAchievements), "KEYS ach_?0_");
        m_db->PrepareStatement(Sid(MMOStmtId::DeleteAchievement), "DELETE ach_?0_?1");
        m_db->PrepareStatement(Sid(MMOStmtId::SaveAchievementStat), "SET achstat_?0_?1 ?2");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadAchievementStats), "KEYS achstat_?0_");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadAchievementStatValue), "GET achstat_?0_?1");
        m_db->PrepareStatement(Sid(MMOStmtId::DeleteAchievementStat), "DELETE achstat_?0_?1");

        // Crafting
        m_db->PrepareStatement(Sid(MMOStmtId::SaveCraftingSkill), "SET craft_?0_?1 ?2|?3");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadCraftingSkills), "KEYS craft_?0_");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadCraftingSkillValue), "GET craft_?0_?1");
        m_db->PrepareStatement(Sid(MMOStmtId::DeleteCraftingSkill), "DELETE craft_?0_?1");
        m_db->PrepareStatement(Sid(MMOStmtId::SaveKnownRecipe), "SET recipe_?0_?1 1");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadKnownRecipes), "KEYS recipe_?0_");
        m_db->PrepareStatement(Sid(MMOStmtId::DeleteKnownRecipe), "DELETE recipe_?0_?1");

        // Guilds (?1 is the full pipe-delimited guild blob built by SaveWorldAsync)
        m_db->PrepareStatement(Sid(MMOStmtId::SaveGuild), "SET guild_?0 ?1");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadGuilds), "KEYS guild_");
        m_db->PrepareStatement(Sid(MMOStmtId::SaveGuildMember), "SET gm_?0_?1 ?2|?3");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadGuildMembers), "KEYS gm_?0_");
        m_db->PrepareStatement(Sid(MMOStmtId::DeleteGuildMember), "DELETE gm_?0_?1");

        // Lockouts
        m_db->PrepareStatement(Sid(MMOStmtId::SaveLockout), "SET lockout_?0_?1_?2 ?3");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadLockouts), "KEYS lockout_?0_");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadLockoutValue), "GET lockout_?0_?1_?2");
        m_db->PrepareStatement(Sid(MMOStmtId::DeleteLockout), "DELETE lockout_?0_?1_?2");

        // Boss kills
        m_db->PrepareStatement(Sid(MMOStmtId::SaveBossKill), "SET bosskill_?0_?1 ?2");
        m_db->PrepareStatement(Sid(MMOStmtId::LoadBossKills), "KEYS bosskill_");
    }

    void MMOPersistenceSystem::CreateSchema()
    {
        // Execute schema creation (the key-value store doesn't need DDL,
        // but this ensures the schema marker exists for future migration checks)
        m_db->SyncQuery(Sid(MMOStmtId::CreateCharacterTable));
    }

    void MMOPersistenceSystem::SeedCharacterIdCounter()
    {
        // The stored counter covers IDs whose characters were later deleted; the
        // key scan covers stores written before the counter existed (including
        // the timestamp-derived IDs earlier builds generated).
        uint64_t next = 1;
        const QueryResult counter = m_db->SyncQuery(Sid(MMOStmtId::LoadNextCharacterId));
        if (const std::string* stored = FirstString(counter))
        {
            if (const auto value = ParseInteger<uint64_t>(*stored))
                next = std::max(next, *value);
            else
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: corrupt character ID counter '%s'",
                                stored->c_str());
        }

        const QueryResult keys = m_db->SyncQuery(Sid(MMOStmtId::ListCharacters));
        for (const auto& row : keys.rows)
        {
            const std::string* key = row.columns.empty() ? nullptr : std::get_if<std::string>(&row.columns[0]);
            if (!key || !key->starts_with("character_"))
                continue;
            if (const auto id = ParseInteger<uint32_t>(std::string_view(*key).substr(10)))
                next = std::max<uint64_t>(next, uint64_t{*id} + 1);
        }
        m_nextCharacterId = next;
    }

    // =========================================================================
    // Character CRUD
    // =========================================================================

    uint32_t MMOPersistenceSystem::AllocateCharacterId()
    {
        if (!m_initialized)
            return 0;
        if (m_nextCharacterId > std::numeric_limits<uint32_t>::max())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: character ID space exhausted");
            return 0;
        }

        // Persist the advanced counter before handing the ID out, so a crash
        // after this point can never reissue it to a second character.
        const uint64_t next = m_nextCharacterId + 1;
        const QueryResult saved =
            m_db->SyncQuery(Sid(MMOStmtId::SaveNextCharacterId), {MakeInt(static_cast<int64_t>(next))});
        if (!saved.success)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: could not persist character ID counter: %s",
                            saved.errorMessage.c_str());
            return 0;
        }
        const auto id = static_cast<uint32_t>(m_nextCharacterId);
        m_nextCharacterId = next;
        return id;
    }

    uint32_t MMOPersistenceSystem::CreateCharacter(const std::string& name, uint32_t accountId)
    {
        const uint32_t charId = AllocateCharacterId();
        if (charId == 0)
            return 0;

        auto result =
            m_db->SyncQuery(Sid(MMOStmtId::InsertCharacter), {MakeInt(charId), MakeString(name), MakeInt(accountId)});

        if (result.success)
        {
            Spark::SimpleConsole::GetInstance().LogInfo("[MMO] Character created: " + name + " (ID " +
                                                        std::to_string(charId) + ")");
        }
        return result.success ? charId : 0;
    }

    bool MMOPersistenceSystem::LoadCharacter(uint32_t characterId, CharacterSaveData& outData)
    {
        if (!m_initialized)
            return false;

        const QueryResult row = m_db->SyncQuery(Sid(MMOStmtId::LoadCharacter), {MakeInt(characterId)});
        const std::string* stored = FirstString(row);
        if (!stored)
            return false;

        outData.characterId = characterId;
        if (!ParseCharacterRecord(*stored, outData))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: corrupt or incomplete character data for ID %u",
                            characterId);
            return false;
        }

        // A corrupt inventory fails the load: entering the world with an empty
        // bag would let the next save overwrite the stored items.
        if (!LoadInventory(characterId, outData.inventory))
            return false;
        LoadReputationState(characterId, outData.reputationState);
        LoadAchievementState(characterId, outData.achievementState);
        LoadCraftingState(characterId, outData.craftingState);
        LoadLockouts(characterId, outData.dungeonState);

        outData.lastLogin = GetTimestamp();
        m_totalLoads++;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "Character loaded: %s (Lv%d, ID %u)", outData.name.c_str(),
                       outData.level, characterId);
        Spark::SimpleConsole::GetInstance().LogInfo("[MMO] Character loaded: " + outData.name + " (Lv" +
                                                    std::to_string(outData.level) + ")");
        return true;
    }

    MMOPersistenceSystem::Transaction MMOPersistenceSystem::BuildCharacterSave(const CharacterSaveData& data) const
    {
        std::ostringstream ss;
        ss << data.name << "|" << data.accountId << "|" << data.level << "|" << data.xp << "|" << data.areaId << "|"
           << data.posX << "|" << data.posY << "|" << data.posZ << "|" << data.rotY << "|" << data.health << "|"
           << data.maxHealth << "|" << data.mana << "|" << data.maxMana << "|" << data.playTime << "|"
           << data.inventory.currency;

        Transaction tx;
        tx.Append(Sid(MMOStmtId::UpdateCharacter), {MakeInt(data.characterId), MakeString(ss.str())});
        SaveInventory(tx, data.characterId, data.inventory);
        SaveReputationState(tx, data.characterId, data.reputationState);
        SaveAchievementState(tx, data.characterId, data.achievementState);
        SaveCraftingState(tx, data.characterId, data.craftingState);
        SaveLockouts(tx, data.characterId, data.dungeonState);
        return tx;
    }

    void MMOPersistenceSystem::SaveCharacterAsync(const CharacterSaveData& data)
    {
        if (!m_initialized)
            return;

        // One transaction per save: the character row and its subsystem records
        // commit together (one flush), and the single worker applies saves in order.
        m_db->AsyncTransaction(BuildCharacterSave(data));

        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Async save character: %s (ID %u)", data.name.c_str(),
                        data.characterId);
        m_totalSaves++;
        m_lastSaveTime = data.playTime;
    }

    bool MMOPersistenceSystem::SaveCharacterSync(const CharacterSaveData& data)
    {
        if (!m_initialized)
            return false;

        // Queued behind any pending auto-save, so this state is the one that lands last.
        const QueryResult result = m_db->AsyncTransaction(BuildCharacterSave(data)).get();
        if (!result.success)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: save failed for character %u: %s",
                            data.characterId, result.errorMessage.c_str());
            return false;
        }
        m_totalSaves++;
        return true;
    }

    void MMOPersistenceSystem::AppendKeyDeletes(Transaction& tx, MMOStmtId listStmt, MMOStmtId deleteStmt,
                                                uint32_t charId, const std::string& family)
    {
        const std::string prefix = family + std::to_string(charId) + "_";
        const QueryResult keys = m_db->SyncQuery(Sid(listStmt), {MakeInt(charId)});
        for (const auto& row : keys.rows)
        {
            const std::string* key = row.columns.empty() ? nullptr : std::get_if<std::string>(&row.columns[0]);
            if (!key || !key->starts_with(prefix))
                continue;
            auto params = KeySuffixParams(charId, key->substr(prefix.size()));
            if (!params)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Game, "MMOPersistence: cannot address stored key '%s' for delete",
                               key->c_str());
                continue;
            }
            tx.Append(Sid(deleteStmt), std::move(*params));
        }
    }

    bool MMOPersistenceSystem::DeleteCharacter(uint32_t characterId)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Deleting character from persistence: ID %u", characterId);
        if (!m_initialized)
            return false;

        // A read queued behind every pending save: once it completes, no earlier
        // save can still create keys after the key scan below.
        m_db->AsyncQuery(Sid(MMOStmtId::LoadCharacter), {MakeInt(characterId)}).wait();

        Transaction tx;
        tx.Append(Sid(MMOStmtId::DeleteCharacter), {MakeInt(characterId)});
        tx.Append(Sid(MMOStmtId::DeleteInventory), {MakeInt(characterId)});
        tx.Append(Sid(MMOStmtId::DeleteCurrency), {MakeInt(characterId)});
        AppendKeyDeletes(tx, MMOStmtId::ListLegacyInventorySlots, MMOStmtId::DeleteLegacyInventorySlot, characterId,
                         "inv_");
        AppendKeyDeletes(tx, MMOStmtId::LoadReputation, MMOStmtId::DeleteReputation, characterId, "rep_");
        AppendKeyDeletes(tx, MMOStmtId::LoadAchievements, MMOStmtId::DeleteAchievement, characterId, "ach_");
        AppendKeyDeletes(tx, MMOStmtId::LoadAchievementStats, MMOStmtId::DeleteAchievementStat, characterId,
                         "achstat_");
        AppendKeyDeletes(tx, MMOStmtId::LoadCraftingSkills, MMOStmtId::DeleteCraftingSkill, characterId, "craft_");
        AppendKeyDeletes(tx, MMOStmtId::LoadKnownRecipes, MMOStmtId::DeleteKnownRecipe, characterId, "recipe_");
        AppendKeyDeletes(tx, MMOStmtId::LoadLockouts, MMOStmtId::DeleteLockout, characterId, "lockout_");
        return m_db->AsyncTransaction(std::move(tx)).get().success;
    }

    std::vector<std::pair<uint32_t, std::string>> MMOPersistenceSystem::ListCharacters(uint32_t accountId)
    {
        std::vector<std::pair<uint32_t, std::string>> result;
        if (!m_initialized || accountId == 0)
            return result;

        const QueryResult keys = m_db->SyncQuery(Sid(MMOStmtId::ListCharacters));
        for (const auto& row : keys.rows)
        {
            const std::string* key = row.columns.empty() ? nullptr : std::get_if<std::string>(&row.columns[0]);
            if (!key || !key->starts_with("character_"))
                continue;
            const auto charId = ParseInteger<uint32_t>(std::string_view(*key).substr(10));
            if (!charId)
                continue;
            const QueryResult stored = m_db->SyncQuery(Sid(MMOStmtId::LoadCharacter), {MakeInt(*charId)});
            const std::string* text = FirstString(stored);
            CharacterSaveData record;
            if (text && ParseCharacterRecord(*text, record) && record.accountId == accountId)
                result.emplace_back(*charId, record.name);
        }
        return result;
    }

    // =========================================================================
    // Subsystem Save/Load Helpers
    // =========================================================================

    void MMOPersistenceSystem::SaveInventory(Transaction& tx, uint32_t charId, const InventoryData& inv) const
    {
        tx.Append(Sid(MMOStmtId::SaveInventory), {MakeInt(charId), MakeString(EncodeInventoryRecord(inv))});
        tx.Append(Sid(MMOStmtId::SaveCurrency), {MakeInt(charId), MakeInt(inv.currency)});
    }

    bool MMOPersistenceSystem::LoadInventory(uint32_t charId, InventoryData& inv)
    {
        const QueryResult record = m_db->SyncQuery(Sid(MMOStmtId::LoadInventory), {MakeInt(charId)});
        if (!record.success)
            return false;
        if (!record.rows.empty())
        {
            const std::string* text = FirstString(record);
            InventoryData parsed;
            if (!text || !ParseInventoryRecord(*text, parsed))
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: corrupt inventory record for character %u",
                                charId);
                return false;
            }
            parsed.currency = inv.currency;
            inv = std::move(parsed);
        }

        const QueryResult currencyRow = m_db->SyncQuery(Sid(MMOStmtId::LoadCurrency), {MakeInt(charId)});
        if (const std::string* currency = FirstString(currencyRow))
        {
            if (const auto value = ParseInteger<int>(*currency))
                inv.currency = *value;
            else
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: corrupt currency for character %u", charId);
        }
        return true;
    }

    void MMOPersistenceSystem::SaveReputationState(Transaction& tx, uint32_t charId, const ReputationState& state) const
    {
        for (const auto& [factionId, standing] : state.standings)
        {
            tx.Append(Sid(MMOStmtId::SaveReputation),
                      {MakeInt(charId), MakeInt(factionId), MakeInt(standing.reputation)});
        }
    }

    void MMOPersistenceSystem::LoadReputationState(uint32_t charId, ReputationState& state)
    {
        auto result = m_db->SyncQuery(Sid(MMOStmtId::LoadReputation), {MakeInt(charId)});
        if (result.success)
        {
            for (const auto& row : result.rows)
            {
                if (!row.columns.empty() && std::holds_alternative<std::string>(row.columns[0]))
                {
                    const auto& key = std::get<std::string>(row.columns[0]);
                    // Parse "rep_<charId>_<factionId>" → factionId
                    auto prefix = "rep_" + std::to_string(charId) + "_";
                    if (key.starts_with(prefix))
                    {
                        try
                        {
                            uint32_t factionId = static_cast<uint32_t>(std::stoul(key.substr(prefix.size())));
                            auto valResult = m_db->SyncQuery(Sid(MMOStmtId::LoadReputationValue),
                                                             {MakeInt(charId), MakeInt(factionId)});
                            if (valResult.success && !valResult.rows.empty())
                            {
                                FactionStanding standing;
                                standing.factionId = factionId;
                                const auto& val = valResult.rows[0].columns[0];
                                if (std::holds_alternative<int64_t>(val))
                                    standing.reputation = static_cast<int>(std::get<int64_t>(val));
                                else if (std::holds_alternative<std::string>(val))
                                    standing.reputation = std::stoi(std::get<std::string>(val));
                                standing.tier = FactionStanding::GetTierForValue(standing.reputation);
                                state.standings[factionId] = standing;
                            }
                        }
                        catch (const std::exception&)
                        {
                            continue;
                        }
                    }
                }
            }
        }
    }

    void MMOPersistenceSystem::SaveAchievementState(Transaction& tx, uint32_t charId,
                                                    const AchievementState& state) const
    {
        // Save completed achievement IDs
        for (uint32_t achId : state.completedIds)
        {
            tx.Append(Sid(MMOStmtId::SaveAchievement), {MakeInt(charId), MakeInt(achId)});
        }

        // Save stat counters
        for (const auto& [key, val] : state.stats)
        {
            tx.Append(Sid(MMOStmtId::SaveAchievementStat), {MakeInt(charId), MakeString(key), MakeInt(val)});
        }
    }

    void MMOPersistenceSystem::LoadAchievementState(uint32_t charId, AchievementState& state)
    {
        // Load completed achievements
        auto achResult = m_db->SyncQuery(Sid(MMOStmtId::LoadAchievements), {MakeInt(charId)});
        if (achResult.success)
        {
            for (const auto& row : achResult.rows)
            {
                if (!row.columns.empty() && std::holds_alternative<std::string>(row.columns[0]))
                {
                    const auto& key = std::get<std::string>(row.columns[0]);
                    auto prefix = "ach_" + std::to_string(charId) + "_";
                    if (key.starts_with(prefix))
                    {
                        try
                        {
                            uint32_t achId = static_cast<uint32_t>(std::stoul(key.substr(prefix.size())));
                            state.completedIds.insert(achId);
                        }
                        catch (const std::exception&)
                        {
                            continue;
                        }
                    }
                }
            }
        }

        // Load stat counters
        auto statResult = m_db->SyncQuery(Sid(MMOStmtId::LoadAchievementStats), {MakeInt(charId)});
        if (statResult.success)
        {
            for (const auto& row : statResult.rows)
            {
                if (!row.columns.empty() && std::holds_alternative<std::string>(row.columns[0]))
                {
                    const auto& key = std::get<std::string>(row.columns[0]);
                    auto prefix = "achstat_" + std::to_string(charId) + "_";
                    if (key.starts_with(prefix))
                    {
                        // The stored key carries the quotes MakeString added on save;
                        // strip them so the map key round-trips, then re-quote via
                        // MakeString for the lookup to match the stored key.
                        std::string statKey = UnquoteStoredString(key.substr(prefix.size()));
                        auto valResult = m_db->SyncQuery(Sid(MMOStmtId::LoadAchievementStatValue),
                                                         {MakeInt(charId), MakeString(statKey)});
                        if (valResult.success && !valResult.rows.empty())
                        {
                            const auto& val = valResult.rows[0].columns[0];
                            if (std::holds_alternative<int64_t>(val))
                                state.stats[statKey] = static_cast<int>(std::get<int64_t>(val));
                            else if (std::holds_alternative<std::string>(val))
                            {
                                try
                                {
                                    state.stats[statKey] = std::stoi(std::get<std::string>(val));
                                }
                                catch (const std::exception&)
                                {
                                    state.stats[statKey] = 0;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    void MMOPersistenceSystem::SaveCraftingState(Transaction& tx, uint32_t charId, const CraftingState& state) const
    {
        // Save skill levels
        for (const auto& [key, skill] : state.skills)
        {
            tx.Append(Sid(MMOStmtId::SaveCraftingSkill),
                      {MakeInt(charId), MakeInt(key), MakeInt(skill.level), MakeInt(skill.currentXP)});
        }

        // Save known recipes
        for (uint32_t recipeId : state.knownRecipes)
        {
            tx.Append(Sid(MMOStmtId::SaveKnownRecipe), {MakeInt(charId), MakeInt(recipeId)});
        }
    }

    void MMOPersistenceSystem::LoadCraftingState(uint32_t charId, CraftingState& state)
    {
        // Load skills: key "craft_<charId>_<discipline>", value "level|xp"
        auto skillResult = m_db->SyncQuery(Sid(MMOStmtId::LoadCraftingSkills), {MakeInt(charId)});
        if (skillResult.success)
        {
            const std::string prefix = "craft_" + std::to_string(charId) + "_";
            for (const auto& row : skillResult.rows)
            {
                const std::string* key = row.columns.empty() ? nullptr : std::get_if<std::string>(&row.columns[0]);
                if (!key || !key->starts_with(prefix))
                    continue;
                const auto discKey = ParseInteger<int>(std::string_view(*key).substr(prefix.size()));
                if (!discKey)
                    continue;
                const QueryResult value =
                    m_db->SyncQuery(Sid(MMOStmtId::LoadCraftingSkillValue), {MakeInt(charId), MakeInt(*discKey)});
                const std::string* text = FirstString(value);
                const std::vector<std::string> fields = text ? Split(*text, '|') : std::vector<std::string>{};
                const auto level = fields.size() == 2 ? ParseInteger<int>(fields[0]) : std::nullopt;
                const auto xp = fields.size() == 2 ? ParseInteger<int>(fields[1]) : std::nullopt;
                if (!level || *level < 1 || !xp || *xp < 0)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: corrupt crafting skill '%s'",
                                    key->c_str());
                    continue;
                }
                CraftingSkill skill;
                skill.discipline = static_cast<CraftingDiscipline>(*discKey);
                skill.level = *level;
                skill.currentXP = *xp;
                state.skills[*discKey] = skill;
            }
        }

        // Load known recipes
        auto recipeResult = m_db->SyncQuery(Sid(MMOStmtId::LoadKnownRecipes), {MakeInt(charId)});
        if (recipeResult.success)
        {
            for (const auto& row : recipeResult.rows)
            {
                if (!row.columns.empty() && std::holds_alternative<std::string>(row.columns[0]))
                {
                    const auto& key = std::get<std::string>(row.columns[0]);
                    auto prefix = "recipe_" + std::to_string(charId) + "_";
                    if (key.starts_with(prefix))
                    {
                        try
                        {
                            uint32_t recipeId = static_cast<uint32_t>(std::stoul(key.substr(prefix.size())));
                            state.knownRecipes.push_back(recipeId);
                        }
                        catch (const std::exception&)
                        {
                            continue;
                        }
                    }
                }
            }
        }
    }

    void MMOPersistenceSystem::SaveLockouts(Transaction& tx, uint32_t charId, const DungeonPlayerState& state) const
    {
        for (const auto& lo : state.lockouts)
        {
            tx.Append(Sid(MMOStmtId::SaveLockout),
                      {MakeInt(charId), MakeInt(lo.dungeonDefId), MakeInt(static_cast<int64_t>(lo.difficulty)),
                       MakeDouble(static_cast<double>(lo.remaining))});
        }
    }

    void MMOPersistenceSystem::LoadLockouts(uint32_t charId, DungeonPlayerState& state)
    {
        // Key "lockout_<charId>_<dungeonDefId>_<difficulty>", value = seconds remaining
        auto result = m_db->SyncQuery(Sid(MMOStmtId::LoadLockouts), {MakeInt(charId)});
        if (!result.success)
            return;

        state.lockouts.clear();
        const std::string prefix = "lockout_" + std::to_string(charId) + "_";
        for (const auto& row : result.rows)
        {
            const std::string* key = row.columns.empty() ? nullptr : std::get_if<std::string>(&row.columns[0]);
            if (!key || !key->starts_with(prefix))
                continue;
            const std::vector<std::string> parts = Split(key->substr(prefix.size()), '_');
            const auto dungeonDefId = parts.size() == 2 ? ParseInteger<uint32_t>(parts[0]) : std::nullopt;
            const auto difficulty = parts.size() == 2 ? ParseInteger<int>(parts[1]) : std::nullopt;
            if (!dungeonDefId || !difficulty)
                continue;
            const QueryResult value = m_db->SyncQuery(Sid(MMOStmtId::LoadLockoutValue),
                                                      {MakeInt(charId), MakeInt(*dungeonDefId), MakeInt(*difficulty)});
            const std::string* text = FirstString(value);
            const auto remaining = text ? ParseFiniteFloat(*text) : std::nullopt;
            if (!remaining)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: corrupt lockout '%s'", key->c_str());
                continue;
            }
            LootLockout lo;
            lo.dungeonDefId = *dungeonDefId;
            lo.difficulty = static_cast<DungeonDifficulty>(*difficulty);
            lo.remaining = *remaining;
            state.lockouts.push_back(lo);
        }
    }

    // =========================================================================
    // World Persistence
    // =========================================================================

    void MMOPersistenceSystem::SaveWorldAsync(const WorldSaveData& data)
    {
        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Saving world data: %zu guilds, %zu boss kills", data.guilds.size(),
                        data.bossKillHistory.size());
        if (!m_initialized)
            return;

        // Save guilds
        for (const auto& guild : data.guilds)
        {
            std::ostringstream guildStr;
            guildStr << guild.name << "|" << guild.tag << "|" << guild.leaderId << "|" << guild.bankCurrency << "|"
                     << guild.motd;

            m_db->AsyncQuery(Sid(MMOStmtId::SaveGuild), {MakeInt(guild.id), MakeString(guildStr.str())});

            // Save members
            for (const auto& member : guild.members)
            {
                m_db->AsyncQuery(Sid(MMOStmtId::SaveGuildMember),
                                 {MakeInt(guild.id), MakeInt(member.playerId), MakeString(member.name),
                                  MakeInt(static_cast<int64_t>(member.rank))});
            }
        }

        // Save boss kill history
        for (const auto& kill : data.bossKillHistory)
        {
            m_db->AsyncQuery(Sid(MMOStmtId::SaveBossKill),
                             {MakeInt(kill.bossDefId), MakeInt(static_cast<int64_t>(kill.killTime)),
                              MakeInt(kill.participantCount)});
        }
    }

    bool MMOPersistenceSystem::LoadWorld(WorldSaveData& outData)
    {
        if (!m_initialized)
            return false;

        // Load guilds
        auto guildResult = m_db->SyncQuery(Sid(MMOStmtId::LoadGuilds));
        if (guildResult.success)
        {
            Spark::SimpleConsole::GetInstance().LogInfo("[MMO] Loaded " + std::to_string(guildResult.rows.size()) +
                                                        " guild records from database");
        }

        // Load boss kill history
        auto bossResult = m_db->SyncQuery(Sid(MMOStmtId::LoadBossKills));
        if (bossResult.success)
        {
            Spark::SimpleConsole::GetInstance().LogInfo("[MMO] Loaded " + std::to_string(bossResult.rows.size()) +
                                                        " boss kill records from database");
        }

        return true;
    }

    // =========================================================================
    // Status & Debug
    // =========================================================================

    int MMOPersistenceSystem::GetPendingWrites() const
    {
        return m_db ? m_db->GetPendingQueryCount() : 0;
    }

    std::string MMOPersistenceSystem::GetStatusString() const
    {
        std::ostringstream ss;
        ss << "=== MMO Persistence ===\n";
        ss << "Database: " << (m_initialized ? "Connected" : "Offline") << "\n";
        ss << "Total saves: " << m_totalSaves << "\n";
        ss << "Total loads: " << m_totalLoads << "\n";
        ss << "Pending writes: " << GetPendingWrites() << "\n";
        ss << "Auto-save interval: " << m_autoSaveInterval << "s\n";
        ss << "Next auto-save in: " << static_cast<int>(m_autoSaveTimer) << "s\n";
        return ss.str();
    }

    void MMOPersistenceSystem::RenderDebugUI()
    {
#ifdef ENABLE_EDITOR
        if (ImGui::TreeNode("MMO Persistence System"))
        {
            ImGui::Text("Status: %s", m_initialized ? "Connected" : "Offline");
            ImGui::Text("Total Saves: %d | Total Loads: %d", m_totalSaves, m_totalLoads);
            ImGui::Text("Pending Writes: %d", GetPendingWrites());
            ImGui::Text("Auto-save: %.0fs interval (%.0fs until next)", m_autoSaveInterval, m_autoSaveTimer);
            if (m_db)
                ImGui::Text("DB Pool Size: %d", m_db->GetPoolSize());
            ImGui::TreePop();
        }
#endif
    }

} // namespace MMO
