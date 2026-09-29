#include "InstallState.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace SparkInstaller
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr std::uintmax_t kMaxInstallStateBytes = 64u * 1024u;
        constexpr std::size_t kMaxPendingMarkerBytes = 4096;

        std::string Escape(const std::string& s)
        {
            std::string out;
            out.reserve(s.size() + 2);
            for (char c : s)
            {
                switch (c)
                {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    out += c;
                }
            }
            return out;
        }

        // Reads the whole install-state file, refusing one above kMaxInstallStateBytes.
        bool ReadFile(const std::string& path, std::string& contents)
        {
            std::error_code error;
            const std::uintmax_t fileSize = fs::file_size(path, error);
            if (error || fileSize > kMaxInstallStateBytes)
                return false;

            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;

            // Allocate only from the bounded stat result, then read exactly that
            // many bytes. A one-byte probe catches a file that grew between the
            // stat and open/read without ever slurping the growth into memory.
            contents.assign(static_cast<size_t>(fileSize), '\0');
            if (fileSize != 0)
            {
                in.read(contents.data(), static_cast<std::streamsize>(fileSize));
                if (in.gcount() != static_cast<std::streamsize>(fileSize))
                {
                    contents.clear();
                    return false;
                }
            }

            char extra = '\0';
            if (in.read(&extra, 1) || !in.eof())
            {
                contents.clear();
                return false;
            }
            return true;
        }

        // Strict reader for the one object Save writes: string members, an integer
        // "schema" and one nested "options" object of booleans. The install tree is
        // editable by anything, so this is not a general JSON parser: each key may
        // appear once, unknown keys are refused, strings unescape exactly the set
        // Escape produces and refuse raw control bytes, and the integer is
        // range-checked. Anything else rejects the whole document. Input is at most
        // kMaxInstallStateBytes and the grammar has one fixed nesting level, so the
        // parse is linear in the input.
        class StateReader
        {
          public:
            explicit StateReader(std::string_view text) : m_text(text) {}

            bool Parse(InstallState& state)
            {
                struct StringField
                {
                    std::string_view key;
                    std::string InstallState::*member;
                };
                constexpr std::array<StringField, 7> kStringFields{{
                    {"ref", &InstallState::ref},
                    {"commit", &InstallState::commit},
                    {"destination", &InstallState::destination},
                    {"generator", &InstallState::generator},
                    {"build_type", &InstallState::buildType},
                    {"built_at", &InstallState::builtAt},
                    {"installer_version", &InstallState::installerVersion},
                }};
                constexpr std::uint32_t kSchemaBit = 1u << 7u;
                constexpr std::uint32_t kOptionsBit = 1u << 8u;
                constexpr std::uint32_t kRequiredBits = kSchemaBit | ((1u << kStringFields.size()) - 1u);

                std::uint32_t seen = 0;
                if (!Consume('{'))
                {
                    return false;
                }
                do
                {
                    std::string key;
                    if (!ReadString(key) || !Consume(':'))
                    {
                        return false;
                    }
                    std::uint32_t bit = 0;
                    bool valueRead = false;
                    if (key == "schema")
                    {
                        bit = kSchemaBit;
                        valueRead = ReadInt(state.schema);
                    }
                    else if (key == "options")
                    {
                        bit = kOptionsBit;
                        valueRead = ReadOptions(state.options);
                    }
                    else
                    {
                        for (std::size_t index = 0; index < kStringFields.size(); ++index)
                        {
                            if (key == kStringFields[index].key)
                            {
                                bit = 1u << index;
                                valueRead = ReadString(state.*kStringFields[index].member);
                                break;
                            }
                        }
                    }
                    // bit == 0 is an unknown key; a set bit is a duplicate.
                    if (bit == 0 || (seen & bit) != 0 || !valueRead)
                    {
                        return false;
                    }
                    seen |= bit;
                } while (Consume(','));
                if (!Consume('}') || !AtEnd() || (seen & kRequiredBits) != kRequiredBits || state.schema != 1)
                {
                    return false;
                }
                for (const StringField& field : kStringFields)
                {
                    if ((state.*field.member).empty())
                    {
                        return false;
                    }
                }
                return true;
            }

          private:
            void SkipWhitespace()
            {
                while (m_pos < m_text.size() && (m_text[m_pos] == ' ' || m_text[m_pos] == '\t' ||
                                                 m_text[m_pos] == '\n' || m_text[m_pos] == '\r'))
                {
                    ++m_pos;
                }
            }

            bool Consume(char expected)
            {
                SkipWhitespace();
                if (m_pos < m_text.size() && m_text[m_pos] == expected)
                {
                    ++m_pos;
                    return true;
                }
                return false;
            }

            bool AtEnd()
            {
                SkipWhitespace();
                return m_pos == m_text.size();
            }

            bool ReadString(std::string& out)
            {
                if (!Consume('"'))
                {
                    return false;
                }
                out.clear();
                while (m_pos < m_text.size())
                {
                    const char c = m_text[m_pos++];
                    if (c == '"')
                    {
                        return true;
                    }
                    if (static_cast<unsigned char>(c) < 0x20u)
                    {
                        return false;
                    }
                    if (c != '\\')
                    {
                        out += c;
                        continue;
                    }
                    if (m_pos >= m_text.size())
                    {
                        return false;
                    }
                    switch (m_text[m_pos++])
                    {
                    case '"':
                        out += '"';
                        break;
                    case '\\':
                        out += '\\';
                        break;
                    case 'n':
                        out += '\n';
                        break;
                    case 'r':
                        out += '\r';
                        break;
                    case 't':
                        out += '\t';
                        break;
                    default:
                        return false;
                    }
                }
                return false;
            }

            bool ReadInt(int& out)
            {
                SkipWhitespace();
                const char* first = m_text.data() + m_pos;
                const char* last = m_text.data() + m_text.size();
                int value = 0;
                const auto [end, error] = std::from_chars(first, last, value);
                if (error != std::errc{})
                {
                    return false;
                }
                m_pos += static_cast<std::size_t>(end - first);
                out = value;
                return true;
            }

            bool ReadBool(bool& out)
            {
                SkipWhitespace();
                const std::string_view rest = m_text.substr(m_pos);
                if (rest.substr(0, 4) == "true")
                {
                    m_pos += 4;
                    out = true;
                    return true;
                }
                if (rest.substr(0, 5) == "false")
                {
                    m_pos += 5;
                    out = false;
                    return true;
                }
                return false;
            }

            bool ReadOptions(std::map<std::string, bool>& options)
            {
                if (!Consume('{'))
                {
                    return false;
                }
                if (Consume('}'))
                {
                    return true;
                }
                do
                {
                    std::string name;
                    bool value = false;
                    if (!ReadString(name) || !Consume(':') || !ReadBool(value) ||
                        !options.emplace(std::move(name), value).second)
                    {
                        return false;
                    }
                } while (Consume(','));
                return Consume('}');
            }

            std::string_view m_text;
            std::size_t m_pos = 0;
        };

        // A pending-marker value must read back exactly: non-empty, one line, no NUL.
        bool IsPendingMarkerValue(std::string_view value)
        {
            return !value.empty() && value.find_first_of(std::string_view("\r\n\0", 3)) == std::string_view::npos;
        }

        std::string NowUtcIso8601()
        {
            auto now = std::chrono::system_clock::now();
            auto t = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
#ifdef _WIN32
            gmtime_s(&tm, &t);
#else
            gmtime_r(&t, &tm);
#endif
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
            return buf;
        }
    } // namespace

    bool InstallState::Exists(const std::string& destination)
    {
        InstallState state;
        return Load(destination, state);
    }

    bool InstallState::Load(const std::string& destination, InstallState& out)
    {
        fs::path path = fs::path(destination) / FileName();
        std::string json;
        if (!ReadFile(path.string(), json))
            return false;

        // Parse into a local and publish only a complete, valid state.
        InstallState parsed;
        if (!StateReader(json).Parse(parsed))
        {
            return false;
        }
        out = std::move(parsed);
        return true;
    }

    bool InstallState::WritePendingMarker(const std::string& tree, const std::string& ref, const std::string& commit)
    {
        // "ref=" + ref + '\n' + "commit=" + commit + '\n' must fit the reader's bound.
        constexpr std::size_t kFramingBytes = 13;
        if (!IsPendingMarkerValue(ref) || !IsPendingMarkerValue(commit) ||
            ref.size() + commit.size() > kMaxPendingMarkerBytes - kFramingBytes)
        {
            return false;
        }
        std::ofstream out(fs::path(tree) / PendingFileName(), std::ios::binary | std::ios::trunc);
        out << "ref=" << ref << '\n' << "commit=" << commit << '\n';
        out.close();
        return static_cast<bool>(out);
    }

    bool InstallState::ReadPendingMarker(const std::string& tree, std::string& ref, std::string& commit)
    {
        const fs::path marker = fs::path(tree) / PendingFileName();
        std::error_code error;
        if (!fs::is_regular_file(fs::symlink_status(marker, error)) || error)
        {
            return false;
        }
        std::ifstream in(marker, std::ios::binary);
        if (!in)
        {
            return false;
        }
        // The read itself is the bound: a file that grew or was swapped after
        // any earlier check still yields at most one byte past the limit.
        std::string contents(kMaxPendingMarkerBytes + 1, '\0');
        in.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        contents.resize(static_cast<std::size_t>(in.gcount()));
        if (contents.size() > kMaxPendingMarkerBytes)
        {
            return false;
        }

        std::optional<std::string> parsedRef;
        std::optional<std::string> parsedCommit;
        std::string_view rest(contents);
        while (!rest.empty())
        {
            const std::size_t newline = rest.find('\n');
            std::string_view line = rest.substr(0, newline);
            rest = newline == std::string_view::npos ? std::string_view() : rest.substr(newline + 1);
            if (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1);
            }
            if (line.empty())
            {
                continue;
            }
            std::optional<std::string>* slot = nullptr;
            std::string_view value;
            if (line.substr(0, 4) == "ref=")
            {
                slot = &parsedRef;
                value = line.substr(4);
            }
            else if (line.substr(0, 7) == "commit=")
            {
                slot = &parsedCommit;
                value = line.substr(7);
            }
            if (slot == nullptr || slot->has_value() || !IsPendingMarkerValue(value))
            {
                return false;
            }
            slot->emplace(value);
        }
        if (!parsedRef || !parsedCommit)
        {
            return false;
        }
        ref = std::move(*parsedRef);
        commit = std::move(*parsedCommit);
        return true;
    }

    bool InstallState::Save(const std::string& destination) const
    {
        if (destination.empty() || schema != 1 || ref.empty() || commit.empty() || generator.empty() ||
            buildType.empty() || installerVersion.empty())
            return false;

        fs::path path = fs::path(destination) / FileName();
        fs::path temporaryPath = path;
        temporaryPath += ".tmp";
        const auto removeTemporary = [&temporaryPath]
        {
            std::error_code ignored;
            fs::remove(temporaryPath, ignored);
        };
        std::ofstream out(temporaryPath, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            removeTemporary();
            return false;
        }

        std::string timestamp = builtAt.empty() ? NowUtcIso8601() : builtAt;

        out << "{\n";
        out << "  \"schema\": " << schema << ",\n";
        out << "  \"ref\": \"" << Escape(ref) << "\",\n";
        out << "  \"commit\": \"" << Escape(commit) << "\",\n";
        out << "  \"destination\": \"" << Escape(destination) << "\",\n";
        out << "  \"generator\": \"" << Escape(generator) << "\",\n";
        out << "  \"build_type\": \"" << Escape(buildType) << "\",\n";
        out << "  \"built_at\": \"" << Escape(timestamp) << "\",\n";
        out << "  \"installer_version\": \"" << Escape(installerVersion) << "\",\n";
        out << "  \"options\": {\n";
        size_t i = 0;
        for (const auto& kv : options)
        {
            out << "    \"" << Escape(kv.first) << "\": " << (kv.second ? "true" : "false");
            if (++i < options.size())
                out << ",";
            out << "\n";
        }
        out << "  }\n";
        out << "}\n";
        out.flush();
        if (!out)
        {
            out.close();
            removeTemporary();
            return false;
        }
        out.close();
        if (!out)
        {
            removeTemporary();
            return false;
        }

        std::error_code renameError;
        std::error_code statusError;
        const fs::file_status existingStatus = fs::symlink_status(path, statusError);
        if (statusError && statusError != std::errc::no_such_file_or_directory)
        {
            removeTemporary();
            return false;
        }
        // symlink_status reports dangling links as a present path too; do not
        // let a replacement follow or overwrite one.
        const auto existingType = existingStatus.type();
        const bool hadExistingMarker = existingType != fs::file_type::not_found && existingType != fs::file_type::none;
        if (hadExistingMarker && !fs::is_regular_file(existingStatus))
        {
            removeTemporary();
            return false;
        }
        // Rename is atomic for the marker replacement on the supported
        // filesystems. Non-regular targets were rejected above so a directory
        // or link cannot be displaced by the new marker.
        fs::rename(temporaryPath, path, renameError);
        if (renameError)
        {
            removeTemporary();
            return false;
        }
        return true;
    }
} // namespace SparkInstaller
