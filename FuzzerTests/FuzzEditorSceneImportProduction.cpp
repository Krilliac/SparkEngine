/**
 * @file FuzzEditorSceneImportProduction.cpp
 * @brief libc++-compiled production adapter for the Scene Import panel libFuzzer harness.
 *
 * SceneImportPanel::ParseSceneFile reads a game INI .scene through one bounded handle and
 * hands the text to SparkEditor::ParseGameSceneIni; SceneEditTools::CommitSceneImport then
 * turns every object into an editor entity's Transform. The adapter hands the fuzz bytes to
 * the shipped reader. A violated contract aborts so libFuzzer records a crash:
 *  - every imported position, rotation and scale component is finite (strtof accepts
 *    "nan" and "inf" and overflows to infinity);
 *  - every object is a cube or model node with a name, every skip names what it skipped;
 *  - parsing the same bytes twice gives the same document;
 *  - the document written back as INI ([Scene], one [Object] per object, one section per
 *    skip, floats with nine significant digits) parses to the identical document, so the
 *    reader has one meaning for everything it produces.
 */

#include "FuzzEditorSceneImportProduction.h"

#include "Panels/SceneImportParser.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzEditorSceneImport: ParseGameSceneIni violated: %s\n", what);
        std::abort();
    }

    using Record = SparkEditor::SceneEditTools::SceneObjectRecord;

    void AppendBits(std::string& out, const float (&values)[3])
    {
        for (const float value : values)
        {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            out += std::to_string(bits);
            out += ',';
        }
    }

    void AppendText(std::string& out, std::string_view value)
    {
        out += std::to_string(value.size());
        out += ':';
        out.append(value.data(), value.size());
    }

    std::string Fingerprint(const SparkEditor::GameSceneIniDocument& document)
    {
        std::string out;
        AppendText(out, document.sceneName);
        for (const Record& record : document.objects)
        {
            out += "|O";
            AppendText(out, record.type);
            AppendText(out, record.name);
            AppendText(out, record.model);
            AppendText(out, record.material);
            AppendBits(out, record.position);
            AppendBits(out, record.rotationDeg);
            AppendBits(out, record.scale);
        }
        for (const std::string& skipped : document.skippedTypes)
        {
            out += "|S";
            AppendText(out, skipped);
        }
        return out;
    }

    std::string Triple(const float (&values)[3])
    {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "%.9g,%.9g,%.9g", static_cast<double>(values[0]),
                      static_cast<double>(values[1]), static_cast<double>(values[2]));
        return buffer;
    }

    std::string Write(const SparkEditor::GameSceneIniDocument& document)
    {
        std::string text = "[Scene]\nname=" + document.sceneName + "\n";
        for (const Record& record : document.objects)
        {
            text += "[Object]\ntype=" + record.type + "\nname=" + record.name + "\nmodel=" + record.model +
                    "\nmaterial=" + record.material + "\nposition=" + Triple(record.position) +
                    "\nrotation=" + Triple(record.rotationDeg) + "\nscale=" + Triple(record.scale) + "\n";
        }
        for (const std::string& skipped : document.skippedTypes)
        {
            text += "[Skipped]\ntype=" + skipped + "\n";
        }
        return text;
    }

    bool Finite(const float (&values)[3])
    {
        return std::isfinite(values[0]) && std::isfinite(values[1]) && std::isfinite(values[2]);
    }
} // namespace

extern "C" int SparkFuzzParseEditorSceneImport(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    const std::string_view text =
        size == 0 ? std::string_view() : std::string_view(reinterpret_cast<const char*>(data), size);

    const SparkEditor::GameSceneIniDocument document = SparkEditor::ParseGameSceneIni(text);
    const std::string print = Fingerprint(document);
    if (Fingerprint(SparkEditor::ParseGameSceneIni(text)) != print)
    {
        InvariantFailure("parsing the same bytes twice gave different documents");
    }

    for (const Record& record : document.objects)
    {
        if (!Finite(record.position) || !Finite(record.rotationDeg) || !Finite(record.scale))
        {
            InvariantFailure("an imported transform component is not finite");
        }
        if (record.type != "cube" && record.type != "Cube" && record.type != "model" && record.type != "Model")
        {
            InvariantFailure("an object that is not a cube or model node was imported");
        }
        if (record.name.empty())
        {
            InvariantFailure("an imported object has no name");
        }
    }
    for (const std::string& skipped : document.skippedTypes)
    {
        if (skipped.empty())
        {
            InvariantFailure("a skipped node is recorded without a type");
        }
    }

    if (Fingerprint(SparkEditor::ParseGameSceneIni(Write(document))) != print)
    {
        InvariantFailure("write -> parse changed the document");
    }
    return 0;
}
