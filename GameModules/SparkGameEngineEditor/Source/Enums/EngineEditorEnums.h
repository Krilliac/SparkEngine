/**
 * @file EngineEditorEnums.h
 * @brief Enumerations for engine/editor no-code game creation tools
 * @author Spark Engine Team
 * @date 2026
 *
 * Contains all enum types used by the engine editor game module: visual
 * scripting node types, level design tools, material parameters, VFX
 * emitter shapes, animation blend modes, UI widget types, prototyping
 * templates, and asset pipeline stages.
 */

#pragma once

#include <cstdint>

namespace EngineEditor
{

    // =====================================================================
    // Visual Scripting
    // =====================================================================

    /// @brief Categories of visual scripting nodes
    enum class NodeCategory : uint8_t
    {
        Event = 0,       ///< Entry points (OnBeginPlay, OnTick, OnCollision)
        Flow = 1,        ///< Branch, Sequence, ForLoop, WhileLoop, Switch
        Math = 2,        ///< Add, Multiply, Lerp, Clamp, Sin, Cos
        Logic = 3,       ///< And, Or, Not, Compare, Select
        Variable = 4,    ///< Get, Set, local/member variables
        Function = 5,    ///< CallFunction, pure functions, macros
        Transform = 6,   ///< GetPosition, SetRotation, LookAt, MoveToward
        Physics = 7,     ///< Raycast, AddForce, SetVelocity, Overlap
        Input = 8,       ///< GetAxis, IsKeyDown, GetMouseDelta
        Animation = 9,   ///< PlayMontage, SetBlendParam, GetBoneTransform
        Audio = 10,      ///< PlaySound, StopSound, SetVolume, SetPitch
        AI = 11,         ///< MoveTo, FindPath, GetPerception, SetBehavior
        UI = 12,         ///< ShowWidget, HideWidget, SetText, BindEvent
        Networking = 13, ///< RPC, Replicate, IsServer, IsClient
        Debug = 14,      ///< PrintString, DrawDebugLine, Breakpoint
        Count = 15
    };

    /// @brief Pin data types for visual scripting connections
    enum class PinType : uint8_t
    {
        Exec = 0, ///< Execution flow (white arrow)
        Bool = 1,
        Int = 2,
        Float = 3,
        String = 4,
        Vector3 = 5,
        Rotator = 6,
        Color = 7,
        Entity = 8,    ///< ECS entity reference
        Component = 9, ///< Component reference
        Array = 10,
        Map = 11,
        Wildcard = 12, ///< Any type (auto-cast)
        Count = 13
    };

    // =====================================================================
    // Level Design
    // =====================================================================

    /// @brief Level design tool modes
    enum class LevelTool : uint8_t
    {
        Select = 0,
        Translate = 1,
        Rotate = 2,
        Scale = 3,
        PrefabPlace = 4,
        TerrainSculpt = 5,
        TerrainPaint = 6,
        FoliagePaint = 7,
        SplinePath = 8,
        VolumeEdit = 9,
        LightPlace = 10,
        DecalPlace = 11,
        Count = 12
    };

    /// @brief Terrain brush shapes
    enum class BrushShape : uint8_t
    {
        Circle = 0,
        Square = 1,
        Smooth = 2,
        Noise = 3,
        Flatten = 4,
        Erode = 5,
        Count = 6
    };

    /// @brief Terrain layer types for painting
    enum class TerrainLayer : uint8_t
    {
        Grass = 0,
        Dirt = 1,
        Rock = 2,
        Sand = 3,
        Snow = 4,
        Mud = 5,
        Gravel = 6,
        Moss = 7,
        Count = 8
    };

    /// @brief Foliage placement density presets
    enum class FoliageDensity : uint8_t
    {
        Sparse = 0,
        Normal = 1,
        Dense = 2,
        Lush = 3,
        Count = 4
    };

    // =====================================================================
    // Material Editor
    // =====================================================================

    /// @brief Material node types in the visual material graph
    enum class MaterialNodeType : uint8_t
    {
        TextureSample = 0,
        ConstantFloat = 1,
        ConstantVector = 2,
        ConstantColor = 3,
        Multiply = 4,
        Add = 5,
        Lerp = 6,
        Fresnel = 7,
        Normal = 8,
        WorldPosition = 9,
        TexCoord = 10,
        Time = 11,
        Panner = 12,
        Noise = 13,
        DotProduct = 14,
        Power = 15,
        Clamp = 16,
        Output = 17, ///< Final material output (albedo, normal, roughness, metallic, AO, emissive)
        Count = 18
    };

    /// @brief Shading models available in material editor
    enum class ShadingModel : uint8_t
    {
        DefaultLit = 0,
        Unlit = 1,
        Subsurface = 2,
        ClearCoat = 3,
        Cloth = 4,
        ThinFilm = 5,
        Hair = 6,
        Eye = 7,
        Count = 8
    };

    /// @brief Material blend modes
    enum class MaterialBlendMode : uint8_t
    {
        Opaque = 0,
        AlphaBlend = 1,
        Additive = 2,
        Modulate = 3,
        Masked = 4,
        Count = 5
    };

    // =====================================================================
    // VFX / Particle Editor
    // =====================================================================

    /// @brief Particle emitter shapes
    enum class EmitterShape : uint8_t
    {
        Point = 0,
        Sphere = 1,
        Hemisphere = 2,
        Cone = 3,
        Box = 4,
        Ring = 5,
        Mesh = 6,
        Edge = 7,
        Count = 8
    };

    /// @brief Particle simulation spaces
    enum class SimulationSpace : uint8_t
    {
        Local = 0,
        World = 1,
        Custom = 2,
        Count = 3
    };

    /// @brief VFX module types composable on each emitter
    enum class VFXModule : uint8_t
    {
        Spawn = 0,
        Lifetime = 1,
        Velocity = 2,
        Acceleration = 3,
        Size = 4,
        Color = 5,
        Rotation = 6,
        Noise = 7,
        Collision = 8,
        SubEmitter = 9,
        Trail = 10,
        Light = 11,
        ForceField = 12,
        Orbit = 13,
        Count = 14
    };

    // =====================================================================
    // Animation Editor
    // =====================================================================

    /// @brief Animation state machine transition conditions
    enum class TransitionCondition : uint8_t
    {
        BoolParam = 0,
        FloatThreshold = 1,
        IntEquals = 2,
        TimeElapsed = 3,
        AnimFinished = 4,
        TriggerParam = 5,
        Count = 6
    };

    /// @brief Blend modes for animation layers
    enum class AnimBlendMode : uint8_t
    {
        Override = 0,
        Additive = 1,
        Layered = 2,
        Count = 3
    };

    /// @brief IK solver types
    enum class IKSolverType : uint8_t
    {
        TwoBone = 0,
        FABRIK = 1,
        CCD = 2,
        LookAt = 3,
        SplineIK = 4,
        Count = 5
    };

    // =====================================================================
    // UI Editor
    // =====================================================================

    /// @brief UI widget types available in the WYSIWYG editor
    enum class WidgetType : uint8_t
    {
        Panel = 0,
        Button = 1,
        Label = 2,
        Image = 3,
        ProgressBar = 4,
        Slider = 5,
        TextField = 6,
        Checkbox = 7,
        Dropdown = 8,
        ListView = 9,
        ScrollBox = 10,
        Canvas = 11,
        Count = 12
    };

    /// @brief UI layout modes
    enum class LayoutMode : uint8_t
    {
        Absolute = 0,
        Horizontal = 1,
        Vertical = 2,
        Grid = 3,
        Wrap = 4,
        Count = 5
    };

    /// @brief UI anchor presets
    enum class AnchorPreset : uint8_t
    {
        TopLeft = 0,
        TopCenter = 1,
        TopRight = 2,
        CenterLeft = 3,
        Center = 4,
        CenterRight = 5,
        BottomLeft = 6,
        BottomCenter = 7,
        BottomRight = 8,
        StretchHorizontal = 9,
        StretchVertical = 10,
        StretchAll = 11,
        Count = 12
    };

    // =====================================================================
    // Prototyping
    // =====================================================================

    /// @brief Pre-built gameplay templates for rapid prototyping
    enum class GameTemplate : uint8_t
    {
        BlankProject = 0,
        FirstPerson = 1,
        ThirdPerson = 2,
        TopDown = 3,
        SideScroller = 4,
        VehicleSim = 5,
        PuzzleGame = 6,
        TwinStick = 7,
        Count = 8
    };

    /// @brief Prototype primitive shapes for quick level blocking
    enum class BlockoutShape : uint8_t
    {
        Cube = 0,
        Cylinder = 1,
        Sphere = 2,
        Ramp = 3,
        Stairs = 4,
        Arch = 5,
        LShape = 6,
        TShape = 7,
        Ring = 8,
        Pipe = 9,
        Count = 10
    };

    // =====================================================================
    // Asset Pipeline
    // =====================================================================

    /// @brief Supported asset import formats
    enum class AssetFormat : uint8_t
    {
        FBX = 0,
        GLTF = 1,
        OBJ = 2,
        PNG = 3,
        TGA = 4,
        HDR = 5,
        WAV = 6,
        OGG = 7,
        TTF = 8,
        JSON = 9,
        Count = 10
    };

    /// @brief Asset processing pipeline stages
    enum class PipelineStage : uint8_t
    {
        Import = 0,
        Validate = 1,
        Optimize = 2,
        Compress = 3,
        Package = 4,
        Deploy = 5,
        Count = 6
    };

    /// @brief LOD generation strategies
    enum class LODStrategy : uint8_t
    {
        ScreenSize = 0,
        Distance = 1,
        Manual = 2,
        Count = 3
    };

} // namespace EngineEditor
