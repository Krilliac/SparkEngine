/**
 * @file FuzzEditorLayoutProduction.cpp
 * @brief libc++-compiled production adapter for the editor panel-layout libFuzzer harness.
 *
 * The layout menu applies a saved layout through EditorLayoutManager::LoadLayout, which
 * reads <layout dir>/<name>.json through one bounded handle, parses every panel before
 * applying any, and applies only panels that are registered. The adapter writes the fuzz
 * bytes as a layout of a manager with five registered panels and loads it. A violated
 * contract aborts so libFuzzer records a crash:
 *  - a refused layout changes no panel and no current-layout name and reports an error;
 *  - an accepted layout becomes current, keeps the registered panel set, and leaves every
 *    panel's geometry finite (a size of 1e300 used to become +inf, which the writer saved as
 *    "inf" and the reader then loaded as 0; a dock or tab order outside int range was
 *    converted with undefined behaviour);
 *  - the accepted state saved by SaveCurrentLayout loads back over the defaults as the
 *    identical state.
 */

#include "FuzzEditorLayoutProduction.h"

#include "Core/EditorLayoutManager.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzEditorLayout: EditorLayoutManager::LoadLayout violated: %s\n", what);
        std::abort();
    }

    std::vector<SparkEditor::PanelConfig> RegisteredPanels()
    {
        using SparkEditor::LayoutDockPosition;
        std::vector<SparkEditor::PanelConfig> panels(5);
        panels[0].name = "Hierarchy";
        panels[0].dockPosition = LayoutDockPosition::Left;
        panels[1].name = "Inspector";
        panels[1].dockPosition = LayoutDockPosition::Right;
        panels[1].sizeX = 350.0f;
        panels[2].name = "SceneView";
        panels[2].canClose = false;
        panels[3].name = "Console";
        panels[3].dockPosition = LayoutDockPosition::Bottom;
        panels[3].tabOrder = 1;
        panels[4].name = "Asset Browser";
        panels[4].displayName = "Assets";
        panels[4].parentDock = "Console";
        return panels;
    }

    struct Fixture
    {
        SparkEditor::EditorLayoutManager manager;
        std::filesystem::path directory;
        std::vector<std::string> names;
    };

    Fixture& Instance()
    {
        static Fixture fixture;
        static const bool ready = []
        {
            std::string pattern = (std::filesystem::temp_directory_path() / "spark-fuzz-layout-XXXXXX").string();
            if (::mkdtemp(pattern.data()) == nullptr)
            {
                InvariantFailure("could not create the fixture directory");
            }
            fixture.directory = std::filesystem::path(pattern);
            if (!fixture.manager.Initialize(fixture.directory.string()))
            {
                InvariantFailure("the layout manager did not initialize");
            }
            for (const SparkEditor::PanelConfig& panel : RegisteredPanels())
            {
                fixture.manager.RegisterPanel(panel);
            }
            fixture.names = fixture.manager.GetRegisteredPanels();
            return true;
        }();
        (void)ready;
        return fixture;
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

    std::string Fingerprint(const Fixture& fixture)
    {
        std::string out;
        for (const std::string& name : fixture.names)
        {
            const SparkEditor::PanelConfig* panel = fixture.manager.GetPanelConfig(name);
            if (panel == nullptr)
            {
                InvariantFailure("a registered panel disappeared");
            }
            out += '|';
            Append(out, panel->name);
            Append(out, panel->displayName);
            out += std::to_string(static_cast<int>(panel->dockPosition));
            out += ',';
            AppendBits(out, panel->sizeX);
            AppendBits(out, panel->sizeY);
            AppendBits(out, panel->posX);
            AppendBits(out, panel->posY);
            out += panel->isVisible ? '1' : '0';
            out += panel->isFloating ? '1' : '0';
            out += panel->canClose ? '1' : '0';
            out += panel->canDock ? '1' : '0';
            AppendBits(out, panel->dockRatio);
            out += std::to_string(panel->tabOrder);
            out += ',';
            Append(out, panel->parentDock);
        }
        return out;
    }

    void CheckFinite(const Fixture& fixture)
    {
        for (const std::string& name : fixture.names)
        {
            const SparkEditor::PanelConfig* panel = fixture.manager.GetPanelConfig(name);
            if (!std::isfinite(panel->sizeX) || !std::isfinite(panel->sizeY) || !std::isfinite(panel->posX) ||
                !std::isfinite(panel->posY) || !std::isfinite(panel->dockRatio))
            {
                InvariantFailure("an accepted panel's geometry is not finite");
            }
        }
    }
} // namespace

extern "C" int SparkFuzzLoadEditorLayout(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    const std::string_view input =
        size == 0 ? std::string_view() : std::string_view(reinterpret_cast<const char*>(data), size);
    Fixture& fixture = Instance();
    SparkEditor::EditorLayoutManager& manager = fixture.manager;
    manager.ResetToDefault();
    WriteFile(fixture.directory / "fuzz.json", input);

    const std::string before = Fingerprint(fixture);
    const std::string currentBefore = manager.GetCurrentLayoutName();
    if (!manager.LoadLayout("fuzz"))
    {
        if (Fingerprint(fixture) != before)
        {
            InvariantFailure("a refused layout changed a panel");
        }
        if (manager.GetCurrentLayoutName() != currentBefore)
        {
            InvariantFailure("a refused layout changed the current layout name");
        }
        if (manager.GetLastError().empty())
        {
            InvariantFailure("a refused layout reports no error");
        }
        return 0;
    }
    if (!manager.GetLastError().empty() || manager.GetCurrentLayoutName() != "fuzz")
    {
        InvariantFailure("an accepted layout reports an error or did not become current");
    }
    if (manager.GetRegisteredPanels() != fixture.names)
    {
        InvariantFailure("an accepted layout changed the registered panel set");
    }
    CheckFinite(fixture);

    const std::string loaded = Fingerprint(fixture);
    if (!manager.SaveCurrentLayout("roundtrip"))
    {
        InvariantFailure("an accepted layout could not be saved");
    }
    manager.ResetToDefault();
    if (!manager.LoadLayout("roundtrip"))
    {
        InvariantFailure("a saved layout is refused");
    }
    if (Fingerprint(fixture) != loaded)
    {
        InvariantFailure("save -> load changed a panel");
    }
    return 0;
}
