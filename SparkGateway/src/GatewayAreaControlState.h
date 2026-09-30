/**
 * @file GatewayAreaControlState.h
 * @brief Codec for the area-control epoch state file LocalAreaControlService persists.
 *
 * LocalAreaControlService::LoadState decodes the file named by
 * [GatewayControl] epoch_state_file with ParseAreaControlState before the service
 * accepts a handoff phase, and SaveState writes it with SerializeAreaControlState.
 * The codec is pure (no sockets, keys or threads) so the SEC-120 fuzz target
 * (FuzzerTests/FuzzGatewayAreaControlState.cpp) drives exactly the reader the
 * server runs. Thread affinity: none.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Spark::Gateway
{
    enum class AreaControlPhase : uint8_t
    {
        Prepare = 1,
        Transfer = 2,
        Commit = 3,
        Acknowledge = 4,
        Abort = 5,
        Probe = 6
    };

    /// Longest handoff session id the fence records.
    inline constexpr size_t kMaxAreaControlSessionIdBytes = 128;

    /// The last applied phase of one handoff session. Area ids are Spark::Net::AreaID values
    /// (0 is Spark::Net::INVALID_AREA and never stored).
    struct AreaControlSessionFence
    {
        uint64_t epoch = 0;
        AreaControlPhase phase = AreaControlPhase::Abort;
        uint32_t sourceArea = 0;
        uint32_t targetArea = 0;
    };

    using AreaControlSessions = std::unordered_map<std::string, AreaControlSessionFence>;

    /**
     * @brief Encode @p sessions as "v2" followed by one
     *        `<std::quoted session> <epoch> <phase> <source> <target>` line per session.
     */
    [[nodiscard]] std::string SerializeAreaControlState(const AreaControlSessions& sessions);

    /**
     * @brief Decode a state file. Whitespace-only text is an empty state.
     *
     * Every session is a non-empty id of at most kMaxAreaControlSessionIdBytes, recorded once,
     * with an epoch of at least 1, a phase from Prepare to Abort and non-zero areas. Every
     * number is plain unsigned decimal digits (no sign, no trailing text) within its type.
     * @return false on anything else; @p out is replaced only on success.
     */
    [[nodiscard]] bool ParseAreaControlState(std::string_view text, AreaControlSessions& out);
} // namespace Spark::Gateway
