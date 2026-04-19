#pragma once

#include "Platform.h"
#include <string>

namespace SparkBuild
{
    namespace Term
    {

        // ANSI color codes (work on Windows 10+, Linux, macOS)
        void EnableColors();

        // Colors
        std::string Bold(const std::string& text);
        std::string Dim(const std::string& text);
        std::string Red(const std::string& text);
        std::string Green(const std::string& text);
        std::string Yellow(const std::string& text);
        std::string Blue(const std::string& text);
        std::string Cyan(const std::string& text);
        std::string Magenta(const std::string& text);
        std::string White(const std::string& text);

        // Clear screen
        void ClearScreen();

        // Print a horizontal divider line
        void PrintDivider(int width = 70);

        // Print a centered header with decoration
        void PrintHeader(const std::string& title, int width = 70);

        // Print a boxed section header
        void PrintSectionHeader(const std::string& title);

        // Read a line of input with prompt
        std::string ReadLine(const std::string& prompt = "> ");

        // Read an integer in range [min, max], returns -1 on invalid
        int ReadInt(const std::string& prompt, int min, int max);

        // Read yes/no, returns true for yes
        bool ReadYesNo(const std::string& prompt, bool defaultYes = true);

        // Get terminal width
        int GetTermWidth();

        // Print a progress indicator
        void PrintProgress(const std::string& label, int percent);

        // Print a status line with colored indicator
        void PrintStatus(const std::string& name, bool ok, const std::string& detail = "");

    } // namespace Term
} // namespace SparkBuild
