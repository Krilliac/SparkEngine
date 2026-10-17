/**
 * @file FuzzAchievementProgressProduction.cpp
 * @brief libc++-compiled production adapter for the achievement progress libFuzzer harness.
 *
 * Games persist achievement progress by putting AchievementSystem::SaveToWriter bytes in
 * their save payload and handing them back to AchievementSystem::LoadFromReader
 * (Engine/Gameplay/AchievementSystem.h) after registering their definitions, so the reader
 * sees whatever a save file holds. The adapter registers a fixed set of definitions, gives
 * some of them progress (one is unlocked), then loads the fuzz bytes. A violated contract
 * aborts so libFuzzer records a crash:
 *  - the resulting state equals an independent model of the format: a u32 count, then
 *    records of [id u32][progress f32][unlocked u8][timestamp u64]; the load stops at the
 *    first incomplete record, skips unregistered ids and non-finite progress, and clamps
 *    finite progress into [0, target]; an applied record is marked notified exactly when it
 *    is unlocked,
 *  - no id that is not registered gains a progress entry,
 *  - every progress value is finite and inside [0, target],
 *  - loading never fires an unlock callback,
 *  - SaveToWriter output loads into an identically prepared system as the same state.
 */

#include "FuzzAchievementProgressProduction.h"

#include "Engine/Gameplay/AchievementSystem.h"
#include "Utils/Serializer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;
    constexpr std::size_t kRecordBytes = 4u + 4u + 1u + 8u;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzAchievementProgress: AchievementSystem::LoadFromReader violated: %s\n", what);
        std::abort();
    }

    using Spark::Gameplay::AchievementDefinition;
    using Spark::Gameplay::AchievementProgress;
    using Spark::Gameplay::AchievementSystem;
    using Spark::Gameplay::AchievementType;

    struct Fixture
    {
        std::uint32_t id;
        float target;
        AchievementType type;
    };

    // Ids and targets the fuzz records can hit: a zero target, a fractional one and the
    // largest id.
    constexpr std::array<Fixture, 7> kFixtures = {{
        {1u, 1.0f, AchievementType::Binary},
        {2u, 10.0f, AchievementType::Progressive},
        {3u, 100.5f, AchievementType::Tiered},
        {5u, 0.0f, AchievementType::Hidden},
        {8u, 3.0f, AchievementType::Cumulative},
        {13u, 2.5f, AchievementType::Progressive},
        {0xFFFFFFFFu, 1.0f, AchievementType::Binary},
    }};

    bool IsRegistered(std::uint32_t id)
    {
        return std::any_of(kFixtures.begin(), kFixtures.end(), [id](const Fixture& f) { return f.id == id; });
    }

    float TargetOf(std::uint32_t id)
    {
        for (const Fixture& fixture : kFixtures)
        {
            if (fixture.id == id)
            {
                return fixture.target;
            }
        }
        InvariantFailure("asked for the target of an unregistered id");
    }

    /// Register the definitions and the starting progress; the unlock callback that aborts is
    /// added afterwards, so only LoadFromReader can trip it.
    void Prepare(AchievementSystem& system)
    {
        system.Initialize();
        for (const Fixture& fixture : kFixtures)
        {
            AchievementDefinition def;
            def.id = fixture.id;
            def.name = "fixture";
            def.type = fixture.type;
            def.targetValue = fixture.target;
            def.pointValue = 5;
            system.RegisterAchievement(def);
        }
        system.UpdateProgress(2, 4.0f);
        system.UnlockAchievement(3);
        system.IncrementProgress(8, 1.0f);
        system.OnAchievementUnlocked([](const Spark::Gameplay::AchievementUnlockEvent&)
                                     { InvariantFailure("loading progress fired an unlock callback"); });
    }

    using State = std::map<std::uint32_t, AchievementProgress>;

    State Snapshot(const AchievementSystem& system)
    {
        State state;
        for (const Fixture& fixture : kFixtures)
        {
            const AchievementProgress* progress = system.GetProgress(fixture.id);
            if (progress == nullptr)
            {
                InvariantFailure("a registered achievement lost its progress entry");
            }
            state[fixture.id] = *progress;
        }
        return state;
    }

    bool SameProgress(const AchievementProgress& a, const AchievementProgress& b)
    {
        return a.achievementId == b.achievementId && a.unlocked == b.unlocked &&
               a.unlockTimestamp == b.unlockTimestamp && a.notified == b.notified &&
               std::memcmp(&a.currentValue, &b.currentValue, sizeof(float)) == 0;
    }

    bool SameState(const State& a, const State& b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (const auto& [id, progress] : a)
        {
            const auto it = b.find(id);
            if (it == b.end() || !SameProgress(progress, it->second))
            {
                return false;
            }
        }
        return true;
    }

    template <typename T> T ReadLittleEndian(const std::uint8_t* bytes)
    {
        T value{};
        std::memcpy(&value, bytes, sizeof(T)); // the fuzz host is little-endian, like the format
        return value;
    }

    /// Independent model of LoadFromReader's documented contract.
    State Model(State state, const std::uint8_t* data, std::size_t size, std::vector<std::uint32_t>& seenIds)
    {
        if (size < 4)
        {
            return state;
        }
        const auto count = ReadLittleEndian<std::uint32_t>(data);
        std::size_t offset = 4;
        for (std::uint32_t i = 0; i < count && size - offset >= kRecordBytes; ++i, offset += kRecordBytes)
        {
            const std::uint8_t* record = data + offset;
            const auto id = ReadLittleEndian<std::uint32_t>(record);
            const auto value = ReadLittleEndian<float>(record + 4);
            const bool unlocked = record[8] != 0;
            const auto timestamp = ReadLittleEndian<std::uint64_t>(record + 9);
            seenIds.push_back(id);
            if (!std::isfinite(value) || !IsRegistered(id))
            {
                continue;
            }
            AchievementProgress& progress = state[id];
            progress.currentValue = std::clamp(value, 0.0f, std::max(0.0f, TargetOf(id)));
            progress.unlocked = unlocked;
            progress.unlockTimestamp = timestamp;
            progress.notified = unlocked;
        }
        return state;
    }
} // namespace

extern "C" int SparkFuzzLoadAchievementProgress(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    static const std::uint8_t kNoBytes = 0;
    const std::uint8_t* bytes = size == 0 ? &kNoBytes : data;

    AchievementSystem& system = AchievementSystem::GetInstance();
    Prepare(system);
    const State before = Snapshot(system);
    std::vector<std::uint32_t> seenIds;
    const State expected = Model(before, bytes, size, seenIds);

    Spark::BinaryReader reader(bytes, size);
    system.LoadFromReader(reader);
    const State loaded = Snapshot(system);

    if (!SameState(loaded, expected))
    {
        InvariantFailure("the loaded progress differs from the format model");
    }
    for (const std::uint32_t id : seenIds)
    {
        if (!IsRegistered(id) && system.GetProgress(id) != nullptr)
        {
            InvariantFailure("a record for an unregistered id was inserted");
        }
    }
    for (const auto& [id, progress] : loaded)
    {
        if (!std::isfinite(progress.currentValue) || progress.currentValue < 0.0f ||
            progress.currentValue > std::max(0.0f, TargetOf(id)))
        {
            InvariantFailure("a progress value is not finite or lies outside [0, target]");
        }
    }

    Spark::BinaryWriter writer;
    system.SaveToWriter(writer);
    const std::vector<std::uint8_t> saved = writer.GetBuffer();
    Prepare(system);
    Spark::BinaryReader savedReader(saved);
    system.LoadFromReader(savedReader);
    if (!SameState(Snapshot(system), loaded))
    {
        InvariantFailure("SaveToWriter -> LoadFromReader changed the progress");
    }
    system.Shutdown();
    return 0;
}
