/**
 * @file GatewayLocalAdapters.cpp
 * @brief Fixture authenticator and deterministic placement (docs/specs/online-services.md section 6).
 */

#include "GatewayLocalAdapters.h"

#include "GatewaySecurity.h"
#include "Utils/JsonUtils.h"

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace Spark::Gateway
{
    namespace
    {
        bool IsPrintableAscii(std::string_view value)
        {
            return std::ranges::all_of(value, [](char character) { return character >= 0x21 && character <= 0x7e; });
        }

        bool HasOnlyKeys(const Json::Value& object, std::initializer_list<std::string_view> allowed)
        {
            return std::ranges::all_of(object.GetKeys(), [&](const std::string& key)
                                       { return std::ranges::find(allowed, std::string_view(key)) != allowed.end(); });
        }

        const Json::Value* FindTyped(const Json::Value& object, const std::string& key, Json::Type type)
        {
            if (!object.HasKey(key))
                return nullptr;
            const Json::Value& value = object[key];
            return value.GetType() == type ? &value : nullptr;
        }
    } // namespace

    LocalFixtureAuthenticator::LocalFixtureAuthenticator(const std::filesystem::path& fixtureFile)
    {
        std::error_code error;
        const uintmax_t size = std::filesystem::file_size(fixtureFile, error);
        if (error)
        {
            m_error = "Admission fixture cannot be read: " + fixtureFile.string();
            return;
        }
        if (size > LocalFixtureMaximumBytes)
        {
            m_error = "Admission fixture exceeds " + std::to_string(LocalFixtureMaximumBytes) + " bytes";
            return;
        }
        std::ifstream input(fixtureFile, std::ios::binary);
        // Read one byte past the limit so a file that grew after file_size() still fails closed.
        std::string text(LocalFixtureMaximumBytes + 1, '\0');
        input.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (!input.is_open() || input.bad())
        {
            m_error = "Admission fixture cannot be read: " + fixtureFile.string();
            return;
        }
        text.resize(static_cast<size_t>(input.gcount()));
        Load(text);
    }

    LocalFixtureAuthenticator LocalFixtureAuthenticator::FromText(std::string_view fixtureText)
    {
        LocalFixtureAuthenticator authenticator;
        authenticator.Load(fixtureText);
        return authenticator;
    }

    void LocalFixtureAuthenticator::Load(std::string_view fixtureText)
    {
        m_principals.clear();
        m_error.clear();
        auto fail = [this](std::string reason)
        {
            m_principals.clear();
            m_error = "Admission fixture rejected: " + std::move(reason);
        };

        Json::JsonLimits limits;
        limits.maxBytes = LocalFixtureMaximumBytes;
        // Root object > principals array > principal object > field.
        limits.maxDepth = 4;
        limits.rejectDuplicateKeys = true;
        Json::Value root;
        std::string parseError;
        if (!Json::ParseBounded(fixtureText, limits, &root, &parseError))
            return fail("malformed or over budget (" + parseError + ")");
        if (!root.IsObject() || !HasOnlyKeys(root, {"version", "principals"}))
            return fail("root must be an object with only 'version' and 'principals'");
        const Json::Value* version = FindTyped(root, "version", Json::Type::Number);
        if (version == nullptr || version->AsNumber() != 1.0)
            return fail("'version' must be 1");
        const Json::Value* principals = FindTyped(root, "principals", Json::Type::Array);
        if (principals == nullptr)
            return fail("'principals' must be an array");

        std::unordered_set<std::string> principalIds;
        for (size_t index = 0; index < principals->Size(); ++index)
        {
            // Reasons name the entry index only: the credential must never reach an error or a log.
            const std::string where = "principals[" + std::to_string(index) + "]";
            const Json::Value& entry = (*principals)[index];
            if (!entry.IsObject() || !HasOnlyKeys(entry, {"credential", "principalId", "entitled", "moderation"}))
                return fail(where + " must be an object with only credential, principalId, entitled, moderation");
            const Json::Value* credential = FindTyped(entry, "credential", Json::Type::String);
            const Json::Value* principalId = FindTyped(entry, "principalId", Json::Type::String);
            const Json::Value* entitled = FindTyped(entry, "entitled", Json::Type::Bool);
            const Json::Value* moderation = FindTyped(entry, "moderation", Json::Type::String);
            if (credential == nullptr || principalId == nullptr || entitled == nullptr || moderation == nullptr)
                return fail(where + " is missing a field or has a field of the wrong type");
            const std::string& credentialText = credential->AsString();
            const std::string& principalText = principalId->AsString();
            if (credentialText.empty() || credentialText.size() > GatewayMaximumCredentialSize ||
                !IsPrintableAscii(credentialText))
                return fail(where + " credential must be 1-" + std::to_string(GatewayMaximumCredentialSize) +
                            " printable ASCII characters");
            if (principalText.empty() || principalText.size() > LocalFixtureMaximumPrincipalIdSize ||
                !IsPrintableAscii(principalText))
                return fail(where + " principalId must be 1-" + std::to_string(LocalFixtureMaximumPrincipalIdSize) +
                            " printable ASCII characters");
            const std::string& moderationText = moderation->AsString();
            if (moderationText != "none" && moderationText != "banned")
                return fail(where + " moderation must be \"none\" or \"banned\"");
            if (!principalIds.insert(principalText).second)
                return fail(where + " repeats a principalId");

            Principal principal;
            principal.principalId = principalText;
            principal.entitled = entitled->AsBool();
            principal.banned = moderationText == "banned";
            if (!m_principals.emplace(credentialText, std::move(principal)).second)
                return fail(where + " repeats a credential");
        }
    }

    AuthenticationResult LocalFixtureAuthenticator::Authenticate(const AdmissionRequest& request)
    {
        if (!IsReady())
            return {false, {}, "Admission fixture is not loaded"};
        const auto found = m_principals.find(request.credential);
        if (found == m_principals.end())
            return {false, {}, "Unknown credential"};
        if (found->second.banned)
            return {false, {}, "Principal is banned"};
        if (!found->second.entitled)
            return {false, {}, "Principal is not entitled"};
        return {true, found->second.principalId, {}};
    }

    Net::AreaID LocalDeterministicPlacement::Place(const PlacementRequest& request, std::span<const AreaSnapshot> areas)
    {
        (void)request;
        const AreaSnapshot* best = nullptr;
        for (const AreaSnapshot& area : areas)
        {
            if (!area.online || area.areaId == Net::INVALID_AREA || area.sessions >= area.capacity)
                continue;
            if (best == nullptr || area.sessions < best->sessions ||
                (area.sessions == best->sessions && area.areaId < best->areaId))
                best = &area;
        }
        return best == nullptr ? Net::INVALID_AREA : best->areaId;
    }
} // namespace Spark::Gateway
