/**
 * @file GatewayTopologyProbe.cpp
 * @brief Black-box client for the production SparkGateway local ingress API.
 */

#include "GatewayAreaControl.h"
#include "GatewaySecurity.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace
{
    struct Options
    {
        std::string endpoint;
        std::filesystem::path keyFile;
        Spark::Net::ClientID clientId = Spark::Net::INVALID_CLIENT;
        uint64_t nonce = 0;
        std::string sessionId;
        std::string playerName;
    };

    std::string EscapeJson(std::string_view input)
    {
        std::string output;
        output.reserve(input.size());
        constexpr char Hex[] = "0123456789abcdef";
        for (const unsigned char value : input)
        {
            switch (value)
            {
            case '"':
                output += "\\\"";
                break;
            case '\\':
                output += "\\\\";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                if (value < 0x20)
                {
                    output += "\\u00";
                    output.push_back(Hex[value >> 4u]);
                    output.push_back(Hex[value & 0x0fu]);
                }
                else
                    output.push_back(static_cast<char>(value));
            }
        }
        return output;
    }

    std::string_view FailureName(Spark::Gateway::RouteFailure failure)
    {
        using Spark::Gateway::RouteFailure;
        switch (failure)
        {
        case RouteFailure::None:
            return "None";
        case RouteFailure::NotReady:
            return "NotReady";
        case RouteFailure::InvalidRequest:
            return "InvalidRequest";
        case RouteFailure::AuthenticationFailed:
            return "AuthenticationFailed";
        case RouteFailure::DuplicateSession:
            return "DuplicateSession";
        case RouteFailure::CapacityReached:
            return "CapacityReached";
        case RouteFailure::NoAreaAvailable:
            return "NoAreaAvailable";
        }
        return "Unknown";
    }

    template <typename Integer> bool ParseUnsigned(std::string_view text, Integer& output)
    {
        if (text.empty() || text.front() == '+' || text.front() == '-')
            return false;
        Integer value{};
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, 10);
        if (error != std::errc{} || end != text.data() + text.size())
            return false;
        output = value;
        return true;
    }

    void Usage()
    {
        std::cout << "Usage: SparkGatewayTopologyProbe --endpoint <name> --key-file <path> "
                     "--client-id <id> --nonce <value> --session <id> --player <name>\n";
    }

    std::optional<Options> ParseOptions(int argc, char** argv)
    {
        Options options;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument = argv[index];
            if (argument == "--help" || argument == "-h")
            {
                Usage();
                return std::nullopt;
            }
            if (index + 1 >= argc)
                return std::nullopt;
            const std::string_view value = argv[++index];
            if (argument == "--endpoint")
                options.endpoint = value;
            else if (argument == "--key-file")
                options.keyFile = std::string(value);
            else if (argument == "--session")
                options.sessionId = value;
            else if (argument == "--player")
                options.playerName = value;
            else if (argument == "--client-id")
            {
                uint64_t parsed = 0;
                if (!ParseUnsigned(value, parsed) || parsed == Spark::Net::INVALID_CLIENT ||
                    parsed > std::numeric_limits<Spark::Net::ClientID>::max())
                    return std::nullopt;
                options.clientId = static_cast<Spark::Net::ClientID>(parsed);
            }
            else if (argument == "--nonce")
            {
                if (!ParseUnsigned(value, options.nonce) || options.nonce == 0)
                    return std::nullopt;
            }
            else
                return std::nullopt;
        }
        if (options.endpoint.empty() || options.keyFile.empty() || options.clientId == Spark::Net::INVALID_CLIENT ||
            options.nonce == 0 || options.sessionId.empty() || options.playerName.empty())
            return std::nullopt;
        return options;
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h"))
    {
        Usage();
        return 0;
    }
    const auto options = ParseOptions(argc, argv);
    if (!options)
    {
        Usage();
        return 2;
    }

    Spark::Gateway::KeyFileAuthenticator authenticator(options->keyFile);
    if (!authenticator.IsReady())
    {
        std::cerr << "GatewayTopologyProbe: " << authenticator.Error() << '\n';
        return 3;
    }

    Spark::Gateway::AdmissionRequest request;
    request.clientId = options->clientId;
    request.sessionId = options->sessionId;
    request.playerName = options->playerName;
    request.spawnPosition = {0.0f, 0.0f, 0.0f};
    const int64_t now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    request.credential = authenticator.CreateCredential(request, now, options->nonce);
    if (request.credential.empty())
    {
        std::cerr << "GatewayTopologyProbe: failed to create admission credential\n";
        return 3;
    }

    const Spark::Gateway::RouteResult route =
        Spark::Gateway::LocalGatewayIngressClient(options->endpoint).Admit(request);
    std::cout << "{\"accepted\":" << (route.accepted ? "true" : "false") << ",\"failure\":\""
              << FailureName(route.failure) << "\",\"reason\":\"" << EscapeJson(route.reason) << "\",\"host\":\""
              << EscapeJson(route.host) << "\",\"port\":" << route.port
              << ",\"area\":" << route.session.authoritativeArea << "}\n";
    return 0;
}
