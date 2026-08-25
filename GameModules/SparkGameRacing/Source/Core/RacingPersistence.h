/**
 * @file RacingPersistence.h
 * @brief Deterministic, validated snapshot codec for Racing's non-ECS state.
 */

#pragma once

#include "AI/RacingAIDriver.h"
#include "Race/RacingRaceManager.h"
#include "Track/RacingTrackSystem.h"
#include "Vehicle/RacingVehicleSystem.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Racing
{
    struct RacingPersistenceSnapshot
    {
        uint32_t trackId = 0;
        AIDifficulty difficulty = AIDifficulty::Medium;
        std::vector<VehicleInstance> vehicles;
        RacingRaceSnapshot race;
    };

    class RacingPersistence
    {
      public:
        static constexpr std::string_view StateKey = "SparkGameRacing.race.v1";

        static bool IsValidSlotName(std::string_view slotName)
        {
            if (slotName.empty() || slotName.size() > 64)
                return false;
            return std::ranges::all_of(slotName,
                                       [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; });
        }

        static RacingPersistenceSnapshot Capture(const RacingTrackSystem& trackSystem,
                                                 const RacingVehicleSystem& vehicleSystem,
                                                 const RacingRaceManager& raceManager, const RacingAIDriver& aiDriver)
        {
            RacingPersistenceSnapshot snapshot;
            snapshot.trackId = trackSystem.GetCurrentTrack().id;
            snapshot.difficulty = aiDriver.GetGlobalDifficulty();
            snapshot.vehicles = vehicleSystem.GetVehicles();
            snapshot.race = raceManager.CaptureState();
            std::ranges::sort(snapshot.vehicles, {}, &VehicleInstance::id);
            return snapshot;
        }

        static bool Validate(const RacingPersistenceSnapshot& snapshot, std::string& error)
        {
            constexpr size_t MaxRacers = 256;
            if (snapshot.trackId == 0 || snapshot.difficulty >= AIDifficulty::Count ||
                snapshot.vehicles.size() > MaxRacers || snapshot.race.racers.size() > MaxRacers ||
                snapshot.race.championship.size() > MaxRacers)
            {
                return Fail(error, "invalid racing snapshot header");
            }

            RacingVehicleSystem vehicleValidator;
            if (!vehicleValidator.RestoreState(snapshot.vehicles))
                return Fail(error, "invalid vehicle snapshot");
            RacingRaceManager raceValidator;
            if (!raceValidator.RestoreState(snapshot.race))
                return Fail(error, "invalid race snapshot");

            std::unordered_set<uint32_t> vehicleIds;
            for (const VehicleInstance& vehicle : snapshot.vehicles)
                vehicleIds.insert(vehicle.id);
            for (const RacerState& racer : snapshot.race.racers)
            {
                if (!vehicleIds.contains(racer.vehicleId))
                    return Fail(error, "race references a missing vehicle");
            }
            for (const ChampionshipEntry& entry : snapshot.race.championship)
            {
                if (!vehicleIds.contains(entry.vehicleId))
                    return Fail(error, "championship references a missing vehicle");
            }

            error.clear();
            return true;
        }

        static std::string Serialize(const RacingPersistenceSnapshot& snapshot)
        {
            std::string error;
            return Serialize(snapshot, error);
        }

        static std::string Serialize(const RacingPersistenceSnapshot& snapshot, std::string& error)
        {
            if (!Validate(snapshot, error))
                return {};

            std::ostringstream out;
            out << std::setprecision(std::numeric_limits<float>::max_digits10);
            out << "SPARK_RACING_STATE_V1\nTRACK " << snapshot.trackId << "\nDIFFICULTY "
                << static_cast<unsigned>(snapshot.difficulty) << "\nVEHICLES " << snapshot.vehicles.size() << '\n';
            for (const VehicleInstance& vehicle : snapshot.vehicles)
            {
                const VehicleStats& stats = vehicle.baseStats;
                out << "V " << vehicle.id << ' ' << std::quoted(vehicle.name) << ' '
                    << static_cast<unsigned>(vehicle.type) << ' ' << stats.maxSpeed << ' ' << stats.acceleration << ' '
                    << stats.handling << ' ' << stats.braking << ' ' << stats.weight << ' ' << stats.durability << ' '
                    << stats.driftBonus << ' ' << vehicle.positionX << ' ' << vehicle.positionY << ' '
                    << vehicle.positionZ << ' ' << vehicle.heading << ' ' << vehicle.speed << ' ' << vehicle.rpm << ' '
                    << vehicle.steerAngle << ' ' << static_cast<unsigned>(vehicle.driftState) << ' '
                    << vehicle.driftCharge << ' ' << vehicle.boostTimer << ' ' << vehicle.nitro << ' ' << vehicle.damage
                    << ' ' << static_cast<unsigned>(vehicle.currentSurface) << ' ' << (vehicle.isPlayer ? 1 : 0) << ' '
                    << (vehicle.isActive ? 1 : 0) << '\n';
            }

            const RacingRaceSnapshot& race = snapshot.race;
            out << "RACE " << static_cast<unsigned>(race.state) << ' ' << static_cast<unsigned>(race.mode) << ' '
                << race.countdownTimer << ' ' << race.raceTime << ' ' << race.totalLaps << ' ' << race.dnfTimeout
                << "\nRACERS " << race.racers.size() << '\n';
            for (const RacerState& racer : race.racers)
            {
                out << "R " << racer.vehicleId << ' ' << std::quoted(racer.name) << ' ' << (racer.isPlayer ? 1 : 0)
                    << ' ' << racer.currentLap << ' ' << racer.lastCheckpoint << ' ' << racer.distanceAlongTrack << ' '
                    << racer.position << ' ' << racer.totalTime << ' ' << racer.currentLapTime << ' '
                    << racer.bestLapTime << ' ' << (racer.finished ? 1 : 0) << ' ' << (racer.dnf ? 1 : 0) << ' '
                    << racer.finishTime << ' ' << racer.championshipPoints << ' ' << racer.splitTimes.size() << ' '
                    << racer.lapTimes.size() << '\n';
                out << "S";
                for (float split : racer.splitTimes)
                    out << ' ' << split;
                out << "\nL";
                for (float lap : racer.lapTimes)
                    out << ' ' << lap;
                out << '\n';
            }

            out << "CHAMPIONSHIP " << race.championship.size() << '\n';
            for (const ChampionshipEntry& entry : race.championship)
                out << "C " << entry.vehicleId << ' ' << std::quoted(entry.name) << ' ' << entry.totalPoints << ' '
                    << entry.wins << '\n';
            out << "END\n";
            error.clear();
            return out.str();
        }

        static bool Deserialize(std::string_view text, RacingPersistenceSnapshot& outSnapshot, std::string& error)
        {
            constexpr size_t MaxRacers = 256;
            std::istringstream in{std::string(text)};
            RacingPersistenceSnapshot parsed;
            std::string tag;
            unsigned difficulty = 0;
            size_t count = 0;
            if (!(in >> tag) || tag != "SPARK_RACING_STATE_V1" || !(in >> tag >> parsed.trackId) || tag != "TRACK" ||
                !(in >> tag >> difficulty) || tag != "DIFFICULTY" ||
                difficulty >= static_cast<unsigned>(AIDifficulty::Count) || !(in >> tag >> count) ||
                tag != "VEHICLES" || count > MaxRacers)
            {
                return Fail(error, "invalid racing snapshot header");
            }
            parsed.difficulty = static_cast<AIDifficulty>(difficulty);
            parsed.vehicles.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                VehicleInstance vehicle;
                unsigned type = 0, drift = 0, surface = 0, isPlayer = 0, isActive = 0;
                VehicleStats& stats = vehicle.baseStats;
                if (!(in >> tag >> vehicle.id >> std::quoted(vehicle.name) >> type >> stats.maxSpeed >>
                      stats.acceleration >> stats.handling >> stats.braking >> stats.weight >> stats.durability >>
                      stats.driftBonus >> vehicle.positionX >> vehicle.positionY >> vehicle.positionZ >>
                      vehicle.heading >> vehicle.speed >> vehicle.rpm >> vehicle.steerAngle >> drift >>
                      vehicle.driftCharge >> vehicle.boostTimer >> vehicle.nitro >> vehicle.damage >> surface >>
                      isPlayer >> isActive) ||
                    tag != "V" || type >= static_cast<unsigned>(VehicleType::Count) ||
                    drift >= static_cast<unsigned>(DriftState::Count) ||
                    surface >= static_cast<unsigned>(SurfaceType::Count) || isPlayer > 1 || isActive > 1)
                {
                    return Fail(error, "malformed vehicle record");
                }
                vehicle.type = static_cast<VehicleType>(type);
                vehicle.driftState = static_cast<DriftState>(drift);
                vehicle.currentSurface = static_cast<SurfaceType>(surface);
                vehicle.isPlayer = isPlayer != 0;
                vehicle.isActive = isActive != 0;
                parsed.vehicles.push_back(std::move(vehicle));
            }

            unsigned state = 0, mode = 0;
            RacingRaceSnapshot& race = parsed.race;
            if (!(in >> tag >> state >> mode >> race.countdownTimer >> race.raceTime >> race.totalLaps >>
                  race.dnfTimeout) ||
                tag != "RACE" || state >= static_cast<unsigned>(RaceState::Count) ||
                mode >= static_cast<unsigned>(RaceMode::Count) || !(in >> tag >> count) || tag != "RACERS" ||
                count > MaxRacers)
            {
                return Fail(error, "malformed race header");
            }
            race.state = static_cast<RaceState>(state);
            race.mode = static_cast<RaceMode>(mode);
            race.racers.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                RacerState racer;
                unsigned isPlayer = 0, finished = 0, dnf = 0;
                size_t splitCount = 0, lapCount = 0;
                if (!(in >> tag >> racer.vehicleId >> std::quoted(racer.name) >> isPlayer >> racer.currentLap >>
                      racer.lastCheckpoint >> racer.distanceAlongTrack >> racer.position >> racer.totalTime >>
                      racer.currentLapTime >> racer.bestLapTime >> finished >> dnf >> racer.finishTime >>
                      racer.championshipPoints >> splitCount >> lapCount) ||
                    tag != "R" || isPlayer > 1 || finished > 1 || dnf > 1 || splitCount > 10000 || lapCount > 100)
                {
                    return Fail(error, "malformed racer record");
                }
                racer.isPlayer = isPlayer != 0;
                racer.finished = finished != 0;
                racer.dnf = dnf != 0;
                if (!(in >> tag) || tag != "S")
                    return Fail(error, "missing racer split list");
                racer.splitTimes.reserve(splitCount);
                for (size_t splitIndex = 0; splitIndex < splitCount; ++splitIndex)
                {
                    float split = 0.0f;
                    if (!(in >> split))
                        return Fail(error, "truncated racer split list");
                    racer.splitTimes.push_back(split);
                }
                if (!(in >> tag) || tag != "L")
                    return Fail(error, "missing racer lap list");
                racer.lapTimes.reserve(lapCount);
                for (size_t lapIndex = 0; lapIndex < lapCount; ++lapIndex)
                {
                    float lap = 0.0f;
                    if (!(in >> lap))
                        return Fail(error, "truncated racer lap list");
                    racer.lapTimes.push_back(lap);
                }
                race.racers.push_back(std::move(racer));
            }

            if (!(in >> tag >> count) || tag != "CHAMPIONSHIP" || count > MaxRacers)
                return Fail(error, "malformed championship section");
            race.championship.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                ChampionshipEntry entry;
                if (!(in >> tag >> entry.vehicleId >> std::quoted(entry.name) >> entry.totalPoints >> entry.wins) ||
                    tag != "C")
                    return Fail(error, "malformed championship record");
                race.championship.push_back(std::move(entry));
            }

            if (!(in >> tag) || tag != "END")
                return Fail(error, "missing racing snapshot terminator");
            in >> std::ws;
            if (!in.eof())
                return Fail(error, "unexpected trailing racing snapshot data");
            if (!Validate(parsed, error))
                return false;
            outSnapshot = std::move(parsed);
            error.clear();
            return true;
        }

        static bool Apply(const RacingPersistenceSnapshot& snapshot, RacingTrackSystem& trackSystem,
                          RacingVehicleSystem& vehicleSystem, RacingRaceManager& raceManager, RacingAIDriver& aiDriver,
                          std::string& error)
        {
            if (!Validate(snapshot, error) || snapshot.trackId > trackSystem.GetTrackCount())
                return snapshot.trackId > trackSystem.GetTrackCount() ? Fail(error, "saved track is unavailable")
                                                                      : false;

            const RacingPersistenceSnapshot previous = Capture(trackSystem, vehicleSystem, raceManager, aiDriver);
            trackSystem.LoadDemoTrack(snapshot.trackId - 1);
            if (!vehicleSystem.RestoreState(snapshot.vehicles) || !raceManager.RestoreState(snapshot.race))
            {
                trackSystem.LoadDemoTrack(previous.trackId - 1);
                vehicleSystem.RestoreState(previous.vehicles);
                raceManager.RestoreState(previous.race);
                aiDriver.SetGlobalDifficulty(previous.difficulty);
                return Fail(error, "racing systems rejected a validated snapshot");
            }
            aiDriver.SetGlobalDifficulty(snapshot.difficulty);
            error.clear();
            return true;
        }

      private:
        static bool Fail(std::string& error, std::string message)
        {
            error = std::move(message);
            return false;
        }
    };
} // namespace Racing
