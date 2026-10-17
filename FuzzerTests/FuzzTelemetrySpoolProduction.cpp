/**
 * @file FuzzTelemetrySpoolProduction.cpp
 * @brief libc++-compiled production adapter for the telemetry spool libFuzzer harness.
 *
 * The spool is read back from disk after a crash or restart, so its bytes are
 * untrusted. The fuzz input is handed to the shipped TelemetryDetail::Parse,
 * once with the absolute event cap and once with a small caller cap, and the
 * result is checked against the guarantees the telemetry flush path relies on.
 * A violation aborts so libFuzzer records a crash rather than a silent pass:
 *  - the event count never exceeds the caller's maximumEvents,
 *  - a small cap accepts exactly the spools the absolute cap accepts that fit
 *    under it (the cap is a bound, not a different grammar),
 *  - a rejected spool leaves the caller's event vector untouched,
 *  - every accepted event has a valid serialized size, sequences strictly
 *    increase, and the spool re-serializes through Serialize to the same
 *    length and re-parses to field-equal events.
 */

#include "FuzzTelemetrySpoolProduction.h"

#include "Utils/Telemetry.h"
#include "Utils/TelemetrySpoolInternal.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;
    constexpr std::uint32_t kSmallEventCap = 4;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzTelemetrySpool: TelemetryDetail::Parse violated: %s\n", what);
        std::abort();
    }

    std::vector<Spark::TelemetryEvent> MakeSentinel()
    {
        Spark::TelemetryEvent event;
        event.name = "sentinel";
        event.sessionId = "sentinel-session";
        event.sequence = 7;
        event.timestamp = 11;
        event.properties.emplace("sentinel-key", "sentinel-value");
        return {event};
    }

    bool SameEvent(const Spark::TelemetryEvent& a, const Spark::TelemetryEvent& b)
    {
        return a.name == b.name && a.timestamp == b.timestamp && a.properties == b.properties &&
               a.sessionId == b.sessionId && a.sequence == b.sequence;
    }

    bool SameEvents(const std::vector<Spark::TelemetryEvent>& a, const std::vector<Spark::TelemetryEvent>& b)
    {
        if (a.size() != b.size())
            return false;
        for (std::size_t index = 0; index < a.size(); ++index)
        {
            if (!SameEvent(a[index], b[index]))
                return false;
        }
        return true;
    }

    /// Parse with @p cap; enforce the cap and the publish-on-success contract.
    bool ParseChecked(const std::vector<std::uint8_t>& bytes, std::uint32_t cap,
                      std::vector<Spark::TelemetryEvent>& events)
    {
        events = MakeSentinel();
        const bool accepted = Spark::TelemetryDetail::Parse(bytes, cap, events);
        if (!accepted)
        {
            if (!SameEvents(events, MakeSentinel()))
                InvariantFailure("a rejected spool modified the caller's events");
            return false;
        }
        if (events.empty() || events.size() > cap)
            InvariantFailure("accepted event count is outside [1, maximumEvents]");
        return true;
    }

    void CheckAccepted(const std::vector<std::uint8_t>& input, const std::vector<Spark::TelemetryEvent>& events)
    {
        std::uint64_t previousSequence = 0;
        for (const Spark::TelemetryEvent& event : events)
        {
            std::size_t serializedSize = 0;
            if (!Spark::TelemetryDetail::EventSerializedSize(event, serializedSize))
                InvariantFailure("an accepted event has no valid serialized size");
            if (event.sequence <= previousSequence)
                InvariantFailure("accepted sequences do not strictly increase");
            previousSequence = event.sequence;
        }

        std::vector<std::uint8_t> reserialized;
        if (!Spark::TelemetryDetail::Serialize(events, input.size(), reserialized))
            InvariantFailure("an accepted spool does not re-serialize within its own size");
        // Properties are stored unordered and written sorted, so only the length
        // (not the byte order) of the re-serialization is fixed by the input.
        if (reserialized.size() != input.size())
            InvariantFailure("re-serialization length differs from the accepted spool");

        std::vector<Spark::TelemetryEvent> reparsed;
        if (!ParseChecked(reserialized, Spark::TelemetryDetail::kAbsoluteMaxEvents, reparsed) ||
            !SameEvents(reparsed, events))
        {
            InvariantFailure("re-serialized spool does not re-parse to the same events");
        }
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production decoder.
extern "C" int SparkFuzzParseTelemetrySpool(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    const std::vector<std::uint8_t> input =
        size == 0 ? std::vector<std::uint8_t>() : std::vector<std::uint8_t>(data, data + size);

    std::vector<Spark::TelemetryEvent> events;
    const bool accepted = ParseChecked(input, Spark::TelemetryDetail::kAbsoluteMaxEvents, events);

    std::vector<Spark::TelemetryEvent> capped;
    const bool acceptedUnderSmallCap = ParseChecked(input, kSmallEventCap, capped);
    if (acceptedUnderSmallCap != (accepted && events.size() <= kSmallEventCap))
        InvariantFailure("the small event cap changed which spools are accepted");
    if (acceptedUnderSmallCap && !SameEvents(capped, events))
        InvariantFailure("the small event cap changed the decoded events");

    if (accepted)
        CheckAccepted(input, events);
    return 0;
}
