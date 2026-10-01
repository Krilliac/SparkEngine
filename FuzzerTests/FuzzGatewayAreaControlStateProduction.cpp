/**
 * @file FuzzGatewayAreaControlStateProduction.cpp
 * @brief libc++-compiled production adapter for the gateway area-control epoch state harness.
 *
 * LocalAreaControlService::LoadState reads the [GatewayControl] epoch_state_file
 * through ParseAreaControlState before SparkServer accepts any handoff phase; the
 * fence it restores decides which epochs every later Prepare may use. The input is
 * the file's bytes. A violated contract aborts so libFuzzer records a crash:
 *  - ParseAreaControlState accepts exactly what an independent model of the documented
 *    format accepts ("v2", then records of a std::quoted session id, epoch, phase,
 *    source and target area as plain unsigned decimal tokens; ids non-empty, at most
 *    128 bytes and unique; epoch >= 1, phase Prepare..Abort, areas non-zero), and
 *    decodes the same sessions,
 *  - a rejected file leaves the caller's sessions untouched,
 *  - SerializeAreaControlState -> ParseAreaControlState reproduces an accepted state.
 */

#include "FuzzGatewayAreaControlStateProduction.h"

#include "GatewayAreaControlState.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace
{
    namespace Gateway = Spark::Gateway;

    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzGatewayAreaControlState: %s\n", what);
        std::abort();
    }

    bool Equal(const Gateway::AreaControlSessions& left, const Gateway::AreaControlSessions& right)
    {
        if (left.size() != right.size())
        {
            return false;
        }
        for (const auto& [session, fence] : left)
        {
            const auto found = right.find(session);
            if (found == right.end() || found->second.epoch != fence.epoch || found->second.phase != fence.phase ||
                found->second.sourceArea != fence.sourceArea || found->second.targetArea != fence.targetArea)
            {
                return false;
            }
        }
        return true;
    }

    Gateway::AreaControlSessions MakeSentinel()
    {
        Gateway::AreaControlSessions sentinel;
        sentinel["sentinel-session"] = Gateway::AreaControlSessionFence{41, Gateway::AreaControlPhase::Transfer, 3, 4};
        return sentinel;
    }

    /// Independent reader: whitespace is the classic-locale isspace set std::istream skips.
    class Model
    {
      public:
        explicit Model(std::string_view text) : m_text(text) {}

        std::optional<Gateway::AreaControlSessions> Parse()
        {
            SkipSpace();
            Gateway::AreaControlSessions sessions;
            if (AtEnd())
            {
                return sessions;
            }
            if (Token() != "v2")
            {
                return std::nullopt;
            }
            while (true)
            {
                SkipSpace();
                if (AtEnd())
                {
                    return sessions;
                }
                const std::optional<std::string> session = Session();
                Gateway::AreaControlSessionFence fence;
                unsigned int phase = 0;
                if (!session || !Number(fence.epoch) || !Number(phase) || !Number(fence.sourceArea) ||
                    !Number(fence.targetArea) || session->empty() ||
                    session->size() > Gateway::kMaxAreaControlSessionIdBytes || fence.epoch == 0 || phase < 1 ||
                    phase > 5 || fence.sourceArea == 0 || fence.targetArea == 0 || sessions.contains(*session))
                {
                    return std::nullopt;
                }
                fence.phase = static_cast<Gateway::AreaControlPhase>(phase);
                sessions.emplace(*session, fence);
            }
        }

      private:
        static bool IsSpace(char c)
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
        }

        bool AtEnd() const { return m_position == m_text.size(); }

        void SkipSpace()
        {
            while (!AtEnd() && IsSpace(m_text[m_position]))
            {
                ++m_position;
            }
        }

        std::string_view Token()
        {
            SkipSpace();
            const std::size_t begin = m_position;
            while (!AtEnd() && !IsSpace(m_text[m_position]))
            {
                ++m_position;
            }
            return m_text.substr(begin, m_position - begin);
        }

        /// std::quoted extraction: a '"'-delimited string with '\\' escaping the next byte,
        /// or else a plain whitespace-delimited token.
        std::optional<std::string> Session()
        {
            SkipSpace();
            if (AtEnd() || m_text[m_position] != '"')
            {
                return std::string(Token());
            }
            ++m_position;
            std::string value;
            while (!AtEnd())
            {
                char c = m_text[m_position++];
                if (c == '\\')
                {
                    if (AtEnd())
                    {
                        return std::nullopt;
                    }
                    c = m_text[m_position++];
                }
                else if (c == '"')
                {
                    return value;
                }
                value += c;
            }
            return std::nullopt;
        }

        template <typename T> bool Number(T& out)
        {
            const std::string_view token = Token();
            if (token.empty())
            {
                return false;
            }
            for (const char c : token)
            {
                if (c < '0' || c > '9')
                {
                    return false;
                }
            }
            const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), out);
            return error == std::errc{} && end == token.data() + token.size();
        }

        std::string_view m_text;
        std::size_t m_position = 0;
    };
} // namespace

extern "C" int SparkFuzzParseGatewayAreaControlState(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    const std::optional<Gateway::AreaControlSessions> expected = Model(text).Parse();

    Gateway::AreaControlSessions sessions = MakeSentinel();
    const bool accepted = Gateway::ParseAreaControlState(text, sessions);
    if (accepted != expected.has_value())
    {
        InvariantFailure(accepted ? "accepted a state file the format model rejects"
                                  : "rejected a state file the format model accepts");
    }
    if (!accepted)
    {
        if (!Equal(sessions, MakeSentinel()))
        {
            InvariantFailure("a rejected state file modified the loaded sessions");
        }
        return 0;
    }
    if (!Equal(sessions, *expected))
    {
        InvariantFailure("decoded sessions differ from the format model");
    }
    Gateway::AreaControlSessions reloaded = MakeSentinel();
    if (!Gateway::ParseAreaControlState(Gateway::SerializeAreaControlState(sessions), reloaded) ||
        !Equal(reloaded, sessions))
    {
        InvariantFailure("Serialize -> Parse changed an accepted state");
    }
    return 0;
}
