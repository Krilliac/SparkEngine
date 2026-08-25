/**
 * @file TFQuickplay.h
 * @brief Local listen-host quickplay options and bootstrap entry point.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Utils/ScopeGuard.h"
#include "Utils/SecureMemory.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Terrafront
{
    class TFBotSystem;

    struct TFQuickplayOptions
    {
        std::string username;
        std::string password;
        FactionId faction = FactionId::MRA;
        ClassId playerClass = ClassId::Striker;
        uint32_t botCount = 12;
    };

    inline std::string QuickplayLower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    inline bool ParseQuickplayOptions(const std::vector<std::string>& args, TFQuickplayOptions& out, std::string& error)
    {
        // A caller may reuse its result object after a prior successful parse.
        // Clear that owned secret before any validation can return early.
        Spark::SecureClear(out.password);
        out = TFQuickplayOptions{};

        if (args.size() < 2 || args.size() > 5)
        {
            error = "usage: tf_quickplay <user> <pass> [mra|auc|hlx] "
                    "[ghost|striker|medtech|fabricator|bulwark] [bots=0..32]";
            return false;
        }

        TFQuickplayOptions parsed;
        // std::string move assignment may leave small-string storage in the
        // source object. Wipe the parser temporary on every return path,
        // including after the successful copy into `out`.
        const auto clearParsedPassword = Spark::MakeScopeExit([&] { Spark::SecureClear(parsed.password); });
        parsed.username = args[0];
        parsed.password = args[1];
        if (parsed.username.empty() || parsed.password.empty())
        {
            error = "username and password must be non-empty";
            return false;
        }

        if (args.size() >= 3)
        {
            const std::string faction = QuickplayLower(args[2]);
            if (faction == "mra")
                parsed.faction = FactionId::MRA;
            else if (faction == "auc")
                parsed.faction = FactionId::AUC;
            else if (faction == "hlx")
                parsed.faction = FactionId::HLX;
            else
            {
                error = "faction must be mra, auc, or hlx";
                return false;
            }
        }

        if (args.size() >= 4)
        {
            const std::string playerClass = QuickplayLower(args[3]);
            if (playerClass == "ghost")
                parsed.playerClass = ClassId::Ghost;
            else if (playerClass == "striker")
                parsed.playerClass = ClassId::Striker;
            else if (playerClass == "medtech")
                parsed.playerClass = ClassId::Medtech;
            else if (playerClass == "fabricator")
                parsed.playerClass = ClassId::Fabricator;
            else if (playerClass == "bulwark")
                parsed.playerClass = ClassId::Bulwark;
            else
            {
                error = "class must be ghost, striker, medtech, fabricator, or bulwark";
                return false;
            }
        }

        if (args.size() >= 5)
        {
            try
            {
                size_t consumed = 0;
                const unsigned long bots = std::stoul(args[4], &consumed, 10);
                if (consumed != args[4].size() || bots > 32)
                    throw std::out_of_range("bots");
                parsed.botCount = static_cast<uint32_t>(bots);
            }
            catch (const std::exception&)
            {
                error = "bots must be an integer from 0 through 32";
                return false;
            }
        }

        // Copy while the source still owns a non-zero length. The scope guard
        // can then overwrite SSO storage reliably; clearing a moved-from string
        // is not sufficient because its size may already be zero while bytes
        // remain in the inline buffer.
        out = parsed;
        error.clear();
        return true;
    }

    /// Pure half of the runtime gate, kept here so all rejected role/state
    /// combinations remain covered without booting the networking singleton.
    inline bool IsQuickplayListenHostRuntime(NetRole role, bool networkInitialized, bool serverRole,
                                             bool connected) noexcept
    {
        return role == NetRole::ListenHost && networkInitialized && serverRole && connected;
    }

    inline std::string QuickplayCharacterName(uint64_t accountId, FactionId faction)
    {
        std::string digits = std::to_string(accountId);
        if (digits.size() > 17)
            digits.erase(0, digits.size() - 17);
        const char suffix = faction == FactionId::AUC ? 'A' : faction == FactionId::HLX ? 'H' : 'M';
        return "Demo" + digits + suffix;
    }

    std::string RunLocalQuickplay(TFGameContext& ctx, TFBotSystem* bots, const std::vector<std::string>& args);
} // namespace Terrafront
