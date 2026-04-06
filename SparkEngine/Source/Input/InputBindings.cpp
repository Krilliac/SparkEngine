/**
 * @file InputBindings.cpp
 * @brief Implementation of the input binding, preset, and accessibility system
 */

#include "InputBindings.h"
#include "../Core/FaultIsolation.h"
#include "../Utils/Validate.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>

namespace Spark
{

    // =============================================================================
    // InputBindingManager
    // =============================================================================

    InputBindingManager::InputBindingManager()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Input, "InputBindingManager initializing");
        CreateDefaultPresets();
    }

    void InputBindingManager::SetBinding(const std::string& action, const InputBinding& binding)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Input);
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Input, action);
        m_bindings[action] = binding;
        for (const auto& callback : m_bindingChangedCallbacks)
        {
            SPARK_GUARDED_UPDATE("Input:BindingChanged", "Input", { callback(action, binding); });
        }
    }

    InputBinding InputBindingManager::GetBinding(const std::string& action) const
    {
        auto it = m_bindings.find(action);
        if (it != m_bindings.end())
        {
            return it->second;
        }
        return InputBinding{};
    }

    void InputBindingManager::RemoveBinding(const std::string& action)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Input);
        SPARK_WARN_IF(Spark::LogCategory::Input, action.empty(), "RemoveBinding called with empty action name");
        m_bindings.erase(action);
    }

    std::string InputBindingManager::FindConflict(const std::string& action) const
    {
        auto it = m_bindings.find(action);
        if (it == m_bindings.end())
        {
            return "";
        }

        int key = it->second.primaryKey;
        for (const auto& [otherAction, otherBinding] : m_bindings)
        {
            if (otherAction != action && (otherBinding.primaryKey == key || otherBinding.alternateKey == key))
            {
                return otherAction;
            }
        }
        return "";
    }

    void InputBindingManager::RegisterPreset(const InputPreset& preset)
    {
        // Replace existing preset with same name
        for (auto& existing : m_presets)
        {
            if (existing.name == preset.name)
            {
                existing = preset;
                return;
            }
        }
        m_presets.push_back(preset);
    }

    bool InputBindingManager::ApplyPreset(const std::string& presetName)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Input);
        SPARK_VALIDATE_RET(Spark::LogCategory::Input, !presetName.empty(), false);
        for (const auto& preset : m_presets)
        {
            if (preset.name == presetName)
            {
                m_bindings = preset.bindings;
                for (const auto& [action, binding] : m_bindings)
                {
                    for (const auto& callback : m_bindingChangedCallbacks)
                    {
                        SPARK_GUARDED_UPDATE("Input:BindingChanged", "Input", { callback(action, binding); });
                    }
                }
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> InputBindingManager::GetPresetNames() const
    {
        std::vector<std::string> names;
        names.reserve(m_presets.size());
        for (const auto& preset : m_presets)
        {
            names.push_back(preset.name);
        }
        return names;
    }

    void InputBindingManager::CreateDefaultPresets()
    {
        // Default FPS preset
        InputPreset defaultPreset;
        defaultPreset.name = "Default";
        defaultPreset.description = "Standard FPS controls (WASD + Mouse)";
        defaultPreset.bindings["MoveForward"] = InputBinding{0x57, 0, -1, 0.0f, false}; // W
        defaultPreset.bindings["MoveBack"] = InputBinding{0x53, 0, -1, 0.0f, false};    // S
        defaultPreset.bindings["MoveLeft"] = InputBinding{0x41, 0, -1, 0.0f, false};    // A
        defaultPreset.bindings["MoveRight"] = InputBinding{0x44, 0, -1, 0.0f, false};   // D
        defaultPreset.bindings["Jump"] = InputBinding{0x20, 0, -1, 0.0f, false};        // Space
        defaultPreset.bindings["Crouch"] = InputBinding{0x11, 0, -1, 0.0f, false};      // Ctrl
        defaultPreset.bindings["Sprint"] = InputBinding{0x10, 0, -1, 0.0f, false};      // Shift
        defaultPreset.bindings["Reload"] = InputBinding{0x52, 0, -1, 0.0f, false};      // R
        defaultPreset.bindings["Interact"] = InputBinding{0x45, 0, -1, 0.0f, false};    // E
        defaultPreset.bindings["Inventory"] = InputBinding{0x49, 0, -1, 0.0f, false};   // I
        RegisterPreset(defaultPreset);

        // Left-handed preset
        InputPreset leftHandPreset;
        leftHandPreset.name = "Left-Handed";
        leftHandPreset.description = "Arrow keys + right-hand mouse";
        leftHandPreset.bindings["MoveForward"] = InputBinding{0x26, 0, -1, 0.0f, false}; // Up arrow
        leftHandPreset.bindings["MoveBack"] = InputBinding{0x28, 0, -1, 0.0f, false};    // Down arrow
        leftHandPreset.bindings["MoveLeft"] = InputBinding{0x25, 0, -1, 0.0f, false};    // Left arrow
        leftHandPreset.bindings["MoveRight"] = InputBinding{0x27, 0, -1, 0.0f, false};   // Right arrow
        leftHandPreset.bindings["Jump"] = InputBinding{0x10, 0, -1, 0.0f, false};        // RShift
        leftHandPreset.bindings["Crouch"] = InputBinding{0x11, 0, -1, 0.0f, false};      // RCtrl
        leftHandPreset.bindings["Sprint"] = InputBinding{0x0D, 0, -1, 0.0f, false};      // Enter
        leftHandPreset.bindings["Reload"] = InputBinding{0xBE, 0, -1, 0.0f, false};      // Period
        leftHandPreset.bindings["Interact"] = InputBinding{0xBF, 0, -1, 0.0f, false};    // Slash
        leftHandPreset.bindings["Inventory"] = InputBinding{0xBC, 0, -1, 0.0f, false};   // Comma
        RegisterPreset(leftHandPreset);
    }

    bool InputBindingManager::SaveToFile(const std::string& filePath) const
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Input);
        SPARK_VALIDATE_RET(Spark::LogCategory::Input, !filePath.empty(), false);
        std::ofstream file(filePath);
        if (!file.is_open())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Input, "Failed to open file for saving bindings: %s", filePath.c_str());
            return false;
        }

        file << "{\n";
        file << "  \"accessibility\": " << static_cast<uint32_t>(m_accessibilityFlags) << ",\n";
        file << "  \"bindings\": {\n";

        bool first = true;
        for (const auto& [action, binding] : m_bindings)
        {
            if (!first)
            {
                file << ",\n";
            }
            first = false;
            file << "    \"" << action << "\": {" << "\"primary\": " << binding.primaryKey << ", "
                 << "\"alternate\": " << binding.alternateKey << ", " << "\"gamepad\": " << binding.gamepadButton
                 << ", " << "\"holdToToggle\": " << (binding.holdToToggle ? "true" : "false") << "}";
        }

        file << "\n  }\n}\n";
        file.flush();
        if (!file.good())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Input, "Error writing bindings to file: %s", filePath.c_str());
            return false;
        }
        return true;
    }

    bool InputBindingManager::LoadFromFile(const std::string& filePath)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Input);
        SPARK_VALIDATE_RET(Spark::LogCategory::Input, !filePath.empty(), false);
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Input, "Failed to open file for loading bindings: %s", filePath.c_str());
            return false;
        }

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        try
        {
            // Parse accessibility flags
            std::regex accessRegex(R"~~("accessibility"\s*:\s*(\d+))~~");
            std::smatch accessMatch;
            if (std::regex_search(content, accessMatch, accessRegex))
            {
                m_accessibilityFlags = static_cast<AccessibilityFlags>(std::stoul(accessMatch[1].str()));
            }

            // Parse bindings
            std::regex bindingRegex(
                R"~~("(\w+)"\s*:\s*\{\s*"primary"\s*:\s*(\d+)\s*,\s*"alternate"\s*:\s*(\d+)\s*,\s*"gamepad"\s*:\s*(-?\d+)\s*,\s*"holdToToggle"\s*:\s*(true|false)\s*\})~~");
            auto begin = std::sregex_iterator(content.begin(), content.end(), bindingRegex);
            auto end = std::sregex_iterator();

            for (auto it = begin; it != end; ++it)
            {
                const std::smatch& match = *it;
                InputBinding binding;
                binding.primaryKey = std::stoi(match[2].str());
                binding.alternateKey = std::stoi(match[3].str());
                binding.gamepadButton = std::stoi(match[4].str());
                binding.holdToToggle = (match[5].str() == "true");
                m_bindings[match[1].str()] = binding;
            }
        }
        catch (const std::exception& e)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Input, "Failed to parse bindings file %s: %s", filePath.c_str(),
                            e.what());
            return false;
        }

        return true;
    }

    void InputBindingManager::OnBindingChanged(std::function<void(const std::string&, const InputBinding&)> callback)
    {
        SPARK_WARN_IF(Spark::LogCategory::Input, !callback, "OnBindingChanged called with null callback");
        m_bindingChangedCallbacks.push_back(std::move(callback));
    }

    std::string InputBindingManager::Console_ListBindings() const
    {
        std::ostringstream oss;
        oss << "=== Input Bindings ===\n";
        for (const auto& [action, binding] : m_bindings)
        {
            oss << "  " << action << ": key=" << binding.primaryKey;
            if (binding.alternateKey != 0)
            {
                oss << " alt=" << binding.alternateKey;
            }
            if (binding.gamepadButton >= 0)
            {
                oss << " gamepad=" << binding.gamepadButton;
            }
            if (binding.holdToToggle)
            {
                oss << " [toggle]";
            }
            oss << "\n";
        }
        return oss.str();
    }

    std::string InputBindingManager::Console_GetAccessibilityStatus() const
    {
        std::ostringstream oss;
        oss << "=== Accessibility Settings ===\n";
        auto flags = static_cast<uint32_t>(m_accessibilityFlags);
        oss << "  HoldToToggle: " << (flags & 1 ? "ON" : "OFF") << "\n";
        oss << "  LargeSubtitles: " << (flags & 2 ? "ON" : "OFF") << "\n";
        oss << "  HighContrastUI: " << (flags & 4 ? "ON" : "OFF") << "\n";
        oss << "  ColorblindProtanopia: " << (flags & 8 ? "ON" : "OFF") << "\n";
        oss << "  ColorblindDeuteranopia: " << (flags & 16 ? "ON" : "OFF") << "\n";
        oss << "  ColorblindTritanopia: " << (flags & 32 ? "ON" : "OFF") << "\n";
        oss << "  ReducedMotion: " << (flags & 64 ? "ON" : "OFF") << "\n";
        oss << "  ScreenNarrator: " << (flags & 128 ? "ON" : "OFF") << "\n";
        oss << "  AutoAim: " << (flags & 256 ? "ON" : "OFF") << "\n";
        oss << "  OneTouchMode: " << (flags & 512 ? "ON" : "OFF") << "\n";
        return oss.str();
    }

} // namespace Spark
