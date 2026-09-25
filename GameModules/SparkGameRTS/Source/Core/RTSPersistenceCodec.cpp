/**
 * @file RTSPersistenceCodec.cpp
 * @brief Text codec for the full-state RTS snapshot (SPARK_RTS_STATE_V2).
 *
 * One record per line of space-separated tokens. Floats travel as IEEE-754 bit patterns so every value round-trips
 * exactly, and the strict reader rejects signs on unsigned fields, partial numbers, truncation, and trailing data.
 */

#include "RTSPersistence.h"

#include <bit>
#include <cctype>
#include <charconv>
#include <string>

namespace RTS
{
    namespace
    {
        constexpr std::string_view Magic = "SPARK_RTS_STATE_V2";
        constexpr std::string_view LegacyMagicV1 = "SPARK_RTS_STATE_V1";
        constexpr size_t MaxRecords = RTSPersistence::MAX_RECORDS;
        constexpr size_t MaxPlayerEconomies = static_cast<size_t>(RTSFaction::Count);
        constexpr size_t MaxProductionQueue = RTSPersistence::MAX_PRODUCTION_QUEUE;
        constexpr size_t MaxNodeWorkers = RTSPersistence::MAX_NODE_WORKERS;

        bool Fail(std::string& error, std::string message)
        {
            error = std::move(message);
            return false;
        }

        /// Space-separated tokens, one record per line. Floats travel as IEEE-754 bit patterns: exact both ways.
        class Writer
        {
          public:
            Writer& Tag(std::string_view tag)
            {
                Separate();
                m_text += tag;
                return *this;
            }
            Writer& U(uint64_t value)
            {
                Separate();
                m_text += std::to_string(value);
                return *this;
            }
            Writer& I(int value)
            {
                Separate();
                m_text += std::to_string(value);
                return *this;
            }
            Writer& F(float value) { return U(std::bit_cast<uint32_t>(value)); }
            template <typename Enum> Writer& E(Enum value) { return U(static_cast<uint64_t>(value)); }
            Writer& B(bool value) { return U(value ? 1u : 0u); }
            void Line()
            {
                m_text += '\n';
                m_lineStart = true;
            }
            std::string Take() { return std::move(m_text); }

          private:
            void Separate()
            {
                if (!m_lineStart)
                    m_text += ' ';
                m_lineStart = false;
            }

            std::string m_text;
            bool m_lineStart = true;
        };

        /// Strict token reader: every number must be a complete decimal token (no sign on unsigned values).
        class Reader
        {
          public:
            explicit Reader(std::string_view text) : m_text(text) {}

            bool Tag(std::string_view expected)
            {
                std::string_view token;
                return Next(token) && token == expected;
            }
            bool Peek(std::string_view& token) const
            {
                Reader copy = *this;
                return copy.Next(token);
            }
            template <typename Integer> bool Number(Integer& value)
            {
                std::string_view token;
                if (!Next(token))
                    return false;
                const auto [end, status] = std::from_chars(token.data(), token.data() + token.size(), value);
                return status == std::errc{} && end == token.data() + token.size();
            }
            bool Count(size_t& value, size_t limit) { return Number(value) && value <= limit; }
            bool F(float& value)
            {
                uint32_t bits = 0;
                if (!Number(bits))
                    return false;
                value = std::bit_cast<float>(bits);
                return true;
            }
            template <typename Enum> bool E(Enum& value)
            {
                unsigned raw = 0;
                if (!Number(raw) || raw >= static_cast<unsigned>(Enum::Count))
                    return false;
                value = static_cast<Enum>(raw);
                return true;
            }
            bool B(bool& value)
            {
                unsigned raw = 0;
                if (!Number(raw) || raw > 1)
                    return false;
                value = raw != 0;
                return true;
            }
            bool AtEnd()
            {
                SkipSpace();
                return m_position == m_text.size();
            }

          private:
            void SkipSpace()
            {
                while (m_position < m_text.size() && std::isspace(static_cast<unsigned char>(m_text[m_position])))
                    ++m_position;
            }
            bool Next(std::string_view& token)
            {
                SkipSpace();
                const size_t start = m_position;
                while (m_position < m_text.size() && !std::isspace(static_cast<unsigned char>(m_text[m_position])))
                    ++m_position;
                token = m_text.substr(start, m_position - start);
                return !token.empty();
            }

            std::string_view m_text;
            size_t m_position = 0;
        };

        bool ReadCommand(Reader& in, UnitCommand& command)
        {
            return in.E(command.type) && in.F(command.targetX) && in.F(command.targetY) &&
                   in.Number(command.targetEntity);
        }

        void WriteFog(Writer& out, const std::vector<FogGrid>& grids)
        {
            out.Tag("FOG").I(grids.front().width).I(grids.front().height);
            out.Line();
            for (const FogGrid& grid : grids)
            {
                // Run-length encoded: explored areas are large contiguous blocks.
                std::vector<std::pair<RTSVisibility, size_t>> runs;
                for (RTSVisibility cell : grid.cells)
                {
                    if (!runs.empty() && runs.back().first == cell)
                        ++runs.back().second;
                    else
                        runs.emplace_back(cell, 1);
                }
                out.Tag("F").U(runs.size());
                for (const auto& [cell, length] : runs)
                    out.E(cell).U(length);
                out.Line();
            }
        }

        bool ReadFog(Reader& in, std::vector<FogGrid>& grids)
        {
            int width = 0;
            int height = 0;
            if (!in.Tag("FOG") || !in.Number(width) || !in.Number(height) || width <= 0 || height <= 0 ||
                width > RTSFogOfWarSystem::MAX_MAP_DIMENSION || height > RTSFogOfWarSystem::MAX_MAP_DIMENSION)
                return false;
            const size_t cellCount = static_cast<size_t>(width) * static_cast<size_t>(height);
            grids.assign(static_cast<size_t>(RTSFaction::Count), FogGrid{});
            for (FogGrid& grid : grids)
            {
                size_t runCount = 0;
                if (!in.Tag("F") || !in.Count(runCount, cellCount))
                    return false;
                grid.width = width;
                grid.height = height;
                grid.cells.reserve(cellCount);
                for (size_t run = 0; run < runCount; ++run)
                {
                    RTSVisibility cell = RTSVisibility::Unexplored;
                    size_t length = 0;
                    if (!in.E(cell) || !in.Count(length, cellCount - grid.cells.size()) || length == 0)
                        return false;
                    grid.cells.insert(grid.cells.end(), length, cell);
                }
                if (grid.cells.size() != cellCount)
                    return false;
            }
            return true;
        }
    } // namespace

    std::string RTSPersistence::Serialize(const RTSPersistenceSnapshot& snapshot)
    {
        std::string error;
        return Serialize(snapshot, error);
    }

    std::string RTSPersistence::Serialize(const RTSPersistenceSnapshot& snapshot, std::string& error)
    {
        if (!Validate(snapshot, error))
            return {};

        Writer out;
        out.Tag(Magic).Line();
        out.Tag("TICK").U(snapshot.tick).Line();
        out.Tag("IDS").U(snapshot.nextUnitId).U(snapshot.nextBuildingId).U(snapshot.nextNodeId).Line();
        out.Tag("HARVEST").F(snapshot.gatherTimer).Line();

        out.Tag("UNITS").U(snapshot.units.size()).Line();
        for (const UnitData& unit : snapshot.units)
        {
            out.Tag("U").U(unit.unitId).E(unit.type).E(unit.faction).E(unit.state).F(unit.health).F(unit.maxHealth);
            out.F(unit.damage).F(unit.attackSpeed).F(unit.moveSpeed).F(unit.visionRange).F(unit.posX).F(unit.posY);
            out.U(unit.targetId).Line();
        }

        out.Tag("BUILDINGS").U(snapshot.buildings.size()).Line();
        for (const BuildingData& building : snapshot.buildings)
        {
            out.Tag("B").U(building.buildingId).E(building.type).E(building.faction).F(building.health);
            out.F(building.maxHealth).F(building.posX).F(building.posY).B(building.constructionComplete);
            out.F(building.constructionProgress).F(building.constructionTime).U(building.productionQueue.size());
            out.Line();
            for (const ProductionEntry& entry : building.productionQueue)
                out.Tag("P").E(entry.unitType).F(entry.timeRemaining).F(entry.totalTime).Line();
        }

        out.Tag("PLAYERS").U(snapshot.players.size()).Line();
        for (const auto& [faction, resources] : snapshot.players)
        {
            out.Tag("R").E(faction).I(resources.minerals).I(resources.gas).I(resources.currentSupply);
            out.I(resources.maxSupply).Line();
        }

        out.Tag("NODES").U(snapshot.resourceNodes.size()).Line();
        for (const ResourceNode& node : snapshot.resourceNodes)
        {
            out.Tag("N").U(node.nodeId).E(node.type).F(node.posX).F(node.posY).I(node.remaining).I(node.maxWorkers);
            out.U(node.assignedWorkers.size());
            for (uint32_t workerId : node.assignedWorkers)
                out.U(workerId);
            out.Line();
        }

        out.Tag("COMMANDS").U(snapshot.commandQueues.size()).Line();
        for (const auto& [unitId, queue] : snapshot.commandQueues)
        {
            out.Tag("Q").U(unitId).U(queue.size());
            for (const UnitCommand& command : queue)
                out.E(command.type).F(command.targetX).F(command.targetY).U(command.targetEntity);
            out.Line();
        }
        out.Tag("SELECTION").U(snapshot.selection.size());
        for (uint32_t unitId : snapshot.selection)
            out.U(unitId);
        out.Line();

        const RTSMatchSnapshot& match = snapshot.match;
        out.Tag("MATCH").E(match.state).F(match.matchTime).B(match.hasWinner).E(match.winner);
        out.U(match.players.size()).Line();
        for (const PlayerSetup& player : match.players)
        {
            out.Tag("M").E(player.faction).F(player.startX).F(player.startY).B(player.isAI);
            out.B(player.hasSurrendered).B(player.isEliminated).Line();
        }

        WriteFog(out, snapshot.fog);
        out.Tag("END").Line();
        error.clear();
        return out.Take();
    }

    bool RTSPersistence::Deserialize(std::string_view text, RTSPersistenceSnapshot& outSnapshot, std::string& error)
    {
        Reader in(text);
        RTSPersistenceSnapshot parsed;
        size_t count = 0;

        std::string_view magic;
        if (in.Peek(magic) && magic == LegacyMagicV1)
            return Fail(error, "unsupported RTS snapshot version 1 (records only; cannot resume a skirmish)");
        if (!in.Tag(Magic) || !in.Tag("TICK") || !in.Number(parsed.tick) || !in.Tag("IDS") ||
            !in.Number(parsed.nextUnitId) || !in.Number(parsed.nextBuildingId) || !in.Number(parsed.nextNodeId) ||
            !in.Tag("HARVEST") || !in.F(parsed.gatherTimer))
            return Fail(error, "invalid RTS snapshot header");

        if (!in.Tag("UNITS") || !in.Count(count, MaxRecords))
            return Fail(error, "malformed unit section");
        parsed.units.resize(count);
        for (UnitData& unit : parsed.units)
        {
            if (!in.Tag("U") || !in.Number(unit.unitId) || !in.E(unit.type) || !in.E(unit.faction) ||
                !in.E(unit.state) || !in.F(unit.health) || !in.F(unit.maxHealth) || !in.F(unit.damage) ||
                !in.F(unit.attackSpeed) || !in.F(unit.moveSpeed) || !in.F(unit.visionRange) || !in.F(unit.posX) ||
                !in.F(unit.posY) || !in.Number(unit.targetId))
                return Fail(error, "malformed unit record");
        }

        if (!in.Tag("BUILDINGS") || !in.Count(count, MaxRecords))
            return Fail(error, "malformed building section");
        parsed.buildings.resize(count);
        for (BuildingData& building : parsed.buildings)
        {
            size_t queueCount = 0;
            if (!in.Tag("B") || !in.Number(building.buildingId) || !in.E(building.type) || !in.E(building.faction) ||
                !in.F(building.health) || !in.F(building.maxHealth) || !in.F(building.posX) || !in.F(building.posY) ||
                !in.B(building.constructionComplete) || !in.F(building.constructionProgress) ||
                !in.F(building.constructionTime) || !in.Count(queueCount, MaxProductionQueue))
                return Fail(error, "malformed building record");
            building.productionQueue.resize(queueCount);
            for (ProductionEntry& entry : building.productionQueue)
            {
                if (!in.Tag("P") || !in.E(entry.unitType) || !in.F(entry.timeRemaining) || !in.F(entry.totalTime))
                    return Fail(error, "malformed production queue entry");
            }
        }

        if (!in.Tag("PLAYERS") || !in.Count(count, MaxPlayerEconomies))
            return Fail(error, "malformed player economy section");
        parsed.players.resize(count);
        for (auto& [faction, resources] : parsed.players)
        {
            if (!in.Tag("R") || !in.E(faction) || !in.Number(resources.minerals) || !in.Number(resources.gas) ||
                !in.Number(resources.currentSupply) || !in.Number(resources.maxSupply))
                return Fail(error, "malformed player economy record");
        }

        if (!in.Tag("NODES") || !in.Count(count, MaxRecords))
            return Fail(error, "malformed resource node section");
        parsed.resourceNodes.resize(count);
        for (ResourceNode& node : parsed.resourceNodes)
        {
            size_t workerCount = 0;
            if (!in.Tag("N") || !in.Number(node.nodeId) || !in.E(node.type) || !in.F(node.posX) || !in.F(node.posY) ||
                !in.Number(node.remaining) || !in.Number(node.maxWorkers) || !in.Count(workerCount, MaxNodeWorkers))
                return Fail(error, "malformed resource node record");
            node.assignedWorkers.resize(workerCount);
            for (uint32_t& workerId : node.assignedWorkers)
            {
                if (!in.Number(workerId))
                    return Fail(error, "truncated resource worker list");
            }
        }

        if (!in.Tag("COMMANDS") || !in.Count(count, MaxRecords))
            return Fail(error, "malformed command section");
        for (size_t i = 0; i < count; ++i)
        {
            uint32_t unitId = 0;
            size_t queueCount = 0;
            if (!in.Tag("Q") || !in.Number(unitId) || !in.Count(queueCount, RTSCommandSystem::MAX_QUEUED_COMMANDS))
                return Fail(error, "malformed command queue");
            std::vector<UnitCommand> queue(queueCount);
            for (UnitCommand& command : queue)
            {
                if (!ReadCommand(in, command))
                    return Fail(error, "malformed command");
            }
            if (!parsed.commandQueues.emplace(unitId, std::move(queue)).second)
                return Fail(error, "duplicate command queue for unit " + std::to_string(unitId));
        }
        if (!in.Tag("SELECTION") || !in.Count(count, MaxRecords))
            return Fail(error, "malformed selection");
        parsed.selection.resize(count);
        for (uint32_t& unitId : parsed.selection)
        {
            if (!in.Number(unitId))
                return Fail(error, "truncated selection");
        }

        RTSMatchSnapshot& match = parsed.match;
        if (!in.Tag("MATCH") || !in.E(match.state) || !in.F(match.matchTime) || !in.B(match.hasWinner) ||
            !in.E(match.winner) || !in.Count(count, static_cast<size_t>(RTSMatchSystem::MAX_PLAYERS)))
            return Fail(error, "malformed match record");
        match.players.resize(count);
        for (PlayerSetup& player : match.players)
        {
            if (!in.Tag("M") || !in.E(player.faction) || !in.F(player.startX) || !in.F(player.startY) ||
                !in.B(player.isAI) || !in.B(player.hasSurrendered) || !in.B(player.isEliminated))
                return Fail(error, "malformed match player");
        }

        if (!ReadFog(in, parsed.fog))
            return Fail(error, "malformed fog of war section");
        if (!in.Tag("END"))
            return Fail(error, "missing RTS snapshot terminator");
        if (!in.AtEnd())
            return Fail(error, "unexpected trailing RTS snapshot data");
        if (!Validate(parsed, error))
            return false;

        outSnapshot = std::move(parsed);
        error.clear();
        return true;
    }

} // namespace RTS
