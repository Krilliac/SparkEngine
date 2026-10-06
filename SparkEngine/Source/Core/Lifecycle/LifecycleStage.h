/**
 * @file LifecycleStage.h
 * @brief Abstract base class for engine lifecycle stages (init, update, shutdown)
 */

#pragma once

#include <cstdint>
#include <string_view>

namespace Spark::Core::Lifecycle
{
    enum class LifecycleThreadAffinity : uint8_t
    {
        MainThread,
        AnyThread
    };

    enum class LifecycleOrder : uint16_t
    {
        /// Debug hooks, logging sinks/levels and detectors must be live before any
        /// other stage initializes, or every SystemPreInit/PostInit hook fired
        /// during networking/gameplay init is a silent no-op.
        Diagnostics = 50,
        Physics = 100,
        Animation = 200,
        AI = 300,
        Audio = 400,
        Lifecycle = 500,
        Render = 600
    };

    class LifecycleStage
    {
      public:
        virtual ~LifecycleStage() = default;

        virtual std::string_view Name() const = 0;
        virtual LifecycleOrder Order() const = 0;
        virtual LifecycleThreadAffinity ThreadAffinity() const = 0;

        virtual bool SupportsInitialize() const { return false; }
        virtual bool SupportsUpdate() const { return false; }
        virtual bool SupportsShutdown() const { return false; }

        /// Bring the stage up. Returning false (or throwing) fails engine startup:
        /// the composition root rolls back every stage already touched and the
        /// process exits non-zero. Report a failure only when the engine cannot
        /// run safely; optional subsystems degrade and return true.
        /// @return true when the stage is ready.
        virtual bool Initialize() { return true; }
        virtual void Update(float /*dt*/) {}
        /// Tear the stage down. Also used to roll back a failed startup, so it
        /// must tolerate a partially initialized stage.
        virtual void Shutdown() {}
    };
} // namespace Spark::Core::Lifecycle
