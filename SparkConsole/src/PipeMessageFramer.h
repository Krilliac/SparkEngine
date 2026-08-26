/**
 * @file PipeMessageFramer.h
 * @brief Bounded newline framing for the engine-to-SparkConsole byte stream.
 */

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class PipeMessageFramer
{
  public:
    static constexpr size_t kMaxLineBytes = 16 * 1024;
    static constexpr std::string_view kOversizedLineNotice =
        "[WARN] Engine console line exceeded 16384 bytes and was discarded.";

    /**
     * Append a transport chunk and return only complete newline-delimited
     * records. An oversized record is discarded through its newline and
     * represented by one bounded notice rather than one message per read.
     */
    std::vector<std::string> Push(std::string_view chunk)
    {
        std::vector<std::string> lines;
        size_t offset = 0;

        while (offset < chunk.size())
        {
            const size_t newline = chunk.find('\n', offset);
            const bool complete = newline != std::string_view::npos;
            const size_t end = complete ? newline : chunk.size();
            const std::string_view piece = chunk.substr(offset, end - offset);

            if (!m_discardingOversizedLine)
            {
                if (piece.size() > kMaxLineBytes - m_pending.size())
                {
                    m_pending.clear();
                    m_discardingOversizedLine = true;
                }
                else
                {
                    m_pending.append(piece.data(), piece.size());
                }
            }

            if (!complete)
                break;

            if (m_discardingOversizedLine)
            {
                lines.emplace_back(kOversizedLineNotice);
                m_discardingOversizedLine = false;
            }
            else
            {
                if (!m_pending.empty() && m_pending.back() == '\r')
                    m_pending.pop_back();
                if (!m_pending.empty())
                    lines.push_back(std::move(m_pending));
                m_pending.clear();
            }

            offset = newline + 1;
        }

        return lines;
    }

  private:
    std::string m_pending;
    bool m_discardingOversizedLine = false;
};
