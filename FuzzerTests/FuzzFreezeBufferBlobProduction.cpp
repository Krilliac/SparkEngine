/**
 * @file FuzzFreezeBufferBlobProduction.cpp
 * @brief libc++-compiled production adapter for the freeze/thaw blob libFuzzer harness.
 *
 * FreezeSystem.h is header-only, so the adapter compiles the production
 * FreezeSystem and FreezeBuffer templates directly. Two passes run per input:
 *
 *  1. FreezeSystem::ThawAll over two fixture subsystems that use every read
 *     primitive a subsystem uses (Freeze<T>, FreezeString, FreezeTag,
 *     FreezeLegacy<T>). When the blob is accepted, the thawed state is frozen
 *     again, thawed again and compared: a reader that accepts a blob but
 *     restores different state from its own output aborts.
 *  2. A raw FreezeBuffer read script whose operations are chosen by the input
 *     itself, so the primitives are exercised with attacker-shaped sizes
 *     outside the subsystem framing. After every operation the read cursor
 *     must stay inside the buffer, and a failed operation must be recorded in
 *     the buffer's error or tag-mismatch state.
 *
 * Thread affinity: the libFuzzer driver calls this on one thread. State: the
 * fixture subsystems and the FreezeSystem singleton registration live for the
 * process; each call resets the fixtures first so runs are independent.
 */

#include "FuzzFreezeBufferBlobProduction.h"

// FreezeSystem.h uses std::min and std::is_trivially_copyable_v without
// including their headers, so supply them first.
#include <algorithm>
#include <type_traits>

#include "Engine/SaveSystem/FreezeSystem.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;
    constexpr std::size_t kMaxScriptOperations = 256;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzFreezeBufferBlob: FreezeBuffer violated: %s\n", what);
        std::abort();
    }

    struct LegacyRecord
    {
        std::uint32_t id = 0;
        float weight = 0.0f;
        std::uint64_t stamp = 0;
    };
    static_assert(std::is_trivially_copyable_v<LegacyRecord>);

    /// Plain value, string and nested-tag state: the common subsystem shape.
    class CounterSubsystem final : public Spark::IFreezable
    {
      public:
        const char* GetFreezeTag() const override { return "Counter"; }

        bool Freeze(Spark::FreezeBuffer& buffer) override
        {
            return buffer.Freeze(m_count) && buffer.FreezeString(m_label) && buffer.FreezeTag("counter-inner");
        }

        void Reset()
        {
            m_count = 7;
            m_label = "seed";
        }

        std::uint32_t m_count = 7;
        std::string m_label = "seed";
    };

    /// A struct that grew between versions: the saved size is stored before the record.
    class LegacySubsystem final : public Spark::IFreezable
    {
      public:
        const char* GetFreezeTag() const override { return "Legacy"; }

        bool Freeze(Spark::FreezeBuffer& buffer) override
        {
            std::uint32_t savedSize = static_cast<std::uint32_t>(sizeof(LegacyRecord));
            return buffer.Freeze(savedSize) && buffer.FreezeLegacy(m_record, savedSize);
        }

        void Reset() { m_record = LegacyRecord{}; }

        LegacyRecord m_record;
    };

    struct Fixtures
    {
        CounterSubsystem counter;
        LegacySubsystem legacy;

        Fixtures()
        {
            Spark::FreezeSystem& system = Spark::FreezeSystem::GetInstance();
            system.Initialize();
            system.RegisterSubsystem(&counter);
            system.RegisterSubsystem(&legacy);
        }

        void Reset()
        {
            counter.Reset();
            legacy.Reset();
        }
    };

    /// Value copy of the fixtures' restorable state.
    struct Snapshot
    {
        std::uint32_t count = 0;
        std::string label;
        LegacyRecord record;
    };

    Snapshot TakeSnapshot(const Fixtures& fixtures)
    {
        return Snapshot{fixtures.counter.m_count, fixtures.counter.m_label, fixtures.legacy.m_record};
    }

    bool SameState(const Snapshot& a, const Snapshot& b)
    {
        return a.count == b.count && a.label == b.label &&
               std::memcmp(&a.record, &b.record, sizeof(LegacyRecord)) == 0;
    }

    void ThawThroughFramework(Fixtures& fixtures, const std::vector<std::uint8_t>& blob)
    {
        Spark::FreezeSystem& system = Spark::FreezeSystem::GetInstance();
        fixtures.Reset();
        if (!system.ThawAll(blob))
            return;

        const Snapshot thawed = TakeSnapshot(fixtures);
        const std::vector<std::uint8_t> reFrozen = system.FreezeAll();
        fixtures.Reset();
        if (!system.ThawAll(reFrozen))
            InvariantFailure("an accepted blob's own FreezeAll output is rejected");
        if (!SameState(thawed, TakeSnapshot(fixtures)))
            InvariantFailure("freeze/thaw of accepted state does not restore the same state");
    }

    void CheckCursor(const Spark::FreezeBuffer& reader)
    {
        if (reader.GetReadPosition() > reader.GetSize())
            InvariantFailure("the read cursor passed the end of the buffer");
    }

    void RunReadScript(const std::vector<std::uint8_t>& blob)
    {
        Spark::FreezeBuffer reader(Spark::FreezeBuffer::Mode::Read, blob);
        for (std::size_t operation = 0; operation < kMaxScriptOperations && reader.GetReadPosition() < reader.GetSize();
             ++operation)
        {
            const std::uint8_t selector = reader.GetData()[reader.GetReadPosition()];
            bool accepted = false;
            switch ((selector + operation) % 5)
            {
            case 0:
            {
                std::uint64_t value = 0;
                accepted = reader.Freeze(value);
                break;
            }
            case 1:
            {
                std::string text;
                accepted = reader.FreezeString(text);
                if (accepted && text.size() > reader.GetSize())
                    InvariantFailure("a decoded string is longer than the whole buffer");
                break;
            }
            case 2:
                accepted = reader.FreezeTag("Counter");
                break;
            case 3:
            {
                LegacyRecord record;
                // Spread the saved size over small, exact and oversized values.
                const std::size_t savedSize = static_cast<std::size_t>(selector) << (selector & 7u);
                accepted = reader.FreezeLegacy(record, savedSize);
                break;
            }
            default:
            {
                std::uint8_t value = 0;
                accepted = reader.Freeze(value);
                break;
            }
            }
            CheckCursor(reader);
            if (!accepted)
            {
                if (!reader.HasError() && !reader.HasTagMismatch())
                    InvariantFailure("a rejected operation left no error state");
                return;
            }
        }
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production header.
extern "C" int SparkFuzzThawFreezeBlob(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    static Fixtures fixtures;
    const std::vector<std::uint8_t> blob(data, data + size);
    ThawThroughFramework(fixtures, blob);
    RunReadScript(blob);
    return 0;
}
