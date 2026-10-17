#pragma once

#include "Config.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace SparkInstaller
{
    enum class Mode
    {
        Install,
        Update,
        // The destination holds a clone this installer activated whose first
        // configure or build never completed (pending marker, no install state).
        ResumeInstall
    };

    enum class Frontend
    {
        Tui,
        Gui,
        Headless
    };

    using LogSink = std::function<void(const std::string&)>;

    struct InstallerContext
    {
        Frontend frontend = Frontend::Tui;
        Mode mode = Mode::Install;

        std::string destination;
        std::string ref = "Working";
        std::string repoUrl = "https://github.com/Krilliac/SparkEngine.git";

        SparkBuild::ConfigManager configManager;

        bool skipBuild = false;
        bool skipSubmoduleUpdate = false;

        // Free-space floor enforced by preflight. Unset selects
        // Preflight::DefaultMinFreeBytes (measured clone or clone+build budget).
        std::optional<std::uintmax_t> minFreeBytes;

        LogSink log = nullptr;
    };
} // namespace SparkInstaller
