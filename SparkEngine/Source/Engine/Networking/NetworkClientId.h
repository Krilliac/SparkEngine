/**
 * @file NetworkClientId.h
 * @brief Shared client identifier types and generated-ID reservation policy.
 */
#pragma once

#include <cstdint>

namespace Spark::Net
{
    using ClientID = uint32_t;

    inline constexpr ClientID INVALID_CLIENT = 0;
    inline constexpr ClientID FIRST_RESERVED_CLIENT_ID = 0xFFFFFF00u;
    inline constexpr ClientID LAST_GENERATED_CLIENT_ID = FIRST_RESERVED_CLIENT_ID - 1u;

    /** IDs outside the generated range are reserved for invalid/application sentinels. */
    [[nodiscard]] constexpr bool IsReservedClientID(ClientID client) noexcept
    {
        return client == INVALID_CLIENT || client >= FIRST_RESERVED_CLIENT_ID;
    }

    /** Normalize an externally seeded/wrapped candidate into the generated range. */
    [[nodiscard]] constexpr ClientID NormalizeGeneratedClientID(ClientID candidate) noexcept
    {
        return IsReservedClientID(candidate) ? ClientID{1} : candidate;
    }

    /** Advance without ever entering the invalid or application-reserved ranges. */
    [[nodiscard]] constexpr ClientID AdvanceGeneratedClientID(ClientID current) noexcept
    {
        return current >= LAST_GENERATED_CLIENT_ID ? ClientID{1} : current + 1u;
    }
} // namespace Spark::Net
