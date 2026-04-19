#pragma once

#include "Config.h"

#include <functional>
#include <string>

namespace SparkInstaller
{
    enum class Mode
    {
        Install,
        Update
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

        LogSink log = nullptr;
    };
} // namespace SparkInstaller
