/**
 * @file TFChatRules.h
 * @brief Pure validation and recipient rules for TERRAFRONT chat.
 */
#pragma once

#include "Net/TFNetProtocol.h"

#include <cstddef>
#include <cstdint>

namespace Terrafront
{

    constexpr float kTFYellRangeM = 150.0f;
    constexpr float kTFChatVisibleSec = 12.0f;
    constexpr size_t kTFChatHistoryMax = 64;

    inline bool IsValidChatChannel(uint8_t channel)
    {
        // W8 ui-polish: Outfit is the last valid channel (additive after Yell).
        return channel <= static_cast<uint8_t>(ChatChannel::Outfit);
    }

    inline size_t ValidUtf8SequenceLength(const char* input, size_t remaining)
    {
        if (remaining == 0)
            return 0;

        const auto byte = [](char c) { return static_cast<unsigned char>(c); };
        const auto continuation = [&](size_t offset)
        { return offset < remaining && (byte(input[offset]) & 0xC0u) == 0x80u; };
        const unsigned char lead = byte(input[0]);
        if (lead < 0x80u)
            return 1;
        if (lead >= 0xC2u && lead <= 0xDFu)
            return continuation(1) ? 2 : 0;
        if (lead >= 0xE0u && lead <= 0xEFu)
        {
            if (!continuation(1) || !continuation(2))
                return 0;
            const unsigned char second = byte(input[1]);
            if ((lead == 0xE0u && second < 0xA0u) || (lead == 0xEDu && second >= 0xA0u))
                return 0;
            return 3;
        }
        if (lead >= 0xF0u && lead <= 0xF4u)
        {
            if (!continuation(1) || !continuation(2) || !continuation(3))
                return 0;
            const unsigned char second = byte(input[1]);
            if ((lead == 0xF0u && second < 0x90u) || (lead == 0xF4u && second >= 0x90u))
                return 0;
            return 4;
        }
        return 0;
    }

    /// Copies a wire field into a canonical, null-terminated form. ASCII control
    /// and whitespace runs become one space. Malformed UTF-8 is rejected, and
    /// truncation never splits a code point.
    inline bool NormalizeChatText(const char* input, size_t inputSize, char* output, size_t outputSize)
    {
        if (!input || !output || outputSize == 0)
            return false;

        size_t written = 0;
        bool pendingSpace = false;
        for (size_t i = 0; i < inputSize && input[i] != '\0'; ++i)
        {
            const auto c = static_cast<unsigned char>(input[i]);
            if (c <= 0x20 || c == 0x7F)
            {
                pendingSpace = written != 0;
                continue;
            }

            const size_t sequenceLength = ValidUtf8SequenceLength(input + i, inputSize - i);
            if (sequenceLength == 0)
            {
                output[0] = '\0';
                return false;
            }

            const size_t required = sequenceLength + (pendingSpace ? 1u : 0u);
            if (required >= outputSize - written)
                break;
            if (pendingSpace)
                output[written++] = ' ';
            pendingSpace = false;
            for (size_t byteIndex = 0; byteIndex < sequenceLength; ++byteIndex)
                output[written++] = input[i + byteIndex];
            i += sequenceLength - 1;
        }
        output[written] = '\0';
        return written != 0;
    }

    struct TFChatRouteView
    {
        bool isSender{false};
        bool sameRegion{false};
        bool sameFaction{false};
        bool sameSquad{false};
        bool bothHavePawns{false};
        float distanceSq{0.0f};
        // W8 ui-polish (additive, kept LAST so older positional initializers stay
        // valid): both players belong to the same outfit — server truth comes from
        // TFOutfitSystem::OutfitIdOf (see TFServerSim::HandleChatMsg wiring).
        bool sameOutfit{false};
    };

    inline bool ShouldReceiveChat(ChatChannel channel, const TFChatRouteView& view)
    {
        if (!IsValidChatChannel(static_cast<uint8_t>(channel)))
            return false;
        if (view.isSender)
            return true;
        switch (channel)
        {
        case ChatChannel::Region:
            return view.sameRegion;
        case ChatChannel::Faction:
            return view.sameFaction;
        case ChatChannel::Squad:
            return view.sameSquad;
        case ChatChannel::Yell:
            return view.bothHavePawns && view.distanceSq >= 0.0f && view.distanceSq <= kTFYellRangeM * kTFYellRangeM;
        case ChatChannel::Outfit:
            return view.sameOutfit;
        default:
            return false;
        }
    }

    inline const char* ChatChannelName(ChatChannel channel)
    {
        switch (channel)
        {
        case ChatChannel::Region:
            return "REGION";
        case ChatChannel::Faction:
            return "FACTION";
        case ChatChannel::Squad:
            return "SQUAD";
        case ChatChannel::Yell:
            return "YELL";
        case ChatChannel::Outfit:
            return "OUTFIT";
        default:
            return "?";
        }
    }

} // namespace Terrafront
