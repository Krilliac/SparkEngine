#pragma once

#include <map>
#include <string>

namespace SparkInstaller
{
    // On-disk record placed inside an installed engine tree at .sparkengine-install.json.
    // Minimal JSON, hand-written — no external dependency.
    struct InstallState
    {
        int schema = 1;
        std::string ref;
        std::string commit;
        std::string destination;
        std::string generator;
        std::string buildType;
        std::string builtAt;
        std::string installerVersion;
        std::map<std::string, bool> options;

        // Filename located at <destination>/.sparkengine-install.json.
        static std::string FileName() { return ".sparkengine-install.json"; }

        static bool Load(const std::string& destination, InstallState& out);
        bool Save(const std::string& destination) const;
        static bool Exists(const std::string& destination);
    };
} // namespace SparkInstaller
