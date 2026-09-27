/**
 * @file TFClientSessionState.h
 * @brief Reply-driven onboarding state owned by one client connection.
 */
#pragma once

#include "Account/TFAccountSystem.h"
#include "Account/TFCharacterSystem.h"
#include "Net/TFNetProtocol.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
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

        /**
         * @brief Validate a server CharListReply at the trust boundary and adopt it.
         *
         * Fails closed (returns false, keeps the previous list) unless count is
         * within the 5-slot array and every listed name is NUL-terminated inside
         * its 24 bytes. Every downstream sink (char-select label, tf_char_list,
         * scripted login) reads TF_CharBrief::name as a C string, so an
         * unterminated name from a malicious server would over-read the heap.
         */
        bool ApplyCharListReply(const TF_CharListReply& reply)
        {
            constexpr size_t kSlots = sizeof(reply.chars) / sizeof(reply.chars[0]);
            if (reply.count > kSlots)
                return false;
            for (size_t i = 0; i < reply.count; ++i)
            {
                const TF_CharBrief& brief = reply.chars[i];
                if (std::memchr(brief.name, '\0', sizeof(brief.name)) == nullptr)
                    return false;
            }
            characters.assign(reply.chars, reply.chars + reply.count);
            return true;
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
