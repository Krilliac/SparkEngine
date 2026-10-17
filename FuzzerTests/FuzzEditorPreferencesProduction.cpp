/**
 * @file FuzzEditorPreferencesProduction.cpp
 * @brief libc++-compiled production adapter for the editor preferences libFuzzer harness.
 *
 * The editor reads editor-preferences.ini through LoadEditorPreferences, which hands at most
 * 64 KiB + 1 bytes to SparkEditor::ParseEditorPreferences. The adapter parses the fuzz bytes
 * and checks the contract the editor relies on. A violated contract aborts so libFuzzer
 * records a crash:
 *  - parsing the same bytes twice gives the same verdict and preferences;
 *  - a rejected parse leaves the output untouched;
 *  - accepted preferences hold at most ten recent scenes, a finite uiScale that is 0 (Auto)
 *    or inside 0.75 .. 2.0, a known appearance, and strings without control characters;
 *  - WriteEditorPreferences of the result parses back to equal preferences, and writing
 *    those again gives the same text.
 */

#include "Core/EditorPreferences.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    using SparkEditor::EditorPreferences;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzEditorPreferences: ParseEditorPreferences violated: %s\n", what);
        std::abort();
    }

    bool HasControlCharacter(std::string_view text)
    {
        return std::any_of(text.begin(), text.end(),
                           [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; });
    }

    EditorPreferences SentinelPreferences()
    {
        EditorPreferences prefs;
        prefs.palette = "sentinel";
        prefs.appearance = "light";
        prefs.uiScale = 1.5f;
        prefs.lastLayout = "sentinel.layout";
        prefs.recentScenes = {"sentinel.scene"};
        return prefs;
    }

    void CheckAccepted(const EditorPreferences& prefs)
    {
        if (prefs.recentScenes.size() > SparkEditor::kMaxEditorRecentScenes)
        {
            InvariantFailure("more than ten recent scenes");
        }
        if (!std::isfinite(prefs.uiScale) ||
            (prefs.uiScale != 0.0f &&
             (prefs.uiScale < SparkEditor::kMinEditorUiScale || prefs.uiScale > SparkEditor::kMaxEditorUiScale)))
        {
            InvariantFailure("uiScale is neither Auto nor inside the user range");
        }
        if (prefs.appearance != "dark" && prefs.appearance != "light" && prefs.appearance != "hc" &&
            prefs.appearance != "system")
        {
            InvariantFailure("unknown appearance");
        }
        if (HasControlCharacter(prefs.palette) || HasControlCharacter(prefs.lastLayout))
        {
            InvariantFailure("a stored string holds a control character");
        }
        for (const std::string& scene : prefs.recentScenes)
        {
            if (scene.empty() || HasControlCharacter(scene))
            {
                InvariantFailure("a recent scene is empty or holds a control character");
            }
        }
    }
} // namespace

extern "C" int SparkFuzzParseEditorPreferences(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    const std::string content = size == 0 ? std::string() : std::string(reinterpret_cast<const char*>(data), size);

    EditorPreferences prefs = SentinelPreferences();
    const bool accepted = SparkEditor::ParseEditorPreferences(content, prefs, nullptr);
    EditorPreferences again = SentinelPreferences();
    if (SparkEditor::ParseEditorPreferences(content, again, nullptr) != accepted || !(again == prefs))
    {
        InvariantFailure("parsing the same bytes twice gave different results");
    }
    if (!accepted)
    {
        if (!(prefs == SentinelPreferences()))
        {
            InvariantFailure("a rejected parse changed the output");
        }
        return 0;
    }
    CheckAccepted(prefs);

    const std::string written = SparkEditor::WriteEditorPreferences(prefs);
    EditorPreferences reread = SentinelPreferences();
    if (!SparkEditor::ParseEditorPreferences(written, reread, nullptr))
    {
        InvariantFailure("the written preferences do not parse");
    }
    if (!(reread == prefs))
    {
        InvariantFailure("write -> parse changed the preferences");
    }
    if (SparkEditor::WriteEditorPreferences(reread) != written)
    {
        InvariantFailure("write -> parse -> write changed the text");
    }
    return 0;
}
