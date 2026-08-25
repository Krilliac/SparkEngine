/**
 * @file TFClientSessionState.h
 * @brief Reply-driven onboarding state owned by one client connection.
 */
#pragma once

#include "Account/TFAccountSystem.h"
#include "Account/TFCharacterSystem.h"
#include "Net/TFNetProtocol.h"

#include <cstdint>
#include <vector>

namespace Terrafront
{
    struct TFClientSessionState
    {
        bool loggedIn{false};
        uint64_t accountId{0};
        TFAuthErr lastAuthError{TFAuthErr::NotLoggedIn};
        std::vector<TF_CharBrief> characters;
        TFCharErr lastCharacterError{TFCharErr::NotLoggedIn};
        uint64_t lastCharacterId{0};

        void ApplyLoginReply(bool ok, uint64_t replyAccountId, TFAuthErr error)
        {
            if (error == TFAuthErr::SessionActive)
            {
                // The authority rejected a re-auth attempt without ending the
                // existing session. Preserve its identity/profile view.
                lastAuthError = error;
                return;
            }
            loggedIn = ok;
            accountId = ok ? replyAccountId : 0;
            lastAuthError = error;
            // A login result starts a new client-side profile view. Never expose
            // characters or operation ids from the previous account while the
            // fresh list reply is pending (or after a rejected login).
            characters.clear();
            lastCharacterError = TFCharErr::NotLoggedIn;
            lastCharacterId = 0;
        }

        void Reset()
        {
            loggedIn = false;
            accountId = 0;
            lastAuthError = TFAuthErr::NotLoggedIn;
            characters.clear();
            lastCharacterError = TFCharErr::NotLoggedIn;
            lastCharacterId = 0;
        }
    };
} // namespace Terrafront
