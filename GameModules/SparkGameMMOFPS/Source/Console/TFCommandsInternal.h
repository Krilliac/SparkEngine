/**
 * @file TFCommandsInternal.h
 * @brief Shared internals for the TFCommands*.cpp split parts: the lower-case
 *        helper, faction-argument parsing and the client-connected probe used
 *        by more than one part. Include only from the TFCommands translation
 *        units.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Net/TFClientNet.h"

#include <cctype>
#include <string>

namespace Terrafront
{
    namespace CommandDetail
    {

        inline std::string Lower(std::string s)
        {
            for (char& c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        inline bool ParseFaction(const std::string& arg, FactionId& out)
        {
            const std::string s = Lower(arg);
            if (s == "mra")
            {
                out = FactionId::MRA;
                return true;
            }
            if (s == "auc")
            {
                out = FactionId::AUC;
                return true;
            }
            if (s == "hlx")
            {
                out = FactionId::HLX;
                return true;
            }
            return false;
        }

        inline bool ClientConnected(const TFGameContext& ctx)
        {
            return ctx.clientNet != nullptr && ctx.clientNet->IsConnected();
        }

        /// TF-110 harness commands (Console/TFCommandsHarness.cpp): tf_observe,
        /// tf_walk, tf_aim_at, tf_give_raw. Handlers keep `ctx` by pointer, so
        /// they must be unregistered with the rest of the module's commands.
        void RegisterConsoleCommandsHarness(TFGameContext& ctx);

    } // namespace CommandDetail
} // namespace Terrafront
