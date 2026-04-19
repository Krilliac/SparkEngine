#include "Terminal.h"
#include <iostream>
#include <sstream>

#ifdef SPARK_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace SparkBuild
{
    namespace Term
    {

        void EnableColors()
        {
#ifdef SPARK_PLATFORM_WINDOWS
            // Enable ANSI escape codes on Windows 10+
            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            if (hOut != INVALID_HANDLE_VALUE)
            {
                DWORD mode = 0;
                if (GetConsoleMode(hOut, &mode))
                {
                    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
                    SetConsoleMode(hOut, mode);
                }
            }
#endif
            // Unix terminals support ANSI by default
        }

        std::string Bold(const std::string& text)
        {
            return "\033[1m" + text + "\033[0m";
        }
        std::string Dim(const std::string& text)
        {
            return "\033[2m" + text + "\033[0m";
        }
        std::string Red(const std::string& text)
        {
            return "\033[31m" + text + "\033[0m";
        }
        std::string Green(const std::string& text)
        {
            return "\033[32m" + text + "\033[0m";
        }
        std::string Yellow(const std::string& text)
        {
            return "\033[33m" + text + "\033[0m";
        }
        std::string Blue(const std::string& text)
        {
            return "\033[34m" + text + "\033[0m";
        }
        std::string Cyan(const std::string& text)
        {
            return "\033[36m" + text + "\033[0m";
        }
        std::string Magenta(const std::string& text)
        {
            return "\033[35m" + text + "\033[0m";
        }
        std::string White(const std::string& text)
        {
            return "\033[37m" + text + "\033[0m";
        }

        void ClearScreen()
        {
            std::cout << "\033[2J\033[H" << std::flush;
        }

        int GetTermWidth()
        {
#ifdef SPARK_PLATFORM_WINDOWS
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            if (GetConsoleScreenBufferInfo(hOut, &csbi))
            {
                return csbi.srWindow.Right - csbi.srWindow.Left + 1;
            }
            return 80;
#else
            struct winsize w;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
            {
                return w.ws_col;
            }
            return 80;
#endif
        }

        void PrintDivider(int width)
        {
            if (width <= 0)
                width = GetTermWidth();
            if (width > 120)
                width = 120;
            std::cout << Dim(std::string(width, '-')) << "\n";
        }

        void PrintHeader(const std::string& title, int width)
        {
            if (width <= 0)
                width = GetTermWidth();
            if (width > 120)
                width = 120;

            std::cout << "\n";
            std::cout << Bold(Cyan(std::string(width, '='))) << "\n";

            int pad = (width - (int)title.size() - 2) / 2;
            if (pad < 0)
                pad = 0;
            std::string line = std::string(pad, ' ') + " " + title + " ";
            std::cout << Bold(Cyan(line)) << "\n";

            std::cout << Bold(Cyan(std::string(width, '='))) << "\n";
            std::cout << "\n";
        }

        void PrintSectionHeader(const std::string& title)
        {
            std::cout << "\n" << Bold(Yellow("--- " + title + " ---")) << "\n\n";
        }

        std::string ReadLine(const std::string& prompt)
        {
            std::cout << Bold(prompt) << std::flush;
            std::string line;
            std::getline(std::cin, line);
            return line;
        }

        int ReadInt(const std::string& prompt, int min, int max)
        {
            std::string input = ReadLine(prompt);
            try
            {
                int val = std::stoi(input);
                if (val >= min && val <= max)
                    return val;
            }
            catch (...)
            {
            }
            return -1;
        }

        bool ReadYesNo(const std::string& prompt, bool defaultYes)
        {
            std::string defStr = defaultYes ? " [Y/n] " : " [y/N] ";
            std::string input = ReadLine(prompt + defStr);

            if (input.empty())
                return defaultYes;
            char c = input[0];
            if (c == 'y' || c == 'Y')
                return true;
            if (c == 'n' || c == 'N')
                return false;
            return defaultYes;
        }

        void PrintProgress(const std::string& label, int percent)
        {
            if (percent < 0)
                percent = 0;
            if (percent > 100)
                percent = 100;

            int barWidth = 30;
            int filled = (barWidth * percent) / 100;

            std::string bar = "[";
            for (int i = 0; i < barWidth; i++)
            {
                bar += (i < filled) ? "#" : ".";
            }
            bar += "]";

            std::cout << "\r  " << label << " " << Green(bar) << " " << percent << "%" << std::flush;
            if (percent == 100)
                std::cout << "\n";
        }

        void PrintStatus(const std::string& name, bool ok, const std::string& detail)
        {
            std::string indicator = ok ? Green("[OK]") : Red("[MISSING]");
            std::cout << "  " << indicator << " " << name;
            if (!detail.empty())
            {
                std::cout << " " << Dim("(" + detail + ")");
            }
            std::cout << "\n";
        }

    } // namespace Term
} // namespace SparkBuild
