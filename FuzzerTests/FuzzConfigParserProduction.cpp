/**
 * @file FuzzConfigParserProduction.cpp
 * @brief libc++-compiled production adapter for the INI configuration libFuzzer harness.
 *
 * The fuzz input is handed to the shipped ConfigParser::LoadFromString, the
 * parser ConfigParser::Load runs on every engine and game configuration file.
 * A violation of the parser's contract aborts so libFuzzer records a crash
 * rather than a silent pass:
 *  - the parse is transactional: a rejected document leaves the previously
 *    loaded configuration byte-identical, as the header promises,
 *  - an accepted document survives SaveToString -> LoadFromString ->
 *    SaveToString byte-stable, so a saved configuration always reloads as itself,
 *  - no accepted section, key or value contains a line break, and no key is
 *    empty or contains '=' or leading or trailing whitespace; any of those
 *    would re-emit as a different or injected line.
 */

#include "FuzzConfigParserProduction.h"

#include "Utils/ConfigParser.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    // A known-good configuration loaded before every fuzz document, so a
    // rejected document has a prior state to preserve.
    constexpr const char* kBaselineDocument = "[Baseline]\nkey = value\ncount = 3\n";

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzConfigParser: ConfigParser::LoadFromString violated: %s\n", what);
        std::abort();
    }

    bool HasEdgeWhitespace(const std::string& text)
    {
        constexpr const char* kWhitespace = " \t\n\r\f\v";
        return !text.empty() && (std::string(kWhitespace).find(text.front()) != std::string::npos ||
                                 std::string(kWhitespace).find(text.back()) != std::string::npos);
    }

    bool HasLineBreak(const std::string& text)
    {
        return text.find('\n') != std::string::npos;
    }

    void CheckNames(const Spark::ConfigParser& parser)
    {
        for (const std::string& section : parser.GetSections())
        {
            if (HasLineBreak(section) || HasEdgeWhitespace(section))
                InvariantFailure("section name would re-emit as a different line");
            for (const std::string& key : parser.GetKeys(section))
            {
                if (key.empty() || HasLineBreak(key) || HasEdgeWhitespace(key) || key.find('=') != std::string::npos)
                    InvariantFailure("key would re-emit as a different line");
                if (HasLineBreak(parser.GetString(section, key)))
                    InvariantFailure("value contains a line break");
            }
        }
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production parser.
extern "C" int SparkFuzzParseConfig(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    const std::string content = size == 0 ? std::string() : std::string(reinterpret_cast<const char*>(data), size);

    Spark::ConfigParser parser;
    if (!parser.LoadFromString(kBaselineDocument))
        InvariantFailure("the baseline document is rejected");
    const std::string baseline = parser.SaveToString();

    if (!parser.LoadFromString(content))
    {
        if (parser.SaveToString() != baseline)
            InvariantFailure("a rejected document modified the loaded configuration");
        return 0;
    }

    CheckNames(parser);

    const std::string saved = parser.SaveToString();
    Spark::ConfigParser reloaded;
    if (!reloaded.LoadFromString(saved))
        InvariantFailure("SaveToString output of an accepted document is rejected");
    if (reloaded.SaveToString() != saved)
        InvariantFailure("an accepted document does not reload byte-stable");
    return 0;
}
