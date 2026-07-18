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

#include <chrono>
#include <sstream>

namespace MMO
{

    using namespace Spark::Persistence;

    static uint64_t GetTimestamp()
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    static PreparedStatementParam MakeInt(int64_t v)
    {
        return PreparedStatementParam{QueryValue{v}};
    }

    static PreparedStatementParam MakeDouble(double v)
    {
        return PreparedStatementParam{QueryValue{v}};
    }

    static PreparedStatementParam MakeString(const std::string& v)
    {
        return PreparedStatementParam{QueryValue{v}};
    }

    // SQLiteConnection::Execute single-quotes string parameters (escaping ' as '')
    // before substitution, and the key-value store keeps that text verbatim.
    // Loads must invert the transformation to recover the original string.
    static std::string UnquoteStoredString(std::string s)
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

    // =========================================================================
    // Initialization
    // =========================================================================

    bool MMOPersistenceSystem::Initialize(Spark::IEngineContext* context, const std::string& dbPath)
    {
        m_context = context;
        m_db = std::make_unique<AsyncDatabasePool>();

        if (!m_db->Open(dbPath, 2))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "Failed to open MMO database: %s", dbPath.c_str());
            Spark::SimpleConsole::GetInstance().LogError("[MMO] Failed to open database: " + dbPath);
            return false;
        }

        RegisterPreparedStatements();
        CreateSchema();

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
            m_db->Close();
            m_db.reset();
        }
        m_initialized = false;
    }

    void MMOPersistenceSystem::Update(float dt)
    {
        if (!m_initialized || !m_db)
            return;

        // Process async callback results
        m_db->ProcessCallbacks();

        // Auto-save timer (module is responsible for triggering the actual save)
        m_autoSaveTimer -= dt;
    }

    void MMOPersistenceSystem::RegisterPreparedStatements()
    {
        auto sid = [](MMOStmtId id) { return static_cast<PreparedStatementID>(id); };

        // Character table
        m_db->PrepareStatement(sid(MMOStmtId::CreateCharacterTable),
                               "SET __schema_characters CREATE TABLE characters (id INTEGER PRIMARY KEY, "
                               "account_id INTEGER, name TEXT, level INTEGER, xp INTEGER, "
                               "area_id INTEGER, pos_x REAL, pos_y REAL, pos_z REAL, rot_y REAL, "
                               "health REAL, max_health REAL, mana REAL, max_mana REAL, "
                               "play_time REAL, currency INTEGER, "
                               "created_at INTEGER, last_login INTEGER, last_save INTEGER)");

        // Placeholders use the engine's zero-based "?N" syntax — the only form
        // SQLiteConnection::Execute substitutes (AsyncDatabase.cpp).
        m_db->PrepareStatement(sid(MMOStmtId::InsertCharacter),
                               "SET character_?0 ?1|1|0|1|0.0|1.0|0.0|0.0|100.0|100.0|50.0|50.0|0.0|0");

        // ?1 is the full pipe-delimited character blob built by the save paths.
        m_db->PrepareStatement(sid(MMOStmtId::UpdateCharacter), "SET character_?0 ?1");

        m_db->PrepareStatement(sid(MMOStmtId::LoadCharacter), "GET character_?0");

        m_db->PrepareStatement(sid(MMOStmtId::DeleteCharacter), "DELETE character_?0");

        m_db->PrepareStatement(sid(MMOStmtId::ListCharacters), "KEYS character_");

        // Inventory
        m_db->PrepareStatement(sid(MMOStmtId::SaveInventorySlot), "SET inv_?0_?1 ?2|?3");

        m_db->PrepareStatement(sid(MMOStmtId::LoadInventory), "KEYS inv_?0_");

        m_db->PrepareStatement(sid(MMOStmtId::ClearInventory), "DELETE inv_?0_*");

        m_db->PrepareStatement(sid(MMOStmtId::SaveCurrency), "SET currency_?0 ?1");

        m_db->PrepareStatement(sid(MMOStmtId::LoadCurrency), "GET currency_?0");

        // Reputation
        m_db->PrepareStatement(sid(MMOStmtId::SaveReputation), "SET rep_?0_?1 ?2");

        m_db->PrepareStatement(sid(MMOStmtId::LoadReputation), "KEYS rep_?0_");

        m_db->PrepareStatement(sid(MMOStmtId::LoadReputationValue), "GET rep_?0_?1");

        m_db->PrepareStatement(sid(MMOStmtId::ClearReputation), "DELETE rep_?0_*");

        // Achievements
        m_db->PrepareStatement(sid(MMOStmtId::SaveAchievement), "SET ach_?0_?1 1");

        m_db->PrepareStatement(sid(MMOStmtId::LoadAchievements), "KEYS ach_?0_");

        m_db->PrepareStatement(sid(MMOStmtId::SaveAchievementStat), "SET achstat_?0_?1 ?2");

        m_db->PrepareStatement(sid(MMOStmtId::LoadAchievementStats), "KEYS achstat_?0_");

        m_db->PrepareStatement(sid(MMOStmtId::LoadAchievementStatValue), "GET achstat_?0_?1");

        // Crafting
        m_db->PrepareStatement(sid(MMOStmtId::SaveCraftingSkill), "SET craft_?0_?1 ?2|?3");

        m_db->PrepareStatement(sid(MMOStmtId::LoadCraftingSkills), "KEYS craft_?0_");

        m_db->PrepareStatement(sid(MMOStmtId::SaveKnownRecipe), "SET recipe_?0_?1 1");

        m_db->PrepareStatement(sid(MMOStmtId::LoadKnownRecipes), "KEYS recipe_?0_");

        // Guilds (?1 is the full pipe-delimited guild blob built by SaveWorldAsync)
        m_db->PrepareStatement(sid(MMOStmtId::SaveGuild), "SET guild_?0 ?1");

        m_db->PrepareStatement(sid(MMOStmtId::LoadGuilds), "KEYS guild_");

        m_db->PrepareStatement(sid(MMOStmtId::SaveGuildMember), "SET gm_?0_?1 ?2|?3");

        m_db->PrepareStatement(sid(MMOStmtId::LoadGuildMembers), "KEYS gm_?0_");

        m_db->PrepareStatement(sid(MMOStmtId::DeleteGuildMember), "DELETE gm_?0_?1");

        // Lockouts
        m_db->PrepareStatement(sid(MMOStmtId::SaveLockout), "SET lockout_?0_?1_?2 ?3");

        m_db->PrepareStatement(sid(MMOStmtId::LoadLockouts), "KEYS lockout_?0_");

        m_db->PrepareStatement(sid(MMOStmtId::ClearExpiredLockouts), "DELETE lockout_?0_expired");

        // Boss kills
        m_db->PrepareStatement(sid(MMOStmtId::SaveBossKill), "SET bosskill_?0_?1 ?2");

        m_db->PrepareStatement(sid(MMOStmtId::LoadBossKills), "KEYS bosskill_");
    }

    void MMOPersistenceSystem::CreateSchema()
    {
        auto sid = [](MMOStmtId id) { return static_cast<PreparedStatementID>(id); };

        // Execute schema creation (the key-value store doesn't need DDL,
        // but this ensures the schema marker exists for future migration checks)
        m_db->SyncQuery(sid(MMOStmtId::CreateCharacterTable));
    }

    // =========================================================================
    // Character CRUD
    // =========================================================================

    uint32_t MMOPersistenceSystem::CreateCharacter(const std::string& name, uint32_t accountId)
    {
        if (!m_initialized)
            return 0;

        auto sid = [](MMOStmtId id) { return static_cast<PreparedStatementID>(id); };

        // Generate a simple character ID from timestamp
        uint32_t charId = static_cast<uint32_t>(GetTimestamp() & 0xFFFFFFFF);

        auto result = m_db->SyncQuery(sid(MMOStmtId::InsertCharacter), {MakeInt(charId), MakeString(name)});

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

        auto sid = [](MMOStmtId id) { return static_cast<PreparedStatementID>(id); };

        auto result = m_db->SyncQuery(sid(MMOStmtId::LoadCharacter), {MakeInt(characterId)});

        if (!result.success || result.rows.empty())
            return false;

        // Parse character core data from the key-value result
        outData.characterId = characterId;
        if (!result.rows.empty() && !result.rows[0].columns.empty())
        {
            // The KV store returns the value as a single string column
            const auto& val = result.rows[0].columns[0];
            if (std::holds_alternative<std::string>(val))
            {
                // UpdateCharacter stores the whole blob as one quoted string;
                // InsertCharacter quotes only the leading name token.
                const auto dataStr = UnquoteStoredString(std::get<std::string>(val));
                // Parse pipe-delimited values
                std::istringstream ss(dataStr);
                std::string token;
                std::vector<std::string> tokens;
                while (std::getline(ss, token, '|'))
                    tokens.push_back(token);

                if (tokens.size() >= 14)
                {
                    try
                    {
                        outData.name = UnquoteStoredString(tokens[0]);
                        outData.level = std::stoi(tokens[1]);
                        outData.xp = std::stoi(tokens[2]);
                        outData.areaId = static_cast<uint32_t>(std::stoi(tokens[3]));
                        outData.posX = std::stof(tokens[4]);
                        outData.posY = std::stof(tokens[5]);
                        outData.posZ = std::stof(tokens[6]);
                        outData.rotY = std::stof(tokens[7]);
                        outData.health = std::stof(tokens[8]);
                        outData.maxHealth = std::stof(tokens[9]);
                        outData.mana = std::stof(tokens[10]);
                        outData.maxMana = std::stof(tokens[11]);
                        outData.playTime = std::stof(tokens[12]);
                        outData.inventory.currency = std::stoi(tokens[13]);
                    }
                    catch (const std::exception&)
                    {
                        SPARK_LOG_ERROR(Spark::LogCategory::Game, "MMOPersistence: corrupt character data for ID %u",
                                        characterId);
                    }
                }
            }
        }

        // Load subsystem data
        LoadInventory(characterId, outData.inventory);
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

    void MMOPersistenceSystem::SaveCharacterAsync(const CharacterSaveData& data)
    {
        if (!m_initialized)
            return;

        auto sid = [](MMOStmtId id) { return static_cast<PreparedStatementID>(id); };

        // Build the pipe-delimited character string
        std::ostringstream ss;
        ss << data.name << "|" << data.level << "|" << data.xp << "|" << data.areaId << "|" << data.posX << "|"
           << data.posY << "|" << data.posZ << "|" << data.rotY << "|" << data.health << "|" << data.maxHealth << "|"
           << data.mana << "|" << data.maxMana << "|" << data.playTime << "|" << data.inventory.currency;

        m_db->AsyncQuery(sid(MMOStmtId::UpdateCharacter), {MakeInt(data.characterId), MakeString(ss.str())});

        SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Async save character: %s (ID %u)", data.name.c_str(),
                        data.characterId);

        // Save subsystem data asynchronously via transactions
        SaveInventory(data.characterId, data.inventory);
        SaveReputationState(data.characterId, data.reputationState);
        SaveAchievementState(data.characterId, data.achievementState);
        SaveCraftingState(data.characterId, data.craftingState);
        SaveLockouts(data.characterId, data.dungeonState);

        m_totalSaves++;
        m_lastSaveTime = data.playTime;
    }

    bool MMOPersistenceSystem::SaveCharacterSync(const CharacterSaveData& data)
    {
        if (!m_initialized)
            return false;

        auto sid = [](MMOStmtId id) { return static_cast<PreparedStatementID>(id); };

        std::ostringstream ss;
        ss << data.name << "|" << data.level << "|" << data.xp << "|" << data.areaId << "|" << data.posX << "|"
           << data.posY << "|" << data.posZ << "|" << data.rotY << "|" << data.health << "|" << data.maxHealth << "|"
           << data.mana << "|" << data.maxMana << "|" << data.playTime << "|" << data.inventory.currency;

        auto result =
            m_db->SyncQuery(sid(MMOStmtId::UpdateCharacter), {MakeInt(data.characterId), MakeString(ss.str())});

        if (result.success)
        {
            SaveInventory(data.characterId, data.inventory);
            SaveReputationState(data.characterId, data.reputationState);
            SaveAchievementState(data.characterId, data.achievementState);
            SaveCraftingState(data.characterId, data.craftingState);
            SaveLockouts(data.characterId, data.dungeonState);
            m_totalSaves++;
        }

        return result.success;
    }

    bool MMOPersistenceSystem::DeleteCharacter(uint32_t characterId)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Deleting character from persistence: ID %u", characterId);
        if (!m_initialized)
            return false;

        auto sid = [](MMOStmtId id) { return static_cast<PreparedStatementID>(id); };

        auto result = m_db->SyncQuery(sid(MMOStmtId::DeleteCharacter), {MakeInt(characterId)});
        // Also clean up subsystem data
        m_db->SyncQuery(sid(MMOStmtId::ClearInventory), {MakeInt(characterId)});
        m_db->SyncQuery(sid(MMOStmtId::ClearReputation), {MakeInt(characterId)});
        return result.success;
    }

    std::vector<std::pair<uint32_t, std::string>> MMOPersistenceSystem::ListCharacters(uint32_t accountId)
    {
        std::vector<std::pair<uint32_t, std::string>> result;
        if (!m_initialized)
            return result;

        auto sid = [](MMOStmtId id) { return static_cast<PreparedStatementID>(id); };

        auto queryResult = m_db->SyncQuery(sid(MMOStmtId::ListCharacters));
        if (queryResult.success)
        {
            for (const auto& row : queryResult.rows)
            {
                if (!row.columns.empty() && std::holds_alternative<std::string>(row.columns[0]))
                {
                    const auto& key = std::get<std::string>(row.columns[0]);
                    // Parse character ID from key "character_<id>"
                    if (key.starts_with("character_"))
                    {
                        auto idStr = key.substr(10);
                        try
                        {
                            uint32_t charId = static_cast<uint32_t>(std::stoul(idStr));
                            result.emplace_back(charId, idStr);
                        }
                        catch (const std::exception&)
                        {
                            continue;
                        }
                    }
                }
            }
        }
        return result;
    }

    // =========================================================================
    // Subsystem Save/Load Helpers
    // =========================================================================

    void MMOPersistenceSystem::SaveInventory(uint32_t charId, const InventoryData& inv)
    {
        auto sid = [](MMOStmtId id) { return static_cast<PreparedStatementID>(id); };

        for (size_t i = 0; i < inv.slots.size(); ++i)
        {
            const auto& slot = inv.slots[i];
            if (!slot.IsEmpty())
            {
                m_db->AsyncQuery(sid(MMOStmtId::SaveInventorySlot), {MakeInt(charId), MakeInt(static_cast<int64_t>(i)),
                                                                     MakeInt(slot.itemDefId), MakeInt(slot.count)});
            }
        }
        m_db->AsyncQuery(sid(MMOStmtId::SaveCurrency), {MakeInt(charId), MakeInt(inv.currency)});
    }

    void MMOPersistenceSystem::LoadInventory(uint32_t charId, InventoryData& inv)
    {
        auto sid = [](MMOStmtId id) { return static_cast<PreparedStatementID>(id); };

        auto result = m_db->SyncQuery(sid(MMOStmtId::LoadInventory), {MakeInt(charId)});
        if (result.success)
        {
            inv.slots.clear();
            for (const auto& row : result.rows)
            {
                if (!row.columns.empty() && std::holds_alternative<std::string>(row.columns[0]))
                {
                    // Each result contains the value "itemDefId|count"
                    const auto& val = std::get<std::string>(row.columns[0]);
                    auto sep = val.find('|');
                    if (sep != std::string::npos)
                    {
                        try
                        {
                            ItemStack stack;
                            stack.itemDefId = static_cast<uint32_t>(std::stoul(val.substr(0, sep)));
                            stack.count = std::stoi(val.substr(sep + 1));
                            if (!stack.IsEmpty())
                                inv.slots.push_back(stack);
                        }
                        catch (const std::exception&)
                        {
                            continue;
                        }
                    }
                }
            }
        }

        // Load currency
        auto currResult = m_db->SyncQuery(sid(MMOStmtId::LoadCurrency), {MakeInt(charId)});
        if (currResult.success && !currResult.rows.empty())
        {
            const auto& val = currResult.rows[0].columns[0];
            if (std::holds_alternative<std::string>(val))
            {
                try
                {
                    inv.currency = std::stoi(std::get<std::string>(val));
                }
                catch (const std::exception&)
                {
                    inv.currency = 0;
                }
            }
            else if (std::holds_alternative<int64_t>(val))
                inv.currency = static_cast<int>(std::get<int64_t>(val));
        }
    }

    void MMOPersistenceSystem::SaveReputationState(uint32_t charId, const ReputationState& state)
    {
        auto sid = [](MMOStmtId id) { return static_cast<PreparedStatementID>(id); };

        for (const auto& [factionId, standing] : state.standings)
        {
            m_db->AsyncQuery(sid(MMOStmtId::SaveReputation),
                             {MakeInt(charId), MakeInt(factionId), MakeInt(standing.reputation)});
        }
    }

    void MMOPersistenceSystem::LoadReputationState(uint32_t charId, ReputationState& state)
    {
        auto sid = [](MMOStmtId id) { return static_cast<PreparedStatementID>(id); };

        auto result = m_db->SyncQuery(sid(MMOStmtId::LoadReputation), {MakeInt(charId)});
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
                            auto valResult = m_db->SyncQuery(sid(MMOStmtId::LoadReputationValue),
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

    void MMOPersistenceSystem::SaveAchievementState(uint32_t charId, const AchievementState& state)
    {
        auto sid = [](MMOStmtId id) { return static_cast<PreparedStatementID>(id); };

        // Save completed achievement IDs
        for (uint32_t achId : state.completedIds)
        {
            m_db->AsyncQuery(sid(MMOStmtId::SaveAchievement), {MakeInt(charId), MakeInt(achId)});
        }

        // Save stat counters
        for (const auto& [key, val] : state.stats)
        {
            m_db->AsyncQuery(sid(MMOStmtId::SaveAchievementStat), {MakeInt(charId), MakeString(key), MakeInt(val)});
        }
    }

    void MMOPersistenceSystem::LoadAchievementState(uint32_t charId, AchievementState& state)
    {
        auto sid = [](MMOStmtId id) { return static_cast<PreparedStatementID>(id); };

        // Load completed achievements
        auto achResult = m_db->SyncQuery(sid(MMOStmtId::LoadAchievements), {MakeInt(charId)});
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
        auto statResult = m_db->SyncQuery(sid(MMOStmtId::LoadAchievementStats), {MakeInt(charId)});
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
                        auto valResult = m_db->SyncQuery(sid(MMOStmtId::LoadAchievementStatValue),
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

    void MMOPersistenceSystem::SaveCraftingState(uint32_t charId, const CraftingState& state)
    {
        auto sid = [](MMOStmtId id) { return static_cast<PreparedStatementID>(id); };

        // Save skill levels
        for (const auto& [key, skill] : state.skills)
        {
            m_db->AsyncQuery(sid(MMOStmtId::SaveCraftingSkill),
                             {MakeInt(charId), MakeInt(key), MakeInt(skill.level), MakeInt(skill.currentXP)});
        }

        // Save known recipes
        for (uint32_t recipeId : state.knownRecipes)
        {
            m_db->AsyncQuery(sid(MMOStmtId::SaveKnownRecipe), {MakeInt(charId), MakeInt(recipeId)});
        }
    }

    void MMOPersistenceSystem::LoadCraftingState(uint32_t charId, CraftingState& state)
    {
        auto sid = [](MMOStmtId id) { return static_cast<PreparedStatementID>(id); };

        // Load skills
        auto skillResult = m_db->SyncQuery(sid(MMOStmtId::LoadCraftingSkills), {MakeInt(charId)});
        if (skillResult.success)
        {
            for (const auto& row : skillResult.rows)
            {
                if (!row.columns.empty() && std::holds_alternative<std::string>(row.columns[0]))
                {
                    const auto& key = std::get<std::string>(row.columns[0]);
                    auto prefix = "craft_" + std::to_string(charId) + "_";
                    if (key.starts_with(prefix))
                    {
                        try
                        {
                            int discKey = std::stoi(key.substr(prefix.size()));
                            CraftingSkill skill;
                            skill.discipline = static_cast<CraftingDiscipline>(discKey);
                            skill.level = 1;
                            skill.currentXP = 0;
                            state.skills[discKey] = skill;
                        }
                        catch (const std::exception&)
                        {
                            continue;
                        }
                    }
                }
            }
        }

        // Load known recipes
        auto recipeResult = m_db->SyncQuery(sid(MMOStmtId::LoadKnownRecipes), {MakeInt(charId)});
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

    void MMOPersistenceSystem::SaveLockouts(uint32_t charId, const DungeonPlayerState& state)
    {
        auto sid = [](MMOStmtId id) { return static_cast<PreparedStatementID>(id); };

        for (const auto& lo : state.lockouts)
        {
            m_db->AsyncQuery(sid(MMOStmtId::SaveLockout),
                             {MakeInt(charId), MakeInt(lo.dungeonDefId), MakeInt(static_cast<int64_t>(lo.difficulty)),
                              MakeDouble(static_cast<double>(lo.remaining))});
        }
    }

    void MMOPersistenceSystem::LoadLockouts(uint32_t charId, DungeonPlayerState& state)
    {
        auto sid = [](MMOStmtId id) { return static_cast<PreparedStatementID>(id); };

        auto result = m_db->SyncQuery(sid(MMOStmtId::LoadLockouts), {MakeInt(charId)});
        if (result.success)
        {
            state.lockouts.clear();
            for (const auto& row : result.rows)
            {
                if (!row.columns.empty() && std::holds_alternative<std::string>(row.columns[0]))
                {
                    const auto& key = std::get<std::string>(row.columns[0]);
                    auto prefix = "lockout_" + std::to_string(charId) + "_";
                    if (key.starts_with(prefix))
                    {
                        auto rest = key.substr(prefix.size());
                        auto sep = rest.find('_');
                        if (sep != std::string::npos)
                        {
                            try
                            {
                                LootLockout lo;
                                lo.dungeonDefId = static_cast<uint32_t>(std::stoul(rest.substr(0, sep)));
                                lo.difficulty = static_cast<DungeonDifficulty>(std::stoi(rest.substr(sep + 1)));
                                lo.remaining = 604800.0f; // Default 1 week, actual loaded from value
                                state.lockouts.push_back(lo);
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

        auto sid = [](MMOStmtId id) { return static_cast<PreparedStatementID>(id); };

        // Save guilds
        for (const auto& guild : data.guilds)
        {
            std::ostringstream guildStr;
            guildStr << guild.name << "|" << guild.tag << "|" << guild.leaderId << "|" << guild.bankCurrency << "|"
                     << guild.motd;

            m_db->AsyncQuery(sid(MMOStmtId::SaveGuild), {MakeInt(guild.id), MakeString(guildStr.str())});

            // Save members
            for (const auto& member : guild.members)
            {
                m_db->AsyncQuery(sid(MMOStmtId::SaveGuildMember),
                                 {MakeInt(guild.id), MakeInt(member.playerId), MakeString(member.name),
                                  MakeInt(static_cast<int64_t>(member.rank))});
            }
        }

        // Save boss kill history
        for (const auto& kill : data.bossKillHistory)
        {
            m_db->AsyncQuery(sid(MMOStmtId::SaveBossKill),
                             {MakeInt(kill.bossDefId), MakeInt(static_cast<int64_t>(kill.killTime)),
                              MakeInt(kill.participantCount)});
        }
    }

    bool MMOPersistenceSystem::LoadWorld(WorldSaveData& outData)
    {
        if (!m_initialized)
            return false;

        auto sid = [](MMOStmtId id) { return static_cast<PreparedStatementID>(id); };

        // Load guilds
        auto guildResult = m_db->SyncQuery(sid(MMOStmtId::LoadGuilds));
        if (guildResult.success)
        {
            Spark::SimpleConsole::GetInstance().LogInfo("[MMO] Loaded " + std::to_string(guildResult.rows.size()) +
                                                        " guild records from database");
        }

        // Load boss kill history
        auto bossResult = m_db->SyncQuery(sid(MMOStmtId::LoadBossKills));
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
