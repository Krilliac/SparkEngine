/**
 * @file FuzzRTSPersistenceProduction.cpp
 * @brief libc++-compiled production adapter for the SparkGameRTS save snapshot harness.
 *
 * SparkGameRTS restores a skirmish from the "SparkGameRTS.match.v2" custom state of a
 * save slot read from disk through RTSPersistence::Deserialize, then hands the
 * snapshot to Apply. The input is that custom-state text. A violated contract aborts
 * so libFuzzer records a crash:
 *  - a rejected snapshot reports an error and leaves the caller's snapshot untouched,
 *  - an accepted snapshot passes RTSPersistence::Validate (the check Apply and every
 *    restoring system rely on),
 *  - an accepted snapshot serializes, and Serialize -> Deserialize -> Serialize is a
 *    fixed point, so a load -> save cycle never changes the skirmish.
 */

#include "FuzzRTSPersistenceProduction.h"

#include "Core/RTSPersistence.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace
{
    constexpr std::size_t kMaxInputBytes = 256u * 1024u;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzRTSPersistence: %s\n", what);
        std::abort();
    }

    /// A small valid snapshot the decoder must keep when it rejects an input.
    RTS::RTSPersistenceSnapshot MakeSentinel()
    {
        RTS::RTSPersistenceSnapshot sentinel;
        sentinel.tick = 4242;
        sentinel.nextUnitId = 9;
        sentinel.selection = {3, 5};
        RTS::FogGrid grid;
        grid.width = 2;
        grid.height = 1;
        grid.cells = {RTS::RTSVisibility::Fog, RTS::RTSVisibility::Visible};
        sentinel.fog.assign(static_cast<std::size_t>(RTS::RTSFaction::Count), grid);
        return sentinel;
    }
} // namespace

extern "C" int SparkFuzzDeserializeRTSSnapshot(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    const std::string sentinelText = RTS::RTSPersistence::Serialize(MakeSentinel());
    if (sentinelText.empty())
    {
        InvariantFailure("the sentinel snapshot does not validate");
    }
    RTS::RTSPersistenceSnapshot snapshot = MakeSentinel();
    std::string error;
    if (!RTS::RTSPersistence::Deserialize(text, snapshot, error))
    {
        if (error.empty())
        {
            InvariantFailure("a rejected snapshot reported no error");
        }
        if (RTS::RTSPersistence::Serialize(snapshot) != sentinelText)
        {
            InvariantFailure("a rejected snapshot modified the caller's snapshot");
        }
        return 0;
    }

    std::string validationError;
    if (!RTS::RTSPersistence::Validate(snapshot, validationError))
    {
        InvariantFailure("an accepted snapshot does not validate");
    }
    const std::string first = RTS::RTSPersistence::Serialize(snapshot);
    if (first.empty())
    {
        InvariantFailure("an accepted snapshot cannot be saved again");
    }
    RTS::RTSPersistenceSnapshot reloaded = MakeSentinel();
    if (!RTS::RTSPersistence::Deserialize(first, reloaded, error))
    {
        InvariantFailure("the serializer wrote a snapshot the decoder rejects");
    }
    if (RTS::RTSPersistence::Serialize(reloaded) != first)
    {
        InvariantFailure("Serialize -> Deserialize -> Serialize changed the snapshot");
    }
    return 0;
}
