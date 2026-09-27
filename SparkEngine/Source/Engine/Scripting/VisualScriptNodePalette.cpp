/**
 * @file VisualScriptNodePalette.cpp
 * @brief Node metadata: authoring palette (display names, categories, tooltips) and stable .vscript names
 */

#include "VisualScriptCompiler.h"
#include "VisualScriptGraphIO.h"

#include <array>

namespace Spark::Scripting
{
    namespace
    {
        struct NodeTypeEntry
        {
            ScriptNodeType type;
            const char* name;
        };

        constexpr std::array kNodeTypeNames = {
            NodeTypeEntry{ScriptNodeType::OnStart, "OnStart"},
            NodeTypeEntry{ScriptNodeType::OnUpdate, "OnUpdate"},
            NodeTypeEntry{ScriptNodeType::OnTriggerEnter, "OnTriggerEnter"},
            NodeTypeEntry{ScriptNodeType::OnTriggerExit, "OnTriggerExit"},
            NodeTypeEntry{ScriptNodeType::OnDamaged, "OnDamaged"},
            NodeTypeEntry{ScriptNodeType::OnKeyPress, "OnKeyPress"},
            NodeTypeEntry{ScriptNodeType::OnCollision, "OnCollision"},
            NodeTypeEntry{ScriptNodeType::OnCustomEvent, "OnCustomEvent"},
            NodeTypeEntry{ScriptNodeType::Branch, "Branch"},
            NodeTypeEntry{ScriptNodeType::ForLoop, "ForLoop"},
            NodeTypeEntry{ScriptNodeType::Sequence, "Sequence"},
            NodeTypeEntry{ScriptNodeType::DoNothing, "DoNothing"},
            NodeTypeEntry{ScriptNodeType::GetPosition, "GetPosition"},
            NodeTypeEntry{ScriptNodeType::GetRotation, "GetRotation"},
            NodeTypeEntry{ScriptNodeType::GetHealth, "GetHealth"},
            NodeTypeEntry{ScriptNodeType::GetSpeed, "GetSpeed"},
            NodeTypeEntry{ScriptNodeType::GetEntityByName, "GetEntityByName"},
            NodeTypeEntry{ScriptNodeType::GetSelf, "GetSelf"},
            NodeTypeEntry{ScriptNodeType::GetKeyDown, "GetKeyDown"},
            NodeTypeEntry{ScriptNodeType::GetKey, "GetKey"},
            NodeTypeEntry{ScriptNodeType::GetDeltaTime, "GetDeltaTime"},
            NodeTypeEntry{ScriptNodeType::SetPosition, "SetPosition"},
            NodeTypeEntry{ScriptNodeType::SetRotation, "SetRotation"},
            NodeTypeEntry{ScriptNodeType::SetHealth, "SetHealth"},
            NodeTypeEntry{ScriptNodeType::ApplyForce, "ApplyForce"},
            NodeTypeEntry{ScriptNodeType::PlaySound, "PlaySound"},
            NodeTypeEntry{ScriptNodeType::PlayAnimation, "PlayAnimation"},
            NodeTypeEntry{ScriptNodeType::SpawnEntity, "SpawnEntity"},
            NodeTypeEntry{ScriptNodeType::DestroyEntity, "DestroyEntity"},
            NodeTypeEntry{ScriptNodeType::PrintMessage, "PrintMessage"},
            NodeTypeEntry{ScriptNodeType::FireEvent, "FireEvent"},
            NodeTypeEntry{ScriptNodeType::Add, "Add"},
            NodeTypeEntry{ScriptNodeType::Subtract, "Subtract"},
            NodeTypeEntry{ScriptNodeType::Multiply, "Multiply"},
            NodeTypeEntry{ScriptNodeType::Divide, "Divide"},
            NodeTypeEntry{ScriptNodeType::Normalize, "Normalize"},
            NodeTypeEntry{ScriptNodeType::DotProduct, "DotProduct"},
            NodeTypeEntry{ScriptNodeType::Distance, "Distance"},
            NodeTypeEntry{ScriptNodeType::Lerp, "Lerp"},
            NodeTypeEntry{ScriptNodeType::Clamp, "Clamp"},
            NodeTypeEntry{ScriptNodeType::Random, "Random"},
            NodeTypeEntry{ScriptNodeType::RandomRange, "RandomRange"},
            NodeTypeEntry{ScriptNodeType::Abs, "Abs"},
            NodeTypeEntry{ScriptNodeType::Negate, "Negate"},
            NodeTypeEntry{ScriptNodeType::BreakVector3, "BreakVector3"},
            NodeTypeEntry{ScriptNodeType::MakeVector3, "MakeVector3"},
            NodeTypeEntry{ScriptNodeType::ToInt, "ToInt"},
            NodeTypeEntry{ScriptNodeType::AppendString, "AppendString"},
            NodeTypeEntry{ScriptNodeType::And, "And"},
            NodeTypeEntry{ScriptNodeType::Or, "Or"},
            NodeTypeEntry{ScriptNodeType::Not, "Not"},
            NodeTypeEntry{ScriptNodeType::Equal, "Equal"},
            NodeTypeEntry{ScriptNodeType::NotEqual, "NotEqual"},
            NodeTypeEntry{ScriptNodeType::Greater, "Greater"},
            NodeTypeEntry{ScriptNodeType::Less, "Less"},
            NodeTypeEntry{ScriptNodeType::GreaterEqual, "GreaterEqual"},
            NodeTypeEntry{ScriptNodeType::LessEqual, "LessEqual"},
            NodeTypeEntry{ScriptNodeType::Select, "Select"},
            NodeTypeEntry{ScriptNodeType::GetVariable, "GetVariable"},
            NodeTypeEntry{ScriptNodeType::SetVariable, "SetVariable"},
            NodeTypeEntry{ScriptNodeType::ConstFloat, "ConstFloat"},
            NodeTypeEntry{ScriptNodeType::ConstInt, "ConstInt"},
            NodeTypeEntry{ScriptNodeType::ConstBool, "ConstBool"},
            NodeTypeEntry{ScriptNodeType::ConstString, "ConstString"},
            NodeTypeEntry{ScriptNodeType::ConstVector3, "ConstVector3"},
            NodeTypeEntry{ScriptNodeType::DefineCustomEvent, "DefineCustomEvent"},
            NodeTypeEntry{ScriptNodeType::CallFunction, "CallFunction"},
            NodeTypeEntry{ScriptNodeType::ReturnValue, "ReturnValue"},
            NodeTypeEntry{ScriptNodeType::Comment, "Comment"},
        };

        constexpr std::array<const char*, 8> kPinKindNames = {"Execution", "Bool",    "Int",    "Float",
                                                              "String",    "Vector3", "Entity", "Any"};

        const std::vector<ScriptNodePaletteEntry>& BuildNodePalette()
        {
            static const std::vector<ScriptNodePaletteEntry> kPalette = {
                {ScriptNodeType::OnStart, "On Start", "Events", "Entry point called once when script starts"},
                {ScriptNodeType::OnUpdate, "On Update", "Events", "Entry point called every frame"},
                {ScriptNodeType::OnTriggerEnter, "On Trigger Enter", "Events", "Entry point for trigger overlap enter"},
                {ScriptNodeType::OnTriggerExit, "On Trigger Exit", "Events", "Entry point for trigger overlap exit"},
                {ScriptNodeType::OnDamaged, "On Damaged", "Events", "Entry point when entity receives damage"},
                {ScriptNodeType::OnKeyPress, "On Key Press", "Events", "Entry point when a key is pressed"},
                {ScriptNodeType::OnCollision, "On Collision", "Events", "Entry point for physics collision"},
                {ScriptNodeType::OnCustomEvent, "On Custom Event", "Events", "Entry point for a named custom event"},
                {ScriptNodeType::Branch, "Branch (If)", "Flow Control", "Executes True or False branch by condition"},
                {ScriptNodeType::ForLoop, "For Loop", "Flow Control", "Iterates an integer range"},
                {ScriptNodeType::Sequence, "Sequence", "Flow Control", "Executes outputs in order"},
                {ScriptNodeType::DoNothing, "Do Nothing", "Flow Control", "Pass-through execution node"},
                {ScriptNodeType::SetPosition, "Set Position", "Actions", "Set entity position"},
                {ScriptNodeType::SetRotation, "Set Rotation", "Actions", "Set entity rotation"},
                {ScriptNodeType::SetHealth, "Set Health", "Actions", "Set entity health"},
                {ScriptNodeType::ApplyForce, "Apply Force", "Actions", "Apply force to entity"},
                {ScriptNodeType::PlaySound, "Play Sound", "Actions", "Play an audio event"},
                {ScriptNodeType::PlayAnimation, "Play Animation", "Actions", "Play an animation clip"},
                {ScriptNodeType::SpawnEntity, "Spawn Entity", "Actions", "Spawn a new entity"},
                {ScriptNodeType::DestroyEntity, "Destroy Entity", "Actions", "Destroy target entity"},
                {ScriptNodeType::PrintMessage, "Print Message", "Actions", "Print to script console"},
                {ScriptNodeType::FireEvent, "Fire Event", "Actions", "Fire a gameplay/custom event"},
                {ScriptNodeType::GetPosition, "Get Position", "Getters", "Get entity position"},
                {ScriptNodeType::GetRotation, "Get Rotation", "Getters", "Get entity rotation"},
                {ScriptNodeType::GetHealth, "Get Health", "Getters", "Get entity health"},
                {ScriptNodeType::GetSpeed, "Get Speed", "Getters", "Get entity speed"},
                {ScriptNodeType::GetEntityByName, "Get Entity By Name", "Getters", "Find entity by name"},
                {ScriptNodeType::GetSelf, "Get Self", "Getters", "Get current script owner entity"},
                {ScriptNodeType::GetKeyDown, "Get Key Down", "Getters", "True on key pressed this frame"},
                {ScriptNodeType::GetKey, "Get Key Held", "Getters", "True while key is held"},
                {ScriptNodeType::GetDeltaTime, "Get Delta Time", "Getters", "Frame delta time in seconds"},
                {ScriptNodeType::Add, "Add", "Math", "Add two float values"},
                {ScriptNodeType::Subtract, "Subtract", "Math", "Subtract second float from first"},
                {ScriptNodeType::Multiply, "Multiply", "Math", "Multiply two float values"},
                {ScriptNodeType::Divide, "Divide", "Math", "Divide first float by second"},
                {ScriptNodeType::Normalize, "Normalize", "Math", "Normalize a Vector3"},
                {ScriptNodeType::DotProduct, "Dot Product", "Math", "Vector3 dot product"},
                {ScriptNodeType::Distance, "Distance", "Math", "Distance between two Vector3 points"},
                {ScriptNodeType::Lerp, "Lerp", "Math", "Linear interpolate between values"},
                {ScriptNodeType::Clamp, "Clamp", "Math", "Clamp a value to min/max"},
                {ScriptNodeType::Random, "Random", "Math", "Random float in [0..1]"},
                {ScriptNodeType::RandomRange, "Random Range", "Math", "Random float in [min..max]"},
                {ScriptNodeType::Abs, "Abs", "Math", "Absolute float value"},
                {ScriptNodeType::Negate, "Negate", "Math", "Negate a float value"},
                {ScriptNodeType::BreakVector3, "Break Vector3", "Math", "Split a Vector3 into x, y and z"},
                {ScriptNodeType::MakeVector3, "Make Vector3", "Math", "Build a Vector3 from x, y and z"},
                {ScriptNodeType::ToInt, "To Int", "Math", "Convert a float to an int (truncates toward zero)"},
                {ScriptNodeType::And, "AND", "Logic", "Logical AND"},
                {ScriptNodeType::Or, "OR", "Logic", "Logical OR"},
                {ScriptNodeType::Not, "NOT", "Logic", "Logical NOT"},
                {ScriptNodeType::Equal, "Equal", "Logic", "Float equality comparison"},
                {ScriptNodeType::NotEqual, "Not Equal", "Logic", "Float inequality comparison"},
                {ScriptNodeType::Greater, "Greater", "Logic", "Greater-than comparison"},
                {ScriptNodeType::Less, "Less", "Logic", "Less-than comparison"},
                {ScriptNodeType::GreaterEqual, "Greater Equal", "Logic", "Greater-than-or-equal comparison"},
                {ScriptNodeType::LessEqual, "Less Equal", "Logic", "Less-than-or-equal comparison"},
                {ScriptNodeType::Select, "Select", "Logic", "A when the condition is true, otherwise B"},
                {ScriptNodeType::AppendString, "Append", "Text", "Append a value's text to a string"},
                {ScriptNodeType::GetVariable, "Get Variable", "Variables", "Read a graph variable"},
                {ScriptNodeType::SetVariable, "Set Variable", "Variables", "Write a graph variable"},
                {ScriptNodeType::ConstFloat, "Float", "Constants", "Float constant"},
                {ScriptNodeType::ConstInt, "Int", "Constants", "Integer constant"},
                {ScriptNodeType::ConstBool, "Bool", "Constants", "Boolean constant"},
                {ScriptNodeType::ConstString, "String", "Constants", "String constant"},
                {ScriptNodeType::ConstVector3, "Vector3", "Constants", "Vector3 constant"},
                {ScriptNodeType::DefineCustomEvent, "Custom Event", "Functions", "Define a custom event handler"},
                {ScriptNodeType::CallFunction, "Call Function", "Functions", "Call a reusable function graph"},
                {ScriptNodeType::ReturnValue, "Return Value", "Functions", "Return from a function graph"},
                {ScriptNodeType::Comment, "Comment", "Functions", "Editor-only note box"}};
            return kPalette;
        }
    } // namespace

    const std::vector<ScriptNodePaletteEntry>& VisualScriptCompiler::GetNodePalette()
    {
        return BuildNodePalette();
    }

    const char* VisualScriptCompiler::GetNodeDisplayName(ScriptNodeType type)
    {
        for (const auto& entry : BuildNodePalette())
        {
            if (entry.type == type)
            {
                return entry.displayName;
            }
        }
        return "Unknown";
    }

    const char* VisualScriptCompiler::GetNodeCategory(ScriptNodeType type)
    {
        for (const auto& entry : BuildNodePalette())
        {
            if (entry.type == type)
            {
                return entry.category;
            }
        }
        return "Misc";
    }

    const char* VisualScriptGraphIO::NodeTypeName(ScriptNodeType type)
    {
        for (const auto& entry : kNodeTypeNames)
        {
            if (entry.type == type)
                return entry.name;
        }
        return nullptr;
    }

    std::optional<ScriptNodeType> VisualScriptGraphIO::NodeTypeFromName(std::string_view name)
    {
        for (const auto& entry : kNodeTypeNames)
        {
            if (name == entry.name)
                return entry.type;
        }
        return std::nullopt;
    }

    const char* VisualScriptGraphIO::PinKindName(PinKind kind)
    {
        const auto index = static_cast<size_t>(kind);
        return index < kPinKindNames.size() ? kPinKindNames[index] : "Unknown";
    }

    std::optional<PinKind> VisualScriptGraphIO::PinKindFromName(std::string_view name)
    {
        for (size_t i = 0; i < kPinKindNames.size(); ++i)
        {
            if (name == kPinKindNames[i])
                return static_cast<PinKind>(i);
        }
        return std::nullopt;
    }

    bool VisualScriptGraphIO::ArePinKindsCompatible(PinKind from, PinKind to)
    {
        if (from == PinKind::Execution || to == PinKind::Execution)
            return from == to;
        if (from == to || from == PinKind::Any || to == PinKind::Any)
            return true;
        return (from == PinKind::Int && to == PinKind::Float) || (from == PinKind::Float && to == PinKind::Int);
    }

} // namespace Spark::Scripting
