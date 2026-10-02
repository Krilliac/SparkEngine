/**
 * @file FuzzUILayoutProduction.cpp
 * @brief libc++-compiled production adapter for the UI layout libFuzzer harness.
 *
 * Two readers build widget trees from UI files: Spark::UI::UILayoutLoader::LoadFromJSON
 * (Engine/UI/UILayoutExtensions.h), a hand-rolled scan of a "children" array into labels,
 * buttons, progress bars, images and nested panels, and UIFactory::ParseConfig
 * (Engine/UI/UIFactory.h), a line format of `type key="value" ...`. The adapter feeds the fuzz
 * bytes to both. A violated contract aborts so libFuzzer records a crash:
 *  - LoadFromJSON never creates a widget deeper than UILayoutLoader::kMaxNestingDepth, never a
 *    nameless one or one outside the five widget kinds, never more widgets than the input has
 *    '{' characters, decoded names and text remain stable strings, and never non-finite geometry,
 *  - LoadFromJSON refuses a null parent and gives the same answer and the same tree twice,
 *  - ParseConfig returns the bare root with only flat children whose types hold no delimiter
 *    and whose ids, bindings and values hold no quote or line break,
 *  - a ParseConfig result written back as `type key="value"` lines parses to the same result,
 *    and parsing the same text twice gives the same result.
 */

#include "FuzzUILayoutProduction.h"

#include "Engine/UI/UIFactory.h"
#include "Engine/UI/UILayoutExtensions.h"
#include "Engine/UI/UISystem.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzUILayout: UI layout reader violated: %s\n", what);
        std::abort();
    }

    using Spark::UI::UIPanel;
    using Spark::UI::UIWidget;
    using Spark::UI::UIWidgetConfig;

    // ------------------------------------------------------------------
    // UILayoutLoader::LoadFromJSON
    // ------------------------------------------------------------------

    enum class Kind : std::uint8_t
    {
        Label,
        Button,
        ProgressBar,
        Image,
        Panel
    };

    struct Node
    {
        Kind kind = Kind::Label;
        std::string name;
        std::string text; // label text, button label or image path
        std::array<float, 4> geometry{};
        std::vector<Node> children;
    };

    bool SameNodes(const std::vector<Node>& a, const std::vector<Node>& b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            if (a[i].kind != b[i].kind || a[i].name != b[i].name || a[i].text != b[i].text ||
                std::memcmp(a[i].geometry.data(), b[i].geometry.data(), sizeof(float) * 4) != 0 ||
                !SameNodes(a[i].children, b[i].children))
            {
                return false;
            }
        }
        return true;
    }

    /// Snapshot @p panel's subtree; its direct children sit at @p level (the root's are 1).
    std::vector<Node> Capture(const UIPanel& panel, std::uint32_t level, std::size_t& count)
    {
        std::vector<Node> nodes;
        for (const auto& child : panel.GetChildren())
        {
            if (level > Spark::UI::UILayoutLoader::kMaxNestingDepth)
            {
                InvariantFailure("a widget was created deeper than kMaxNestingDepth");
            }
            const UIWidget* widget = child.get();
            Node node;
            if (const auto* label = dynamic_cast<const Spark::UI::UILabel*>(widget))
            {
                node.kind = Kind::Label;
                node.text = label->GetText();
            }
            else if (const auto* button = dynamic_cast<const Spark::UI::UIButton*>(widget))
            {
                node.kind = Kind::Button;
                node.text = button->GetLabel();
            }
            else if (dynamic_cast<const Spark::UI::UIProgressBar*>(widget) != nullptr)
            {
                node.kind = Kind::ProgressBar;
            }
            else if (const auto* image = dynamic_cast<const Spark::UI::UIImageWidget*>(widget))
            {
                node.kind = Kind::Image;
                node.text = image->GetTexturePath();
            }
            else if (const auto* nested = dynamic_cast<const UIPanel*>(widget))
            {
                node.kind = Kind::Panel;
                node.children = Capture(*nested, level + 1, count);
            }
            else
            {
                InvariantFailure("a widget of an unknown kind was created");
            }
            node.name = widget->GetName();
            node.geometry = {widget->GetX(), widget->GetY(), widget->GetWidth(), widget->GetHeight()};
            if (node.name.empty())
            {
                InvariantFailure("a nameless widget was created");
            }
            for (const float value : node.geometry)
            {
                if (!std::isfinite(value))
                {
                    InvariantFailure("a widget has non-finite geometry");
                }
            }
            ++count;
            nodes.push_back(std::move(node));
        }
        return nodes;
    }

    void CheckLayout(std::string_view json)
    {
        UIPanel first("fuzz-root");
        const bool accepted = Spark::UI::UILayoutLoader::LoadFromJSON(json, &first);
        std::size_t count = 0;
        const std::vector<Node> tree = Capture(first, 1, count);
        if (!accepted && count != 0)
        {
            InvariantFailure("a rejected layout partially mutated its parent");
        }
        if (count > static_cast<std::size_t>(std::count(json.begin(), json.end(), '{')))
        {
            InvariantFailure("more widgets were created than the layout has objects");
        }

        UIPanel second("fuzz-root");
        const bool acceptedAgain = Spark::UI::UILayoutLoader::LoadFromJSON(json, &second);
        std::size_t countAgain = 0;
        if (acceptedAgain != accepted || !SameNodes(tree, Capture(second, 1, countAgain)))
        {
            InvariantFailure("loading the same layout twice gave different trees");
        }

        if (Spark::UI::UILayoutLoader::LoadFromJSON(json, nullptr))
        {
            InvariantFailure("a layout was accepted without a parent panel");
        }
    }

    // ------------------------------------------------------------------
    // UIFactory::ParseConfig
    // ------------------------------------------------------------------

    bool SameConfig(const UIWidgetConfig& a, const UIWidgetConfig& b)
    {
        if (a.type != b.type || a.id != b.id || a.bindingKey != b.bindingKey || a.properties != b.properties ||
            a.children.size() != b.children.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < a.children.size(); ++i)
        {
            if (!SameConfig(a.children[i], b.children[i]))
            {
                return false;
            }
        }
        return true;
    }

    bool HoldsQuoteOrBreak(const std::string& value)
    {
        return value.find_first_of("\"\n") != std::string::npos;
    }

    bool IsTypeDelimiter(char c)
    {
        return c == ' ' || c == '\t' || c == '{';
    }

    /// One `type key="value" ...` line that ParseConfig reads back as @p widget. ParseConfig
    /// starts the first key at the delimiter that ended the type and each later key right
    /// after the previous closing quote, so nothing is inserted that would change a key.
    std::string WriteConfigLine(const UIWidgetConfig& widget)
    {
        std::vector<std::pair<std::string, std::string>> entries(widget.properties.begin(), widget.properties.end());
        if (!widget.id.empty())
        {
            entries.emplace_back("id", widget.id);
        }
        if (!widget.bindingKey.empty())
        {
            entries.emplace_back("bind", widget.bindingKey);
        }

        if (widget.type.empty())
        {
            // An empty type ends at a leading '{', which the first key keeps.
            const auto brace = std::find_if(entries.begin(), entries.end(), [](const auto& entry)
                                            { return !entry.first.empty() && entry.first.front() == '{'; });
            if (brace == entries.end())
            {
                if (!entries.empty())
                {
                    InvariantFailure("an empty-type widget has keys but none kept the brace that ended its type");
                }
                return "{";
            }
            std::rotate(entries.begin(), brace, brace + 1);
        }
        std::string line = widget.type;
        if (entries.empty())
        {
            return line + " ";
        }
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            const auto& [key, value] = entries[i];
            if (i == 0 && (key.empty() || !IsTypeDelimiter(key.front())))
            {
                line += ' ';
            }
            line.append(key).append("=\"").append(value).append("\"");
        }
        return line;
    }

    void CheckConfig(const std::string& text)
    {
        const auto& factory = Spark::UI::UIFactory::GetInstance();
        const UIWidgetConfig root = factory.ParseConfig(text);
        if (root.type != "root" || !root.id.empty() || !root.bindingKey.empty() || !root.properties.empty())
        {
            InvariantFailure("ParseConfig's root is not the bare root node");
        }
        if (root.children.size() > static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) + 1)
        {
            InvariantFailure("ParseConfig produced more widgets than the text has lines");
        }

        std::string written;
        for (const UIWidgetConfig& widget : root.children)
        {
            if (!widget.children.empty())
            {
                InvariantFailure("ParseConfig produced a nested widget");
            }
            if (std::any_of(widget.type.begin(), widget.type.end(),
                            [](char c) { return IsTypeDelimiter(c) || c == '\n'; }))
            {
                InvariantFailure("a widget type holds a delimiter or a line break");
            }
            if (!widget.type.empty() && widget.type.front() == '#')
            {
                InvariantFailure("a comment line produced a widget");
            }
            if (HoldsQuoteOrBreak(widget.id) || HoldsQuoteOrBreak(widget.bindingKey))
            {
                InvariantFailure("an id or binding holds a quote or a line break");
            }
            for (const auto& [key, value] : widget.properties)
            {
                if (key.find_first_of("=\n") != std::string::npos || key == "id" || key == "bind")
                {
                    InvariantFailure("a property key holds '=' or a line break, or names id/bind");
                }
                if (HoldsQuoteOrBreak(value))
                {
                    InvariantFailure("a property value holds a quote or a line break");
                }
            }
            written.append(WriteConfigLine(widget)).append("\n");
        }

        if (!SameConfig(root, factory.ParseConfig(written)))
        {
            InvariantFailure("write -> ParseConfig changed the widget config");
        }
        if (!SameConfig(root, factory.ParseConfig(text)))
        {
            InvariantFailure("parsing the same config twice gave different results");
        }
    }
} // namespace

extern "C" int SparkFuzzLoadUILayout(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    const std::string text = size == 0 ? std::string() : std::string(reinterpret_cast<const char*>(data), size);
    CheckLayout(text);
    CheckConfig(text);
    return 0;
}
