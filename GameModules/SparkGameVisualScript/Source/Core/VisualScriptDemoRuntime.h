/**
 * @file VisualScriptDemoRuntime.h
 * @brief Deterministic manifest and source binding helpers for the visual-script demo.
 */

#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Spark::VisualScriptDemo
{
    enum class RuntimeSupport
    {
        Ready,
        MissingCompiledSupport,
        MissingWorld,
        MissingScriptEngine,
    };

#ifdef SPARK_ANGELSCRIPT_SUPPORT
    inline constexpr bool AngelScriptCompiledIn = true;
#else
    inline constexpr bool AngelScriptCompiledIn = false;
#endif

    /** Evaluate the module's runtime prerequisites in a deterministic order. */
    inline constexpr RuntimeSupport EvaluateRuntimeSupport(bool compiledIn, bool hasWorld, bool hasScriptEngine)
    {
        if (!compiledIn)
            return RuntimeSupport::MissingCompiledSupport;
        if (!hasWorld)
            return RuntimeSupport::MissingWorld;
        if (!hasScriptEngine)
            return RuntimeSupport::MissingScriptEngine;
        return RuntimeSupport::Ready;
    }

    /** User-facing load contract; unsupported builds never advertise vs_* commands. */
    inline constexpr std::string_view RuntimeSupportMessage(RuntimeSupport support)
    {
        switch (support)
        {
        case RuntimeSupport::Ready:
            return "AngelScript runtime is ready";
        case RuntimeSupport::MissingCompiledSupport:
            return "AngelScript support is not compiled into SparkEngine; configure with "
                   "-DENABLE_ANGELSCRIPT=ON and provide the vendored SDK. Module load rejected; vs_* commands "
                   "are unavailable";
        case RuntimeSupport::MissingWorld:
            return "A live World is required. Module load rejected; vs_* commands are unavailable";
        case RuntimeSupport::MissingScriptEngine:
            return "An initialized AngelScript engine is required. Module load rejected; vs_* commands are "
                   "unavailable";
        }
        return "Unknown visual-script runtime state";
    }

    struct ScriptAsset
    {
        std::string_view className;
        std::string_view fileName;
        uint32_t instanceCount;
    };

    inline constexpr std::array<ScriptAsset, 5> ScriptManifest = {
        ScriptAsset{"PlayerController", "PlayerController.as", 1}, ScriptAsset{"Collectible", "Collectible.as", 5},
        ScriptAsset{"EnemyPatrol", "EnemyPatrol.as", 3},           ScriptAsset{"GameManager", "GameManager.as", 1},
        ScriptAsset{"HealthPickup", "HealthPickup.as", 1},
    };

    inline constexpr uint32_t ExpectedEntityCount = 11;
    inline constexpr std::string_view SelfEntityDeclaration = "uint selfEntity = 0;";

    using IsRegularFile = std::function<bool(const std::filesystem::path&)>;

    /** Select the first root containing the complete manifest, never a partial/duplicate mix. */
    inline std::optional<std::filesystem::path> SelectCompleteScriptRoot(
        std::span<const std::filesystem::path> candidates, const IsRegularFile& isRegularFile)
    {
        for (const auto& root : candidates)
        {
            bool complete = true;
            for (const auto& asset : ScriptManifest)
            {
                if (!isRegularFile(root / std::filesystem::path(asset.fileName)))
                {
                    complete = false;
                    break;
                }
            }
            if (complete)
                return root;
        }
        return std::nullopt;
    }

    /** Bind the generated script's single selfEntity declaration to its ECS entity. */
    inline std::optional<std::string> BindSelfEntity(std::string_view source, uint32_t entityId)
    {
        const size_t declaration = source.find(SelfEntityDeclaration);
        if (declaration == std::string_view::npos ||
            source.find(SelfEntityDeclaration, declaration + SelfEntityDeclaration.size()) != std::string_view::npos)
        {
            return std::nullopt;
        }

        std::string bound(source);
        bound.replace(declaration, SelfEntityDeclaration.size(), "uint selfEntity = " + std::to_string(entityId) + ";");
        return bound;
    }

    /** Reject invalid frame deltas and bound hitches so scripts remain deterministic and responsive. */
    inline float SanitizeDeltaTime(float deltaTime)
    {
        if (!std::isfinite(deltaTime) || deltaTime <= 0.0f)
            return 0.0f;
        return (std::min)(deltaTime, 0.1f);
    }
} // namespace Spark::VisualScriptDemo
