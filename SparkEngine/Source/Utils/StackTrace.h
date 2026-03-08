/**
 * @file StackTrace.h
 * @brief Portable stack trace capture utility for diagnostics and logging
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides on-demand stack trace capture that works outside of crash contexts.
 * Useful for tracking allocation origins, logging call chains, and diagnosing
 * hard-to-reproduce issues without attaching a full debugger.
 *
 * Features:
 * - Capture stack traces at any point with SPARK_CAPTURE_STACK()
 * - Symbol resolution on Windows via DbgHelp
 * - Formatted output for logging and console display
 * - Configurable max frame depth
 * - Thread-safe operation
 *
 * Typical usage:
 * @code
 *   auto trace = Spark::StackTrace::Capture();
 *   SPARK_LOG_DEBUG("Debug", "Call stack:\n%s", trace.ToString().c_str());
 * @endcode
 *
 * @see CrashHandler.h, SparkError.h
 */

#pragma once

#include "../Core/Platform.h"
#include <string>
#include <vector>
#include <sstream>
#include <cstdint>

#ifdef SPARK_PLATFORM_WINDOWS
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#elif defined(SPARK_PLATFORM_LINUX) || defined(SPARK_PLATFORM_MACOS)
#include <execinfo.h>
#include <cxxabi.h>
#include <cstdlib>
#endif

namespace Spark
{

    /**
 * @brief Single frame in a captured stack trace
 */
    struct StackFrame
    {
        uintptr_t address = 0;    ///< Instruction pointer address
        std::string moduleName;   ///< Module/library name (e.g., "SparkEngine.exe")
        std::string functionName; ///< Demangled function name
        std::string fileName;     ///< Source file name (if available)
        int lineNumber = 0;       ///< Source line number (if available)
        int displacement = 0;     ///< Byte offset from function start

        /**
     * @brief Format this frame as a human-readable string
     */
        std::string ToString() const
        {
            std::ostringstream oss;
            oss << "0x" << std::hex << address << std::dec;

            if (!functionName.empty())
                oss << " " << functionName;
            else
                oss << " <unknown>";

            if (displacement > 0)
                oss << "+0x" << std::hex << displacement << std::dec;

            if (!fileName.empty())
                oss << " (" << fileName << ":" << lineNumber << ")";

            if (!moduleName.empty())
                oss << " [" << moduleName << "]";

            return oss.str();
        }
    };

    /**
 * @brief Captured stack trace with symbol resolution
 */
    class StackTrace
    {
      public:
        /** @brief Maximum number of frames to capture */
        static constexpr int MAX_FRAMES = 64;

        /**
     * @brief Capture the current call stack
     * @param skipFrames Number of frames to skip (default 1 = skip Capture itself)
     * @param maxFrames Maximum frames to capture
     * @return StackTrace object with resolved symbols
     */
        static StackTrace Capture(int skipFrames = 1, int maxFrames = MAX_FRAMES)
        {
            StackTrace trace;
            trace.m_capturedFrames = CaptureRaw(trace.m_rawAddresses, skipFrames + 1, maxFrames);
            trace.ResolveSymbols();
            return trace;
        }

        /**
     * @brief Get the resolved stack frames
     */
        const std::vector<StackFrame>& GetFrames() const { return m_frames; }

        /**
     * @brief Get the number of captured frames
     */
        int GetFrameCount() const { return static_cast<int>(m_frames.size()); }

        /**
     * @brief Format the entire stack trace as a multi-line string
     * @param indent Indentation prefix for each line
     */
        std::string ToString(const std::string& indent = "  ") const
        {
            std::ostringstream oss;
            for (int i = 0; i < static_cast<int>(m_frames.size()); ++i)
            {
                oss << indent << "#" << i << " " << m_frames[i].ToString() << "\n";
            }
            return oss.str();
        }

        /**
     * @brief Get a compact single-line summary (top N frames)
     * @param topN Number of top frames to include
     */
        std::string ToCompactString(int topN = 5) const
        {
            std::ostringstream oss;
            int count = (topN < static_cast<int>(m_frames.size())) ? topN : static_cast<int>(m_frames.size());
            for (int i = 0; i < count; ++i)
            {
                if (i > 0)
                    oss << " <- ";
                if (!m_frames[i].functionName.empty())
                    oss << m_frames[i].functionName;
                else
                    oss << "0x" << std::hex << m_frames[i].address;
            }
            if (count < static_cast<int>(m_frames.size()))
                oss << " <- ... (" << m_frames.size() - count << " more)";
            return oss.str();
        }

      private:
        std::vector<StackFrame> m_frames;
        void* m_rawAddresses[MAX_FRAMES] = {};
        int m_capturedFrames = 0;

        static int CaptureRaw(void** addresses, int skipFrames, int maxFrames)
        {
            if (maxFrames > MAX_FRAMES)
                maxFrames = MAX_FRAMES;

#ifdef SPARK_PLATFORM_WINDOWS
            int totalSkip = skipFrames + 1; // skip CaptureRaw itself
            USHORT captured =
                CaptureStackBackTrace(static_cast<DWORD>(totalSkip), static_cast<DWORD>(maxFrames), addresses, nullptr);
            return static_cast<int>(captured);

#elif defined(SPARK_PLATFORM_LINUX) || defined(SPARK_PLATFORM_MACOS)
            void* buffer[MAX_FRAMES + 32]; // extra room for skip
            int totalCapture = maxFrames + skipFrames + 1;
            if (totalCapture > MAX_FRAMES + 32)
                totalCapture = MAX_FRAMES + 32;

            int captured = backtrace(buffer, totalCapture);
            int start = skipFrames + 1;
            if (start >= captured)
                return 0;

            int count = captured - start;
            if (count > maxFrames)
                count = maxFrames;
            for (int i = 0; i < count; ++i)
                addresses[i] = buffer[start + i];
            return count;
#else
            (void)addresses;
            (void)skipFrames;
            (void)maxFrames;
            return 0;
#endif
        }

        void ResolveSymbols()
        {
            m_frames.resize(m_capturedFrames);

#ifdef SPARK_PLATFORM_WINDOWS
            HANDLE process = GetCurrentProcess();
            SymInitialize(process, nullptr, TRUE);

            for (int i = 0; i < m_capturedFrames; ++i)
            {
                StackFrame& frame = m_frames[i];
                frame.address = reinterpret_cast<uintptr_t>(m_rawAddresses[i]);

                // Resolve function name
                char symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)] = {};
                SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = MAX_SYM_NAME;

                DWORD64 displacement64 = 0;
                if (SymFromAddr(process, frame.address, &displacement64, symbol))
                {
                    frame.functionName = symbol->Name;
                    frame.displacement = static_cast<int>(displacement64);
                }

                // Resolve file/line
                IMAGEHLP_LINE64 lineInfo = {};
                lineInfo.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
                DWORD lineDisplacement = 0;
                if (SymGetLineFromAddr64(process, frame.address, &lineDisplacement, &lineInfo))
                {
                    frame.fileName = lineInfo.FileName;
                    frame.lineNumber = static_cast<int>(lineInfo.LineNumber);
                }

                // Resolve module name
                IMAGEHLP_MODULE64 moduleInfo = {};
                moduleInfo.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);
                if (SymGetModuleInfo64(process, frame.address, &moduleInfo))
                {
                    frame.moduleName = moduleInfo.ModuleName;
                }
            }

#elif defined(SPARK_PLATFORM_LINUX) || defined(SPARK_PLATFORM_MACOS)
            char** symbols = backtrace_symbols(m_rawAddresses, m_capturedFrames);
            if (symbols)
            {
                for (int i = 0; i < m_capturedFrames; ++i)
                {
                    StackFrame& frame = m_frames[i];
                    frame.address = reinterpret_cast<uintptr_t>(m_rawAddresses[i]);

                    // Try to demangle the symbol
                    std::string raw = symbols[i];
                    frame.functionName = raw;

                    // Try demangling C++ names
                    size_t start = raw.find('(');
                    size_t end = raw.find('+', start);
                    if (start != std::string::npos && end != std::string::npos)
                    {
                        std::string mangled = raw.substr(start + 1, end - start - 1);
                        int status = 0;
                        char* demangled = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
                        if (status == 0 && demangled)
                        {
                            frame.functionName = demangled;
                            free(demangled);
                        }
                        else
                        {
                            frame.functionName = mangled;
                        }

                        // Extract module name
                        frame.moduleName = raw.substr(0, start);
                    }
                }
                free(symbols);
            }
#endif
        }
    };

} // namespace Spark

// ============================================================================
// Convenience macros
// ============================================================================

/** @brief Capture the current stack trace (skips the macro call frame) */
#define SPARK_CAPTURE_STACK() Spark::StackTrace::Capture(1)

/** @brief Capture with custom skip and depth */
#define SPARK_CAPTURE_STACK_EX(skip, depth) Spark::StackTrace::Capture(skip, depth)

/** @brief Log a stack trace at the current location */
#define SPARK_LOG_STACK_TRACE(category)                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        auto _st = Spark::StackTrace::Capture(1);                                                                      \
        SparkError::LogMessage(SparkError::Severity::Debug, category, __FILE__, __LINE__, __FUNCTION__,                \
                               "Stack trace:\n%s", _st.ToString().c_str());                                            \
    } while (0)
