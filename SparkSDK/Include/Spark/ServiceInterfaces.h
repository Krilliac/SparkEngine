/**
 * @file ServiceInterfaces.h
 * @brief Thin service interfaces for context-driven dependency injection.
 */

#pragma once

#include <cstdint>
#include <string_view>

namespace Spark
{

    /** @brief Thin runtime interface for networking lifecycle orchestration. */
    class INetworkService
    {
      public:
        virtual ~INetworkService() = default;
        virtual bool Initialize() = 0;
        virtual void Shutdown() = 0;
        virtual void Update(float deltaTime) = 0;
        virtual bool IsInitialized() const = 0;
    };

    /** @brief Thin runtime interface for telemetry lifecycle orchestration. */
    class ITelemetryService
    {
      public:
        virtual ~ITelemetryService() = default;
        virtual void Update(float deltaTime) = 0;
        virtual void Shutdown() = 0;
        virtual bool IsInitialized() const = 0;
    };

    /** @brief Thin runtime interface for gameplay tag registry orchestration. */
    class IGameplayTagService
    {
      public:
        virtual ~IGameplayTagService() = default;
        virtual void Initialize() = 0;
        virtual void Shutdown() = 0;
        virtual uint32_t RegisterTag(std::string_view fullName) = 0;
        virtual uint32_t GetTagId(std::string_view fullName) const = 0;
    };

} // namespace Spark
