// ScopedLoggerBaseline.h - restore the process-wide Logger to the state
// TestMain established, after a test has reconfigured it.
//
// The Logger is a singleton shared by every test in the process. Tests that
// exercise it (sink installation, level filtering) legitimately call
// ClearSinks(), Initialize(), SetGlobalLevel(Trace) and Shutdown(); if they
// return without undoing that, every later test runs with whatever they left
// behind. Under CI's shuffled order that leaked Trace level once made the
// 100,000-entity load test emit 150,000 trace lines, overrun the sanitizer
// runner's 16 MiB console cap, and take three Linux lanes down with it.
//
// Declare one of these as the first line of any test that mutates the Logger.
// Its destructor runs on every exit path (return, ASSERT, SKIP_TEST, throw).
#pragma once

#include "Utils/Logger.h"

#include <memory>

struct ScopedLoggerBaseline
{
    ScopedLoggerBaseline() { Restore(); }
    ScopedLoggerBaseline(const ScopedLoggerBaseline&) = delete;
    ScopedLoggerBaseline& operator=(const ScopedLoggerBaseline&) = delete;

    ~ScopedLoggerBaseline() { Restore(); }

  private:
    static void Restore()
    {
        // Mirrors TestMain: synchronous logger, exactly one stderr sink, Debug
        // level, automatic stack traces off. Keep the two in step.
        auto& logger = Spark::Logger::Get();
        logger.ClearSinks();
        logger.Shutdown();
        logger.Initialize(false);
        logger.AddSink(std::make_unique<Spark::StderrSink>());
        logger.SetCategoryMask(Spark::kLogCategoryAll);
        logger.SetGlobalLevel(Spark::LogLevel::Debug);
        logger.SetStackTraceLevel(Spark::LogLevel::Off);
    }
};
