#pragma once

/**
 * @file MMOSessionGateProtocol.h
 * @brief Bounded binary protocol at the SparkGameMMO session gate boundary.
 *
 * The codec is synchronous and allocation-free while decoding. Callers own the
 * packet and its credential storage; password bytes must be erased by the
 * caller immediately after authentication (Decode deliberately does not retain
 * any input storage). The wire format is little-endian and versioned so a
 * session gate can reject incompatible peers before dispatching gameplay work.
 */

#include <array>
#include <cstddef>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace MMO::SessionGateWire
{
    enum class Operation : uint8_t
    {
        Register = 1,
        Login,
        CreateCharacter,
        EnterWorld,
        Move,
        Interact,
        State
    };

    enum class Status : uint8_t
    {
        Ok = 0,
        Rejected,
        Unauthenticated,
        NotOwner,
        Invalid,
        RateLimited
    };

    constexpr size_t MaxPacketBytes = 512;
    constexpr uint8_t CurrentVersion = 1;

    struct Packet
    {
        Operation operation = Operation::Register;
        Status status = Status::Ok;
        bool response = false;
        uint32_t requestId = 0;
        uint32_t accountId = 0;
        uint32_t characterId = 0;
        uint32_t targetId = 0;
        uint32_t areaId = 0;
        uint32_t interactionCount = 0;
        float x = 0.0F;
        float y = 0.0F;
        float z = 0.0F;
        float health = 0.0F;
        std::array<char, 33> username{};
        std::array<char, 129> password{};
        std::array<char, 17> name{};
        uint8_t race = 0;
        uint8_t classId = 0;
    };

    /** @brief Decode one complete session-gate packet without allocating. */
    bool Decode(std::span<const uint8_t> bytes, Packet& packet) noexcept;

    /**
     * @brief Encode one canonical packet, returning an empty vector for invalid input.
     *
     * Register/Login credentials are copied into the returned buffer. The
     * caller must erase its Packet::password after the authentication attempt.
     */
    std::vector<uint8_t> Encode(const Packet& packet);
} // namespace MMO::SessionGateWire
