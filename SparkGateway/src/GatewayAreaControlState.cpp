/**
 * @file GatewayAreaControlState.cpp
 * @brief Area-control epoch state codec (see GatewayAreaControlState.h).
 */

#include "GatewayAreaControlState.h"

#include <charconv>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace Spark::Gateway
{
    namespace
    {
        /// The next whitespace-delimited token as a whole unsigned decimal. std::istream's
        /// numeric extraction accepts a sign and negates it into the unsigned type ("-1"
        /// became the largest epoch), so the token is read as text and parsed strictly.
        template <typename T> bool ReadUnsigned(std::istream& input, T& out)
        {
            std::string token;
            if (!(input >> token))
            {
                return false;
            }
            T value{};
            const char* last = token.data() + token.size();
            const auto [end, error] = std::from_chars(token.data(), last, value);
            if (error != std::errc{} || end != last || token.front() < '0' || token.front() > '9')
            {
                return false;
            }
            out = value;
            return true;
        }
    } // namespace

    std::string SerializeAreaControlState(const AreaControlSessions& sessions)
    {
        std::ostringstream output;
        output << "v2\n";
        for (const auto& [session, fence] : sessions)
        {
            output << std::quoted(session) << ' ' << fence.epoch << ' ' << static_cast<unsigned int>(fence.phase) << ' '
                   << fence.sourceArea << ' ' << fence.targetArea << '\n';
        }
        return output.str();
    }

    bool ParseAreaControlState(std::string_view text, AreaControlSessions& out)
    {
        std::istringstream input{std::string(text)};
        input >> std::ws;
        if (input.peek() == std::char_traits<char>::eof())
        {
            out.clear();
            return true;
        }
        std::string version;
        input >> version;
        if (!input || version != "v2")
        {
            return false;
        }
        AreaControlSessions loaded;
        while (true)
        {
            input >> std::ws;
            if (input.peek() == std::char_traits<char>::eof())
            {
                break;
            }
            std::string session;
            AreaControlSessionFence fence;
            unsigned int phase = 0;
            if (!(input >> std::quoted(session)) || !ReadUnsigned(input, fence.epoch) || !ReadUnsigned(input, phase) ||
                !ReadUnsigned(input, fence.sourceArea) || !ReadUnsigned(input, fence.targetArea) || session.empty() ||
                session.size() > kMaxAreaControlSessionIdBytes || fence.epoch == 0 || phase < 1 ||
                phase > static_cast<unsigned int>(AreaControlPhase::Abort) || fence.sourceArea == 0 ||
                fence.targetArea == 0 || loaded.contains(session))
            {
                return false;
            }
            fence.phase = static_cast<AreaControlPhase>(phase);
            loaded.emplace(std::move(session), fence);
        }
        if (input.bad())
        {
            return false;
        }
        out.swap(loaded);
        return true;
    }
} // namespace Spark::Gateway
