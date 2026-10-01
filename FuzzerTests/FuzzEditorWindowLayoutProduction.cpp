/**
 * @file FuzzEditorWindowLayoutProduction.cpp
 * @brief libc++-compiled production adapter for the editor window-layout libFuzzer harness.
 *
 * At startup EditorApplication loads the window layout through
 * EditorWindowManager::LoadLayoutFromFile, which reads the file through one bounded handle
 * and scrapes the layout name, the ImGui dock string and the panel list. The adapter writes
 * the fuzz bytes as that file and loads it into the editor's singleton manager. A violated
 * contract aborts so libFuzzer records a crash:
 *  - a refused file leaves the current layout exactly as it was;
 *  - an accepted layout has a name, only named panels, and finite panel geometry (a
 *    monitor index outside int32 used to be converted with undefined behaviour);
 *  - the accepted layout saved by SaveCurrentLayoutToFile loads again, and saving that
 *    gives the identical file, so the reader has one meaning for what the writer writes.
 */

#include "FuzzEditorWindowLayoutProduction.h"

#include "Core/EditorWindowManager.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzEditorWindowLayout: LoadLayoutFromFile violated: %s\n", what);
        std::abort();
    }

    struct Paths
    {
        std::filesystem::path input;
        std::filesystem::path first;
        std::filesystem::path second;
    };

    const Paths& FixturePaths()
    {
        static const Paths paths = []
        {
            std::string pattern = (std::filesystem::temp_directory_path() / "spark-fuzz-window-layout-XXXXXX").string();
            if (::mkdtemp(pattern.data()) == nullptr)
            {
                InvariantFailure("could not create the fixture directory");
            }
            const std::filesystem::path base(pattern);
            return Paths{base / "input.json", base / "first.json", base / "second.json"};
        }();
        return paths;
    }

    void WriteFile(const std::filesystem::path& path, std::string_view bytes)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out)
        {
            InvariantFailure("could not write the fixture file");
        }
    }

    std::string ReadFile(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    void Append(std::string& out, std::string_view text)
    {
        out += std::to_string(text.size());
        out += ':';
        out.append(text.data(), text.size());
    }

    void AppendBits(std::string& out, float value)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        out += std::to_string(bits);
        out += ',';
    }

    std::string Fingerprint(const SparkEditor::WindowLayout& layout)
    {
        std::string out;
        Append(out, layout.name);
        Append(out, layout.dockLayoutINI);
        for (const SparkEditor::PanelWindowState& panel : layout.panels)
        {
            out += '|';
            Append(out, panel.panelName);
            out += panel.isOpen ? '1' : '0';
            out += panel.isFloating ? '1' : '0';
            AppendBits(out, panel.posX);
            AppendBits(out, panel.posY);
            AppendBits(out, panel.width);
            AppendBits(out, panel.height);
            out += std::to_string(panel.monitorIndex);
        }
        return out;
    }
} // namespace

extern "C" int SparkFuzzLoadEditorWindowLayout(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    const std::string_view input =
        size == 0 ? std::string_view() : std::string_view(reinterpret_cast<const char*>(data), size);
    const Paths& paths = FixturePaths();
    WriteFile(paths.input, input);

    SparkEditor::EditorWindowManager& manager = SparkEditor::EditorWindowManager::GetInstance();
    const std::string before = Fingerprint(manager.GetCurrentLayout());
    if (!manager.LoadLayoutFromFile(paths.input.string()))
    {
        if (Fingerprint(manager.GetCurrentLayout()) != before)
        {
            InvariantFailure("a refused layout file changed the current layout");
        }
        return 0;
    }

    const SparkEditor::WindowLayout& layout = manager.GetCurrentLayout();
    if (layout.name.empty())
    {
        InvariantFailure("an accepted layout has no name");
    }
    for (const SparkEditor::PanelWindowState& panel : layout.panels)
    {
        if (panel.panelName.empty())
        {
            InvariantFailure("an accepted layout holds an unnamed panel");
        }
        if (!std::isfinite(panel.posX) || !std::isfinite(panel.posY) || !std::isfinite(panel.width) ||
            !std::isfinite(panel.height))
        {
            InvariantFailure("an accepted panel's geometry is not finite");
        }
    }

    if (!manager.SaveCurrentLayoutToFile(paths.first.string()))
    {
        InvariantFailure("an accepted layout could not be saved");
    }
    if (!manager.LoadLayoutFromFile(paths.first.string()))
    {
        InvariantFailure("a saved layout file is refused");
    }
    if (!manager.SaveCurrentLayoutToFile(paths.second.string()))
    {
        InvariantFailure("a reloaded layout could not be saved");
    }
    if (ReadFile(paths.first) != ReadFile(paths.second))
    {
        InvariantFailure("save -> load -> save changed the layout file");
    }
    return 0;
}
