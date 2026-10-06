/**
 * @file FuzzMMOClientStateProduction.cpp
 * @brief libc++-compiled production adapter for the SparkGameMMO client state request libFuzzer harness.
 *
 * MMOWorldSetup::ApplyClientStateRequest runs MMO::DecodeClientStateRequest on
 * every client EntityStateUpdate the server receives. The fuzz input is handed
 * to it as the payload. A violation of the decoder's contract aborts so
 * libFuzzer records a crash:
 *  - an accepted request is exactly kClientStateRequestSize bytes,
 *  - every accepted component is finite and inside the documented bounds, re-checked
 *    here from the raw little-endian floats against the numeric limits rather than
 *    through the production predicates,
 *  - re-encoding the accepted request with the NetBuffer writers (the input's own
 *    network ID and a zero property count) reproduces the input byte for byte: the
 *    format has no slack a second decoder could read differently.
 */

#include "FuzzMMOClientStateProduction.h"

#include "Engine/Networking/NetworkManager.h"
#include "World/MMOClientStateCodec.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 1024;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzMMOClientState: client state request contract violated: %s\n", what);
        std::abort();
    }

    float WireFloat(const std::uint8_t* bytes)
    {
        const std::uint32_t bits = static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
                                   (static_cast<std::uint32_t>(bytes[2]) << 16) |
                                   (static_cast<std::uint32_t>(bytes[3]) << 24);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    bool Same(float decoded, float wire)
    {
        return std::memcmp(&decoded, &wire, sizeof(float)) == 0;
    }

    void CheckAccepted(std::span<const std::uint8_t> input, const MMO::ClientStateRequest& request)
    {
        if (input.size() != MMO::kClientStateRequestSize)
            InvariantFailure("a request that is not kClientStateRequestSize bytes was accepted");

        // Offsets: networkId 0, position 4, rotation 16, velocity 28, property count 40.
        const float wire[9] = {WireFloat(&input[4]),  WireFloat(&input[8]),  WireFloat(&input[12]),
                               WireFloat(&input[16]), WireFloat(&input[20]), WireFloat(&input[24]),
                               WireFloat(&input[28]), WireFloat(&input[32]), WireFloat(&input[36])};
        const float decoded[9] = {request.position.x, request.position.y, request.position.z,
                                  request.rotation.x, request.rotation.y, request.rotation.z,
                                  request.velocity.x, request.velocity.y, request.velocity.z};
        for (int i = 0; i < 9; ++i)
        {
            if (!Same(decoded[i], wire[i]))
                InvariantFailure("a decoded component differs from its wire bits");
            if (!std::isfinite(wire[i]))
                InvariantFailure("a non-finite component was accepted");
        }
        for (int i = 0; i < 3; ++i)
        {
            if (!(std::fabs(wire[i]) <= 1.0e6f))
                InvariantFailure("a coordinate past 1000 km was accepted");
            if (!(std::fabs(wire[3 + i]) <= 360.0f))
                InvariantFailure("a rotation past one turn was accepted");
        }
        const double speedSquared = static_cast<double>(wire[6]) * wire[6] + static_cast<double>(wire[7]) * wire[7] +
                                    static_cast<double>(wire[8]) * wire[8];
        // The production check sums in float; allow its rounding at the boundary, nothing more.
        if (!(speedSquared <= 100.0 * 100.0 * (1.0 + 1.0e-6)))
            InvariantFailure("a speed above 100 m/s was accepted");
        if (input[40] != 0 || input[41] != 0)
            InvariantFailure("a request with properties was accepted");

        Spark::Net::NetBuffer buffer;
        buffer.WriteUint32(static_cast<std::uint32_t>(input[0]) | (static_cast<std::uint32_t>(input[1]) << 8) |
                           (static_cast<std::uint32_t>(input[2]) << 16) | (static_cast<std::uint32_t>(input[3]) << 24));
        buffer.WriteVector3(request.position);
        buffer.WriteVector3(request.rotation);
        buffer.WriteVector3(request.velocity);
        buffer.WriteUint16(0);
        const std::vector<std::uint8_t>& encoded = buffer.GetData();
        if (encoded.size() != input.size() || std::memcmp(encoded.data(), input.data(), input.size()) != 0)
            InvariantFailure("re-encoding an accepted request does not reproduce the input");
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production decoder.
extern "C" int SparkFuzzDecodeMMOClientState(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    const std::span<const std::uint8_t> input(data, size);
    if (const std::optional<MMO::ClientStateRequest> request = MMO::DecodeClientStateRequest(input))
        CheckAccepted(input, *request);
    return 0;
}
