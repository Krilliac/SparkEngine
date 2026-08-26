/**
 * @file ProcessPipeBuffer.h
 * @brief Bounded buffering policy shared by platform Process implementations.
 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>

namespace Spark::ProcessDetail
{
    inline constexpr size_t MaxBufferedLineBytes = 64 * 1024;
    inline constexpr size_t MaxPipeDrainBytesPerCall = 64 * 1024;

    inline size_t RemainingReadCapacity(size_t bufferedBytes, size_t drainedBytes) noexcept
    {
        const size_t bufferCapacity = bufferedBytes < MaxBufferedLineBytes ? MaxBufferedLineBytes - bufferedBytes : 0;
        const size_t drainCapacity =
            drainedBytes < MaxPipeDrainBytesPerCall ? MaxPipeDrainBytesPerCall - drainedBytes : 0;
        return std::min(bufferCapacity, drainCapacity);
    }

    /**
     * Extract one newline-terminated line, or a bounded chunk when a writer has
     * produced MaxBufferedLineBytes without a newline. Chunking preserves bytes
     * while preventing an unterminated stream from growing without limit.
     */
    inline bool ExtractBufferedLine(std::string& buffer, std::string& line)
    {
        const size_t newline = buffer.find('\n');
        if (newline != std::string::npos)
        {
            line.assign(buffer.data(), newline);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            buffer.erase(0, newline + 1);
            return true;
        }

        if (buffer.size() < MaxBufferedLineBytes)
            return false;

        line.assign(buffer.data(), MaxBufferedLineBytes);
        buffer.erase(0, MaxBufferedLineBytes);
        return true;
    }
} // namespace Spark::ProcessDetail
