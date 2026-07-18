/**
 * @file VisualScriptNodeTypes.h
 * @brief Node type enumeration for visual script graphs
 *
 * Part of the VisualScriptCompiler umbrella header — include
 * VisualScriptCompiler.h rather than this file directly.
 *
 * @see VisualScriptCompiler.h for the umbrella header
 */

#pragma once

#include <cstdint>

namespace Spark::Scripting
{

    // ========================================================================
    // Node Type Enum
    // ========================================================================

    /**
     * @brief Types of nodes in a visual script graph
     */
    enum class ScriptNodeType : uint32_t
    {
        // Events (entry points — each generates a method in the class)
        OnStart = 0,
        OnUpdate = 1,
        OnTriggerEnter = 2,
        OnTriggerExit = 3,
        OnDamaged = 4,
        OnKeyPress = 5,
        OnCollision = 6,
        OnCustomEvent = 7,

        // Flow control
        Branch = 50,
        ForLoop = 51,
        Sequence = 52,
        DoNothing = 53,

        // Getters (produce data values)
        GetPosition = 100,
        GetRotation = 101,
        GetHealth = 102,
        GetSpeed = 103,
        GetEntityByName = 104,
        GetSelf = 105,
        GetKeyDown = 106,
        GetKey = 107,
        GetDeltaTime = 108,

        // Setters / Actions (execute side effects)
        SetPosition = 150,
        SetRotation = 151,
        SetHealth = 152,
        ApplyForce = 153,
        PlaySound = 154,
        PlayAnimation = 155,
        SpawnEntity = 156,
        DestroyEntity = 157,
        PrintMessage = 158,
        FireEvent = 159,

        // Math
        Add = 200,
        Subtract = 201,
        Multiply = 202,
        Divide = 203,
        Normalize = 204,
        DotProduct = 205,
        Distance = 206,
        Lerp = 207,
        Clamp = 208,
        Random = 209,
        RandomRange = 210,
        Abs = 211,
        Negate = 212,

        // Logic
        And = 250,
        Or = 251,
        Not = 252,
        Equal = 253,
        NotEqual = 254,
        Greater = 255,
        Less = 256,
        GreaterEqual = 257,
        LessEqual = 258,

        // Variables
        GetVariable = 300,
        SetVariable = 301,

        // Constants
        ConstFloat = 350,
        ConstInt = 351,
        ConstBool = 352,
        ConstString = 353,
        ConstVector3 = 354,

        // Custom events & functions (user-defined)
        DefineCustomEvent = 400, ///< Defines a custom event handler method
        CallFunction = 401,      ///< Calls a reusable function sub-graph
        ReturnValue = 402,       ///< Returns a value from a function sub-graph

        // Editor-only (not compiled)
        Comment = 500, ///< Visual comment box for graph organization
    };

} // namespace Spark::Scripting
