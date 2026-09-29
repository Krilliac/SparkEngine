/**
 * @file AreaHandoffParticipant.h
 * @brief Engine-owned seam for fenced cross-area entity handoff participants.
 */
#pragma once

#include "AreaServerTypes.h"

#include <cstdint>
#include <string>

#ifdef ENABLE_NETWORKING

namespace Spark::Net
{
    enum class HandoffPhase : uint8_t
    {
        Prepare = 1,
        Transfer = 2,
        Commit = 3,
        Acknowledge = 4,
        Abort = 5
    };

    enum class HandoffResult : uint8_t
    {
        Applied,
        Duplicate,
        Rejected,
        Unavailable
    };

    /**
     * @brief Immutable input delivered to a game-owned handoff participant.
     *
     * Calls occur on the server game thread through AreaHandoffDispatcher.
     * The participant is borrowed and must detach before module unload.
     * Implementations own their persistent reservation and entity checkpoint;
     * no gameplay payload or credentials are carried by the control command.
     */
    struct HandoffRequest
    {
        std::string sessionId;
        uint64_t epoch = 0;
        AreaID sourceArea = INVALID_AREA;
        AreaID targetArea = INVALID_AREA;
    };

    /**
     * @brief Game/module seam for the fenced SparkGateway/SparkServer handoff.
     *
     * The engine validates phase ordering and idempotency before invoking this
     * interface. A participant must make each accepted phase durable before
     * returning Applied; retries of an already accepted phase return Duplicate.
     */
    class IAreaHandoffParticipant
    {
      public:
        virtual ~IAreaHandoffParticipant() = default;

        [[nodiscard]] virtual HandoffResult Prepare(const HandoffRequest& request) = 0;
        [[nodiscard]] virtual HandoffResult Transfer(const HandoffRequest& request) = 0;
        [[nodiscard]] virtual HandoffResult Commit(const HandoffRequest& request) = 0;
        [[nodiscard]] virtual HandoffResult Acknowledge(const HandoffRequest& request) = 0;
        [[nodiscard]] virtual HandoffResult Abort(const HandoffRequest& request) = 0;
    };
} // namespace Spark::Net

#endif // ENABLE_NETWORKING
