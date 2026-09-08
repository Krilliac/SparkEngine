/**
 * @file CrashRedaction.h
 * @brief Deterministic crash-text redaction rules
 */

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Spark::CrashHandlerDetail
{

    /**
     * @brief What a crash report must not carry off the user's machine
     *
     * Populated from the environment, with OS-derived fallbacks, by
     * MakeCrashRedactionContext(); taken as a parameter so the redaction itself
     * is deterministic and testable.
     */
    struct CrashRedactionContext
    {
        /// Absolute directories to collapse, each with the token that replaces it.
        /// Ordered longest-first by MakeCrashRedactionContext so that
        /// %LOCALAPPDATA% wins over the %USERPROFILE% it sits inside.
        std::vector<std::pair<std::string, std::string>> pathTokens;
        std::string userName;    ///< Account name, replaced wherever it appears alone
        std::string machineName; ///< Host name, replaced wherever it appears alone
    };

    /// Names shorter than this are masked only as part of a path token: a one- or
    /// two-character account name occurs inside ordinary words, and substituting
    /// it everywhere would shred the diagnostic text.
    inline constexpr size_t kMinimumMaskableNameLength = 3;

    /**
     * @brief Whether this context can actually remove anything
     *
     * A context with no path roots and no maskable name makes RedactCrashText()
     * an identity function, so the caller would upload the profile path and
     * account name verbatim while believing the text had been redacted. Callers
     * must check this before transport rather than trusting the return value.
     */
    inline bool HasRedactionRules(const CrashRedactionContext& context)
    {
        for (const auto& entry : context.pathTokens)
        {
            if (!entry.first.empty())
                return true;
        }
        return context.userName.size() >= kMinimumMaskableNameLength ||
               context.machineName.size() >= kMinimumMaskableNameLength;
    }

    /** @brief Case-insensitive match of @p needle at @p position, treating '/' and '\\' as equal. */
    inline bool MatchesIgnoringCaseAndSeparators(std::string_view text, size_t position, std::string_view needle)
    {
        if (needle.empty() || position + needle.size() > text.size())
            return false;

        for (size_t i = 0; i < needle.size(); ++i)
        {
            const unsigned char textChar = static_cast<unsigned char>(text[position + i]);
            const unsigned char needleChar = static_cast<unsigned char>(needle[i]);
            const bool bothSeparators =
                (textChar == '\\' || textChar == '/') && (needleChar == '\\' || needleChar == '/');
            if (bothSeparators)
                continue;
            if (std::tolower(textChar) != std::tolower(needleChar))
                return false;
        }
        return true;
    }

    /**
     * @brief Replace every occurrence of @p needle in @p text with @p replacement
     *
     * Case-insensitive and separator-insensitive, so it catches the same path
     * written as C:\Users\name, c:/users/name, or C:\USERS\NAME.
     */
    inline std::string ReplaceAllIgnoringCase(std::string text, std::string_view needle, std::string_view replacement)
    {
        if (needle.empty())
            return text;

        std::string result;
        result.reserve(text.size());
        size_t index = 0;
        while (index < text.size())
        {
            if (MatchesIgnoringCaseAndSeparators(text, index, needle))
            {
                result.append(replacement);
                index += needle.size();
            }
            else
            {
                result.push_back(text[index]);
                ++index;
            }
        }
        return result;
    }

    /**
     * @brief Strip personally identifying data from crash text before transport
     *
     * The Windows crash log embeds the faulting module's full path, which starts
     * with the user's profile directory, and the report is posted to a public
     * GitHub issue. Collapse the known private roots to tokens and mask the bare
     * account and machine names; system paths such as C:\Windows\System32 are
     * kept because they identify nobody and are the diagnostic value of the log.
     *
     * @note Applied to the uploaded copy only — the local artifact on the user's
     *       own disk keeps its full paths.
     */
    inline std::string RedactCrashText(std::string text, const CrashRedactionContext& context)
    {
        for (const auto& [path, token] : context.pathTokens)
        {
            if (!path.empty())
                text = ReplaceAllIgnoringCase(std::move(text), path, token);
        }
        // Longest name first, for the same reason the path roots are: a host
        // name usually embeds the account name (jane -> JANE-DESKTOP), and
        // masking the account first leaves the host name unmatched and half of
        // it still in the uploaded report.
        std::array<std::pair<std::string_view, std::string_view>, 2> names = {{
            {context.userName, "<user>"},
            {context.machineName, "<machine>"},
        }};
        std::stable_sort(names.begin(), names.end(),
                         [](const auto& lhs, const auto& rhs) { return lhs.first.size() > rhs.first.size(); });
        for (const auto& [name, token] : names)
        {
            if (name.size() >= kMinimumMaskableNameLength)
                text = ReplaceAllIgnoringCase(std::move(text), name, token);
        }
        return text;
    }

} // namespace Spark::CrashHandlerDetail
