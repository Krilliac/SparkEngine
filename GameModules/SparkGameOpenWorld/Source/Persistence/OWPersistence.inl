/**
 * @file OWPersistence.inl
 * @brief OpenWorld atomic disk persistence and versioned snapshot codec implementation.
 */

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <system_error>

namespace OpenWorld
{

    namespace
    {
        constexpr size_t kMaxSaveBytes = 4 * 1024 * 1024;
        constexpr size_t kMaxFastTravelPoints = 256;
        constexpr size_t kMaxPOIProgress = 4096;
        constexpr size_t kMaxResources = 64;
        constexpr size_t kMaxResourceNodes = 4096;
        constexpr size_t kMaxCamps = 1024;
        constexpr size_t kMaxAnimals = 10000;
        constexpr size_t kMaxHerds = 2048;
        constexpr size_t kMaxHerdMembers = 256;
        constexpr size_t kMaxEvents = 1024;
        constexpr size_t kMaxCooldowns = 1024;

        bool ReadBool(std::istream& input, bool& value)
        {
            unsigned raw = 0;
            if (!(input >> raw) || raw > 1)
                return false;
            value = raw != 0;
            return true;
        }

        template <typename Enum> bool ReadEnum(std::istream& input, Enum& value)
        {
            unsigned raw = 0;
            if (!(input >> raw) || raw > std::numeric_limits<uint8_t>::max())
                return false;
            value = static_cast<Enum>(raw);
            return true;
        }

        bool ReadTag(std::istream& input, const char* expected)
        {
            std::string tag;
            return static_cast<bool>(input >> tag) && tag == expected;
        }

        bool ReadCount(std::istream& input, const char* tag, size_t maximum, size_t& count)
        {
            uint64_t raw = 0;
            if (!ReadTag(input, tag) || !(input >> raw) || raw > maximum)
                return false;
            count = static_cast<size_t>(raw);
            return true;
        }

        bool HasOnlyTrailingWhitespace(std::istream& input)
        {
            input >> std::ws;
            return input.eof();
        }

        uint64_t HashSaveBody(std::string_view body)
        {
            uint64_t hash = 14695981039346656037ull;
            for (unsigned char byte : body)
            {
                hash ^= byte;
                hash *= 1099511628211ull;
            }
            return hash;
        }
    } // namespace

    // =========================================================================
    // Save System
    // =========================================================================

    void OWEngineSystems::ConfigurePersistence()
    {
        auto* saveSystem = m_context->GetSaveSystem();
        if (!saveSystem)
            return;

        saveSystem->SetMaxAutoSaves(3);

        SPARK_LOG_INFO(Spark::LogCategory::Game, "Open world persistence initialized");
        Spark::SimpleConsole::GetInstance().LogInfo("[OpenWorld] Versioned gameplay persistence ready");
    }

    std::string OWEngineSystems::SaveGame(const std::string& slotName)
    {
        if (!m_initialized || !m_context || !m_player || !m_exploration || !m_gathering || !m_settlements ||
            !m_wildlife || !m_events)
            return "Open world persistence is not ready";
        if (!IsValidSlotName(slotName))
            return "Invalid save slot name (use 1-64 letters, digits, '_' or '-')";

        auto* saveSystem = m_context->GetSaveSystem();
        auto* world = m_context->GetWorld();
        if (!saveSystem || !world)
            return "Save system or world not available";

        const std::string payload = SerializeSnapshot(CaptureSnapshot());
        if (payload.empty() || payload.size() > kMaxSaveBytes)
            return "Open world state is too large to save";

        const auto finalPath = GetModuleSavePath(slotName);
        auto temporaryPath = finalPath;
        temporaryPath += ".tmp";
        auto backupPath = finalPath;
        backupPath += ".bak";
        std::error_code error;
        std::filesystem::create_directories(finalPath.parent_path(), error);
        if (error)
            return "Could not create OpenWorld save directory: " + error.message();

        std::filesystem::remove(temporaryPath, error);
        error.clear();
        std::filesystem::remove(backupPath, error);
        error.clear();
        {
            std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
            file.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            file.flush();
            if (!file)
            {
                file.close();
                std::filesystem::remove(temporaryPath, error);
                return "Could not write OpenWorld save data";
            }
        }

        const bool hadPrevious = std::filesystem::exists(finalPath, error) && !error;
        if (hadPrevious)
        {
            std::filesystem::rename(finalPath, backupPath, error);
            if (error)
            {
                std::filesystem::remove(temporaryPath, error);
                return "Could not prepare the previous OpenWorld save for replacement";
            }
        }
        std::filesystem::rename(temporaryPath, finalPath, error);
        if (error)
        {
            if (hadPrevious)
            {
                std::error_code restoreError;
                std::filesystem::rename(backupPath, finalPath, restoreError);
                if (restoreError)
                    return "Could not commit OpenWorld save data or restore its backup: " + restoreError.message();
            }
            std::filesystem::remove(temporaryPath, error);
            return "Could not commit OpenWorld save data";
        }

        Spark::SaveMetadata metadata;
        metadata.saveName = "Open World - " + slotName;
        metadata.sceneName = "OpenWorld";
        metadata.playerClass = "Explorer";
        metadata.playerHealth = m_player->GetSurvivalState().health;
        const auto& playerWorld = m_player->GetWorldState();
        metadata.playerPosition = {playerWorld.posX, playerWorld.posY, playerWorld.posZ};

        if (!saveSystem->Save(slotName, *world, metadata))
        {
            std::error_code rollbackError;
            std::filesystem::remove(finalPath, rollbackError);
            if (rollbackError)
                return "Engine world save failed and the new OpenWorld sidecar could not be removed: " +
                       rollbackError.message();
            if (hadPrevious)
            {
                std::filesystem::rename(backupPath, finalPath, rollbackError);
                if (rollbackError)
                    return "Engine world save failed and the previous OpenWorld save could not be restored: " +
                           rollbackError.message();
            }
            return hadPrevious ? "Engine world save failed; previous OpenWorld save was preserved"
                               : "Engine world save failed; OpenWorld sidecar was rolled back";
        }

        std::filesystem::remove(backupPath, error);
        return "Saved OpenWorld game to slot '" + slotName + "'";
    }

    std::string OWEngineSystems::LoadGame(const std::string& slotName)
    {
        if (!m_initialized || !m_context || !m_player || !m_exploration || !m_gathering || !m_settlements ||
            !m_wildlife || !m_events)
            return "Open world persistence is not ready";
        if (!IsValidSlotName(slotName))
            return "Invalid save slot name (use 1-64 letters, digits, '_' or '-')";

        auto* saveSystem = m_context->GetSaveSystem();
        auto* world = m_context->GetWorld();
        if (!saveSystem || !world)
            return "Save system or world not available";
        if (!saveSystem->SaveExists(slotName))
            return "No save found in slot '" + slotName + "'";

        const auto modulePath = GetModuleSavePath(slotName);
        std::error_code filesystemError;
        const auto byteCount = std::filesystem::file_size(modulePath, filesystemError);
        if (filesystemError)
            return "OpenWorld gameplay data is missing for slot '" + slotName + "'";
        if (byteCount == 0 || byteCount > kMaxSaveBytes)
            return "OpenWorld gameplay data has an invalid size";

        std::ifstream file(modulePath, std::ios::binary);
        std::string payload(static_cast<size_t>(byteCount), '\0');
        file.read(payload.data(), static_cast<std::streamsize>(payload.size()));
        if (!file)
            return "Could not read OpenWorld gameplay data";

        OWGameSaveData data;
        std::string validationError;
        if (!DeserializeSnapshot(payload, data, validationError) || !ValidateSnapshot(data, validationError))
            return "OpenWorld save is invalid: " + validationError;
        if (!saveSystem->Load(slotName, *world))
            return "Engine world load failed; gameplay state was not changed";
        if (!RestoreSnapshot(data, validationError))
            return "Engine world loaded, but OpenWorld state restore failed: " + validationError;

        return "Loaded OpenWorld game from slot '" + slotName + "'";
    }

    OWGameSaveData OWEngineSystems::CaptureSnapshot() const
    {
        OWGameSaveData data;
        data.player = m_player->CaptureSaveState();
        data.exploration = m_exploration->CaptureSaveState();
        data.gathering = m_gathering->CaptureSaveState();
        data.settlements = m_settlements->CaptureSaveState();
        data.wildlife = m_wildlife->CaptureSaveState();
        data.events = m_events->CaptureSaveState();
        return data;
    }

    bool OWEngineSystems::ValidateSnapshot(const OWGameSaveData& data, std::string& error) const
    {
        if (data.version != kOpenWorldSaveVersion)
        {
            error = "unsupported OpenWorld save version";
            return false;
        }
        auto player = *m_player;
        auto exploration = *m_exploration;
        auto gathering = *m_gathering;
        auto settlements = *m_settlements;
        auto wildlife = *m_wildlife;
        auto events = *m_events;
        return player.RestoreSaveState(data.player, &error) && exploration.RestoreSaveState(data.exploration, &error) &&
               gathering.RestoreSaveState(data.gathering, &error) &&
               settlements.RestoreSaveState(data.settlements, &error) &&
               wildlife.RestoreSaveState(data.wildlife, &error) && events.RestoreSaveState(data.events, &error);
    }

    bool OWEngineSystems::RestoreSnapshot(const OWGameSaveData& data, std::string& error)
    {
        if (!ValidateSnapshot(data, error))
            return false;

        // Every subsystem validates before mutation, so these calls cannot partially
        // apply a well-formed snapshot.
        return m_player->RestoreSaveState(data.player, &error) &&
               m_exploration->RestoreSaveState(data.exploration, &error) &&
               m_gathering->RestoreSaveState(data.gathering, &error) &&
               m_settlements->RestoreSaveState(data.settlements, &error) &&
               m_wildlife->RestoreSaveState(data.wildlife, &error) && m_events->RestoreSaveState(data.events, &error);
    }

    bool OWEngineSystems::IsValidSlotName(const std::string& slotName)
    {
        if (slotName.empty() || slotName.size() > 64)
            return false;
        return std::all_of(
            slotName.begin(), slotName.end(), [](char character)
            { return std::isalnum(static_cast<unsigned char>(character)) || character == '_' || character == '-'; });
    }

    std::filesystem::path OWEngineSystems::GetModuleSavePath(const std::string& slotName)
    {
        return std::filesystem::path("Saves") / "OpenWorld" / (slotName + ".ow_save");
    }

    std::string OWEngineSystems::SerializeSnapshot(const OWGameSaveData& data)
    {
        std::ostringstream output;
        output.imbue(std::locale::classic());
        output << std::setprecision(std::numeric_limits<float>::max_digits10);
        output << "SPARK_OPEN_WORLD_SAVE " << data.version << '\n';

        const auto& player = data.player;
        const auto& survival = player.survival;
        const auto& world = player.world;
        output << "PLAYER " << survival.health << ' ' << survival.maxHealth << ' ' << survival.stamina << ' '
               << survival.maxStamina << ' ' << survival.hunger << ' ' << survival.thirst << ' ' << survival.temperature
               << ' ' << survival.warmth << ' ' << world.posX << ' ' << world.posY << ' ' << world.posZ << ' '
               << world.yaw << ' ' << world.speed << ' ' << world.isSprinting << ' ' << world.isSwimming << ' '
               << world.isClimbing << ' ' << world.isInShelter << ' ' << world.currentRegionId << ' '
               << player.survivalTickTimer << '\n';
        output << "FAST_TRAVEL " << player.fastTravelPoints.size() << '\n';
        for (const auto& point : player.fastTravelPoints)
            output << "F " << point.pointId << ' ' << std::quoted(point.name) << ' ' << point.x << ' ' << point.y << ' '
                   << point.z << ' ' << point.regionId << '\n';

        output << "EXPLORATION " << data.exploration.progress.size() << ' ' << data.exploration.totalXPEarned << '\n';
        for (const auto& progress : data.exploration.progress)
            output << "X " << progress.poiId << ' ' << static_cast<unsigned>(progress.state) << ' '
                   << progress.secretFound << '\n';

        output << "INVENTORY " << data.gathering.inventory.size() << ' ' << data.gathering.totalHarvested << ' '
               << data.gathering.totalCrafted << '\n';
        for (const auto& [type, amount] : data.gathering.inventory)
            output << "I " << static_cast<unsigned>(type) << ' ' << amount << '\n';
        output << "RESOURCE_NODES " << data.gathering.nodes.size() << '\n';
        for (const auto& node : data.gathering.nodes)
            output << "N " << node.nodeId << ' ' << node.currentYield << ' ' << node.respawnTimer << ' '
                   << node.isDepleted << '\n';

        output << "CAMPS " << data.settlements.camps.size() << ' ' << data.settlements.nextCampId << '\n';
        for (const auto& camp : data.settlements.camps)
            output << "C " << camp.campId << ' ' << std::quoted(camp.name) << ' ' << static_cast<unsigned>(camp.tier)
                   << ' ' << camp.posX << ' ' << camp.posY << ' ' << camp.posZ << ' ' << camp.regionId << ' '
                   << camp.hasCraftingStation << ' ' << camp.hasStorageChest << ' ' << camp.hasCookingFire << ' '
                   << camp.storageCapacity << '\n';

        output << "WILDLIFE " << data.wildlife.animals.size() << ' ' << data.wildlife.nextInstanceId << ' '
               << data.wildlife.respawnTimer << '\n';
        for (const auto& animal : data.wildlife.animals)
            output << "A " << animal.instanceId << ' ' << static_cast<unsigned>(animal.type) << ' ' << animal.posX
                   << ' ' << animal.posY << ' ' << animal.posZ << ' ' << animal.health << ' '
                   << static_cast<unsigned>(animal.behavior) << ' ' << animal.herdId << ' ' << animal.regionId << ' '
                   << animal.isTamed << ' ' << animal.isAlive << '\n';
        output << "HERDS " << data.wildlife.herds.size() << ' ' << data.wildlife.nextHerdId << '\n';
        for (const auto& herd : data.wildlife.herds)
        {
            output << "H " << herd.herdId << ' ' << static_cast<unsigned>(herd.type) << ' ' << herd.centerX << ' '
                   << herd.centerZ << ' ' << herd.regionId << ' ' << herd.memberIds.size();
            for (uint32_t memberId : herd.memberIds)
                output << ' ' << memberId;
            output << '\n';
        }

        output << "EVENTS " << data.events.activeEvents.size() << ' ' << data.events.nextEventId << ' '
               << data.events.eventCheckTimer << ' ' << data.events.totalEventsCompleted << '\n';
        for (const auto& event : data.events.activeEvents)
            output << "E " << event.eventId << ' ' << event.templateId << ' ' << std::quoted(event.name) << ' '
                   << static_cast<unsigned>(event.type) << ' ' << static_cast<unsigned>(event.state) << ' '
                   << event.posX << ' ' << event.posZ << ' ' << event.regionId << ' ' << event.timeRemaining << ' '
                   << event.totalDuration << ' ' << event.playerParticipating << '\n';
        output << "COOLDOWNS " << data.events.cooldowns.size() << '\n';
        for (const auto& [templateId, remaining] : data.events.cooldowns)
            output << "D " << templateId << ' ' << remaining << '\n';
        output << "END\n";

        std::string serialized = output.str();
        const size_t bodyOffset = serialized.find('\n') + 1;
        const uint64_t checksum = HashSaveBody(std::string_view(serialized).substr(bodyOffset));
        serialized.insert(bodyOffset, "CHECKSUM " + std::to_string(checksum) + "\n");
        return serialized;
    }

    bool OWEngineSystems::DeserializeSnapshot(std::string_view text, OWGameSaveData& outData, std::string& error)
    {
        auto fail = [&](const char* message)
        {
            error = message;
            return false;
        };
        if (text.empty() || text.size() > kMaxSaveBytes)
            return fail("save payload size is invalid");

        const size_t headerEnd = text.find('\n');
        const size_t checksumEnd =
            headerEnd == std::string_view::npos ? std::string_view::npos : text.find('\n', headerEnd + 1);
        if (headerEnd == std::string_view::npos || checksumEnd == std::string_view::npos)
            return fail("save header is truncated");

        std::istringstream input{std::string(text)};
        input.imbue(std::locale::classic());
        OWGameSaveData parsed;
        if (!ReadTag(input, "SPARK_OPEN_WORLD_SAVE") || !(input >> parsed.version))
            return fail("missing OpenWorld save header");
        if (parsed.version != kOpenWorldSaveVersion)
            return fail("unsupported OpenWorld save version");
        uint64_t storedChecksum = 0;
        if (!ReadTag(input, "CHECKSUM") || !(input >> storedChecksum) ||
            storedChecksum != HashSaveBody(text.substr(checksumEnd + 1)))
            return fail("OpenWorld save checksum mismatch");

        auto& player = parsed.player;
        auto& survival = player.survival;
        auto& world = player.world;
        if (!ReadTag(input, "PLAYER") ||
            !(input >> survival.health >> survival.maxHealth >> survival.stamina >> survival.maxStamina >>
              survival.hunger >> survival.thirst >> survival.temperature >> survival.warmth >> world.posX >>
              world.posY >> world.posZ >> world.yaw >> world.speed) ||
            !ReadBool(input, world.isSprinting) || !ReadBool(input, world.isSwimming) ||
            !ReadBool(input, world.isClimbing) || !ReadBool(input, world.isInShelter) ||
            !(input >> world.currentRegionId >> player.survivalTickTimer))
            return fail("invalid player record");

        size_t count = 0;
        if (!ReadCount(input, "FAST_TRAVEL", kMaxFastTravelPoints, count))
            return fail("invalid fast travel count");
        player.fastTravelPoints.resize(count);
        for (auto& point : player.fastTravelPoints)
        {
            if (!ReadTag(input, "F") ||
                !(input >> point.pointId >> std::quoted(point.name) >> point.x >> point.y >> point.z >>
                  point.regionId) ||
                point.name.size() > 128)
                return fail("invalid fast travel record");
        }

        if (!ReadTag(input, "EXPLORATION"))
            return fail("missing exploration record");
        uint64_t rawCount = 0;
        if (!(input >> rawCount >> parsed.exploration.totalXPEarned) || rawCount > kMaxPOIProgress)
            return fail("invalid exploration count");
        parsed.exploration.progress.resize(static_cast<size_t>(rawCount));
        for (auto& progress : parsed.exploration.progress)
        {
            if (!ReadTag(input, "X") || !(input >> progress.poiId) || !ReadEnum(input, progress.state) ||
                !ReadBool(input, progress.secretFound))
                return fail("invalid exploration record");
        }

        if (!ReadTag(input, "INVENTORY") ||
            !(input >> rawCount >> parsed.gathering.totalHarvested >> parsed.gathering.totalCrafted) ||
            rawCount > kMaxResources)
            return fail("invalid inventory count");
        parsed.gathering.inventory.resize(static_cast<size_t>(rawCount));
        for (auto& [type, amount] : parsed.gathering.inventory)
        {
            if (!ReadTag(input, "I") || !ReadEnum(input, type) || !(input >> amount))
                return fail("invalid inventory record");
        }
        if (!ReadCount(input, "RESOURCE_NODES", kMaxResourceNodes, count))
            return fail("invalid resource node count");
        parsed.gathering.nodes.resize(count);
        for (auto& node : parsed.gathering.nodes)
        {
            if (!ReadTag(input, "N") || !(input >> node.nodeId >> node.currentYield >> node.respawnTimer) ||
                !ReadBool(input, node.isDepleted))
                return fail("invalid resource node record");
        }

        if (!ReadTag(input, "CAMPS") || !(input >> rawCount >> parsed.settlements.nextCampId) || rawCount > kMaxCamps)
            return fail("invalid camp count");
        parsed.settlements.camps.resize(static_cast<size_t>(rawCount));
        for (auto& camp : parsed.settlements.camps)
        {
            if (!ReadTag(input, "C") || !(input >> camp.campId >> std::quoted(camp.name)) ||
                !ReadEnum(input, camp.tier) || !(input >> camp.posX >> camp.posY >> camp.posZ >> camp.regionId) ||
                !ReadBool(input, camp.hasCraftingStation) || !ReadBool(input, camp.hasStorageChest) ||
                !ReadBool(input, camp.hasCookingFire) || !(input >> camp.storageCapacity) || camp.name.size() > 128)
                return fail("invalid player camp record");
        }

        if (!ReadTag(input, "WILDLIFE") ||
            !(input >> rawCount >> parsed.wildlife.nextInstanceId >> parsed.wildlife.respawnTimer) ||
            rawCount > kMaxAnimals)
            return fail("invalid wildlife count");
        parsed.wildlife.animals.resize(static_cast<size_t>(rawCount));
        for (auto& animal : parsed.wildlife.animals)
        {
            if (!ReadTag(input, "A") || !(input >> animal.instanceId) || !ReadEnum(input, animal.type) ||
                !(input >> animal.posX >> animal.posY >> animal.posZ >> animal.health) ||
                !ReadEnum(input, animal.behavior) || !(input >> animal.herdId >> animal.regionId) ||
                !ReadBool(input, animal.isTamed) || !ReadBool(input, animal.isAlive))
                return fail("invalid wildlife animal record");
        }
        if (!ReadTag(input, "HERDS") || !(input >> rawCount >> parsed.wildlife.nextHerdId) || rawCount > kMaxHerds)
            return fail("invalid wildlife herd count");
        parsed.wildlife.herds.resize(static_cast<size_t>(rawCount));
        for (auto& herd : parsed.wildlife.herds)
        {
            uint64_t memberCount = 0;
            if (!ReadTag(input, "H") || !(input >> herd.herdId) || !ReadEnum(input, herd.type) ||
                !(input >> herd.centerX >> herd.centerZ >> herd.regionId >> memberCount) ||
                memberCount > kMaxHerdMembers)
                return fail("invalid wildlife herd record");
            herd.memberIds.resize(static_cast<size_t>(memberCount));
            for (uint32_t& memberId : herd.memberIds)
            {
                if (!(input >> memberId))
                    return fail("invalid wildlife herd membership");
            }
        }

        if (!ReadTag(input, "EVENTS") ||
            !(input >> rawCount >> parsed.events.nextEventId >> parsed.events.eventCheckTimer >>
              parsed.events.totalEventsCompleted) ||
            rawCount > kMaxEvents)
            return fail("invalid dynamic event count");
        parsed.events.activeEvents.resize(static_cast<size_t>(rawCount));
        for (auto& event : parsed.events.activeEvents)
        {
            if (!ReadTag(input, "E") || !(input >> event.eventId >> event.templateId >> std::quoted(event.name)) ||
                !ReadEnum(input, event.type) || !ReadEnum(input, event.state) ||
                !(input >> event.posX >> event.posZ >> event.regionId >> event.timeRemaining >> event.totalDuration) ||
                !ReadBool(input, event.playerParticipating) || event.name.size() > 128)
                return fail("invalid active world event record");
        }
        if (!ReadCount(input, "COOLDOWNS", kMaxCooldowns, count))
            return fail("invalid event cooldown count");
        parsed.events.cooldowns.resize(count);
        for (auto& [templateId, remaining] : parsed.events.cooldowns)
        {
            if (!ReadTag(input, "D") || !(input >> templateId >> remaining))
                return fail("invalid event cooldown record");
        }
        if (!ReadTag(input, "END") || !HasOnlyTrailingWhitespace(input))
            return fail("unexpected trailing or missing save data");

        outData = std::move(parsed);
        error.clear();
        return true;
    }

} // namespace OpenWorld
