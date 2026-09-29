#include "MMOSessionGateProtocol.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace MMO::SessionGateWire
{
    namespace
    {
        constexpr uint8_t Magic0 = 'M';
        constexpr uint8_t Magic1 = 'G';
        constexpr size_t HeaderBytes = 10;

        bool IsOperation(uint8_t value) noexcept
        {
            return value >= static_cast<uint8_t>(Operation::Register) &&
                   value <= static_cast<uint8_t>(Operation::State);
        }

        bool IsStatus(uint8_t value) noexcept
        {
            return value <= static_cast<uint8_t>(Status::RateLimited);
        }

        bool IsPrintable(const char value) noexcept
        {
            return static_cast<unsigned char>(value) >= 0x20U && static_cast<unsigned char>(value) <= 0x7EU;
        }

        template <size_t N> bool StringLength(const std::array<char, N>& value, size_t& length) noexcept
        {
            length = 0;
            while (length < N && value[length] != '\0')
            {
                if (!IsPrintable(value[length]))
                {
                    return false;
                }
                ++length;
            }
            for (size_t index = length; index < N; ++index)
            {
                if (value[index] != '\0')
                {
                    return false;
                }
            }
            return length + 1U <= N;
        }

        template <size_t N>
        bool ReadString(std::span<const uint8_t> bytes, size_t& cursor, std::array<char, N>& output) noexcept
        {
            if (cursor >= bytes.size())
            {
                return false;
            }
            const size_t length = bytes[cursor++];
            if (length + 1U > N || cursor + length > bytes.size())
            {
                return false;
            }
            for (size_t index = 0; index < length; ++index)
            {
                const char value = static_cast<char>(bytes[cursor + index]);
                if (!IsPrintable(value))
                {
                    return false;
                }
                output[index] = value;
            }
            output[length] = '\0';
            cursor += length;
            return true;
        }

        void PutU32(std::vector<uint8_t>& bytes, uint32_t value)
        {
            bytes.push_back(static_cast<uint8_t>(value));
            bytes.push_back(static_cast<uint8_t>(value >> 8U));
            bytes.push_back(static_cast<uint8_t>(value >> 16U));
            bytes.push_back(static_cast<uint8_t>(value >> 24U));
        }

        void PutFloat(std::vector<uint8_t>& bytes, float value)
        {
            PutU32(bytes, std::bit_cast<uint32_t>(value));
        }

        bool ReadU32(std::span<const uint8_t> bytes, size_t& cursor, uint32_t& value) noexcept
        {
            if (cursor + sizeof(uint32_t) > bytes.size())
            {
                return false;
            }
            value = static_cast<uint32_t>(bytes[cursor]) | (static_cast<uint32_t>(bytes[cursor + 1U]) << 8U) |
                    (static_cast<uint32_t>(bytes[cursor + 2U]) << 16U) |
                    (static_cast<uint32_t>(bytes[cursor + 3U]) << 24U);
            cursor += sizeof(uint32_t);
            return true;
        }

        bool ReadFloat(std::span<const uint8_t> bytes, size_t& cursor, float& value) noexcept
        {
            uint32_t bits = 0;
            if (!ReadU32(bytes, cursor, bits))
            {
                return false;
            }
            value = std::bit_cast<float>(bits);
            return std::isfinite(value);
        }

        bool ReadU8(std::span<const uint8_t> bytes, size_t& cursor, uint8_t& value) noexcept
        {
            if (cursor >= bytes.size())
            {
                return false;
            }
            value = bytes[cursor++];
            return true;
        }

        bool ValidFloat(float value) noexcept
        {
            return std::isfinite(value);
        }

        template <size_t N> void PutString(std::vector<uint8_t>& bytes, const std::array<char, N>& value, size_t length)
        {
            bytes.push_back(static_cast<uint8_t>(length));
            bytes.insert(bytes.end(), value.data(), value.data() + length);
        }

        void PutSnapshot(std::vector<uint8_t>& bytes, const Packet& packet)
        {
            PutU32(bytes, packet.accountId);
            PutU32(bytes, packet.characterId);
            PutU32(bytes, packet.targetId);
            PutU32(bytes, packet.areaId);
            PutFloat(bytes, packet.x);
            PutFloat(bytes, packet.y);
            PutFloat(bytes, packet.z);
            PutFloat(bytes, packet.health);
            PutU32(bytes, packet.interactionCount);
            PutU32(bytes, packet.stateSequence);
        }

        bool ReadSnapshot(std::span<const uint8_t> bytes, size_t& cursor, Packet& packet) noexcept
        {
            return ReadU32(bytes, cursor, packet.accountId) && ReadU32(bytes, cursor, packet.characterId) &&
                   ReadU32(bytes, cursor, packet.targetId) && ReadU32(bytes, cursor, packet.areaId) &&
                   ReadFloat(bytes, cursor, packet.x) && ReadFloat(bytes, cursor, packet.y) &&
                   ReadFloat(bytes, cursor, packet.z) && ReadFloat(bytes, cursor, packet.health) &&
                   ReadU32(bytes, cursor, packet.interactionCount) && ReadU32(bytes, cursor, packet.stateSequence);
        }
    } // namespace

    bool Decode(std::span<const uint8_t> bytes, Packet& packet) noexcept
    {
        packet = {};
        if (bytes.size() < HeaderBytes || bytes.size() > MaxPacketBytes || bytes[0] != Magic0 || bytes[1] != Magic1 ||
            bytes[2] != CurrentVersion || !IsOperation(bytes[3]) || !IsStatus(bytes[4]) || (bytes[5] & 0xFEU) != 0U)
        {
            return false;
        }
        packet.operation = static_cast<Operation>(bytes[3]);
        packet.status = static_cast<Status>(bytes[4]);
        packet.response = (bytes[5] & 1U) != 0U;
        size_t cursor = 6;
        if (!ReadU32(bytes, cursor, packet.requestId))
        {
            packet = {};
            return false;
        }

        bool valid = false;
        if (packet.response)
        {
            valid = ReadSnapshot(bytes, cursor, packet);
        }
        else
        {
            switch (packet.operation)
            {
            case Operation::Register:
            case Operation::Login:
                valid = ReadString(bytes, cursor, packet.username) && ReadString(bytes, cursor, packet.password);
                break;
            case Operation::CreateCharacter:
                valid = ReadU32(bytes, cursor, packet.accountId) && ReadString(bytes, cursor, packet.name) &&
                        ReadU8(bytes, cursor, packet.race) && ReadU8(bytes, cursor, packet.classId);
                break;
            case Operation::EnterWorld:
                valid = ReadU32(bytes, cursor, packet.accountId) && ReadU32(bytes, cursor, packet.characterId) &&
                        ReadU32(bytes, cursor, packet.areaId);
                break;
            case Operation::Move:
                valid = ReadU32(bytes, cursor, packet.accountId) && ReadU32(bytes, cursor, packet.characterId) &&
                        ReadFloat(bytes, cursor, packet.x) && ReadFloat(bytes, cursor, packet.z);
                break;
            case Operation::Interact:
                valid = ReadU32(bytes, cursor, packet.accountId) && ReadU32(bytes, cursor, packet.characterId) &&
                        ReadU32(bytes, cursor, packet.targetId);
                break;
            case Operation::State:
                valid = ReadSnapshot(bytes, cursor, packet);
                break;
            }
        }
        if (!valid || cursor != bytes.size())
        {
            packet = {};
            return false;
        }
        return true;
    }

    std::vector<uint8_t> Encode(const Packet& packet)
    {
        if (!IsOperation(static_cast<uint8_t>(packet.operation)) || !IsStatus(static_cast<uint8_t>(packet.status)) ||
            !ValidFloat(packet.x) || !ValidFloat(packet.y) || !ValidFloat(packet.z) || !ValidFloat(packet.health))
        {
            return {};
        }

        size_t usernameLength = 0;
        size_t passwordLength = 0;
        size_t nameLength = 0;
        if (!StringLength(packet.username, usernameLength) || !StringLength(packet.password, passwordLength) ||
            !StringLength(packet.name, nameLength))
        {
            return {};
        }

        std::vector<uint8_t> bytes;
        bytes.reserve(MaxPacketBytes);
        bytes.insert(bytes.end(),
                     {Magic0, Magic1, CurrentVersion, static_cast<uint8_t>(packet.operation),
                      static_cast<uint8_t>(packet.status), static_cast<uint8_t>(packet.response ? 1U : 0U)});
        PutU32(bytes, packet.requestId);
        if (packet.response || packet.operation == Operation::State)
        {
            PutSnapshot(bytes, packet);
        }
        else
        {
            switch (packet.operation)
            {
            case Operation::Register:
            case Operation::Login:
                PutString(bytes, packet.username, usernameLength);
                PutString(bytes, packet.password, passwordLength);
                break;
            case Operation::CreateCharacter:
                PutU32(bytes, packet.accountId);
                PutString(bytes, packet.name, nameLength);
                bytes.push_back(packet.race);
                bytes.push_back(packet.classId);
                break;
            case Operation::EnterWorld:
                PutU32(bytes, packet.accountId);
                PutU32(bytes, packet.characterId);
                PutU32(bytes, packet.areaId);
                break;
            case Operation::Move:
                PutU32(bytes, packet.accountId);
                PutU32(bytes, packet.characterId);
                PutFloat(bytes, packet.x);
                PutFloat(bytes, packet.z);
                break;
            case Operation::Interact:
                PutU32(bytes, packet.accountId);
                PutU32(bytes, packet.characterId);
                PutU32(bytes, packet.targetId);
                break;
            case Operation::State:
                break;
            }
        }
        if (bytes.size() > MaxPacketBytes)
        {
            return {};
        }
        return bytes;
    }
} // namespace MMO::SessionGateWire
