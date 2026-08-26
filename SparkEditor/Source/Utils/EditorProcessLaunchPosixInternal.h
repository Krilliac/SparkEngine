#pragma once

/**
 * @file EditorProcessLaunchPosixInternal.h
 * @brief Internal process-handle, UTF-8, parsing, and wait helpers for the POSIX backend.
 */

struct PosixProcessHandle
{
    pid_t pid = -1;
    pid_t processGroup = -1;
    bool exited = false;
    bool terminationRequested = false;
    unsigned long exitCode = 0;
};

bool EncodeUtf8(std::wstring_view input, std::string& output)
{
    output.clear();
    for (size_t index = 0; index < input.size(); ++index)
    {
        uint32_t codePoint = static_cast<uint32_t>(input[index]);
#if WCHAR_MAX <= 0xffff
        if (codePoint >= 0xd800u && codePoint <= 0xdbffu)
        {
            if (index + 1 >= input.size())
                return false;
            const uint32_t low = static_cast<uint32_t>(input[++index]);
            if (low < 0xdc00u || low > 0xdfffu)
                return false;
            codePoint = 0x10000u + ((codePoint - 0xd800u) << 10u) + (low - 0xdc00u);
        }
        else if (codePoint >= 0xdc00u && codePoint <= 0xdfffu)
        {
            return false;
        }
#else
        if ((codePoint >= 0xd800u && codePoint <= 0xdfffu) || codePoint > 0x10ffffu)
            return false;
#endif

        if (codePoint < 0x80u)
        {
            output.push_back(static_cast<char>(codePoint));
        }
        else if (codePoint < 0x800u)
        {
            output.push_back(static_cast<char>(0xc0u | (codePoint >> 6u)));
            output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
        }
        else if (codePoint < 0x10000u)
        {
            output.push_back(static_cast<char>(0xe0u | (codePoint >> 12u)));
            output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
        }
        else
        {
            output.push_back(static_cast<char>(0xf0u | (codePoint >> 18u)));
            output.push_back(static_cast<char>(0x80u | ((codePoint >> 12u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
        }
    }
    return true;
}

bool ParsePosixCommandLine(std::wstring_view commandLine, std::vector<std::wstring>& arguments, std::string& error)
{
    enum class QuoteMode
    {
        None,
        Single,
        Double
    };

    arguments.clear();
    std::wstring current;
    QuoteMode quote = QuoteMode::None;
    bool escaped = false;
    bool tokenStarted = false;
    for (const wchar_t character : commandLine)
    {
        if (escaped)
        {
            current.push_back(character);
            escaped = false;
            tokenStarted = true;
            continue;
        }
        if (quote != QuoteMode::Single && character == L'\\')
        {
            escaped = true;
            tokenStarted = true;
            continue;
        }
        if (quote == QuoteMode::None &&
            (character == L' ' || character == L'\t' || character == L'\r' || character == L'\n'))
        {
            if (tokenStarted)
            {
                arguments.push_back(std::move(current));
                current.clear();
                tokenStarted = false;
            }
            continue;
        }
        if (character == L'\'' && quote != QuoteMode::Double)
        {
            quote = quote == QuoteMode::Single ? QuoteMode::None : QuoteMode::Single;
            tokenStarted = true;
            continue;
        }
        if (character == L'"' && quote != QuoteMode::Single)
        {
            quote = quote == QuoteMode::Double ? QuoteMode::None : QuoteMode::Double;
            tokenStarted = true;
            continue;
        }
        current.push_back(character);
        tokenStarted = true;
    }

    if (escaped || quote != QuoteMode::None)
    {
        error = "Launch failed: malformed POSIX command line";
        return false;
    }
    if (tokenStarted)
        arguments.push_back(std::move(current));
    if (arguments.empty())
    {
        error = "Launch failed: command line contains no arguments";
        return false;
    }
    return true;
}

unsigned long PosixExitCode(int status)
{
    if (WIFEXITED(status))
        return static_cast<unsigned long>(WEXITSTATUS(status));
    if (WIFSIGNALED(status))
        return static_cast<unsigned long>(128 + WTERMSIG(status));
    return 1;
}

bool PollPosixProcess(PosixProcessHandle& handle, unsigned long& exitCode)
{
    if (handle.exited)
    {
        exitCode = handle.exitCode;
        return true;
    }

    int status = 0;
    pid_t waited = -1;
    do
    {
        waited = waitpid(handle.pid, &status, WNOHANG);
    } while (waited < 0 && errno == EINTR);
    if (waited != handle.pid)
        return false;

    handle.exited = true;
    handle.exitCode = PosixExitCode(status);
    exitCode = handle.exitCode;
    return true;
}

int SignalPosixProcess(const PosixProcessHandle& handle, int signal)
{
    return handle.processGroup > 1 ? kill(-handle.processGroup, signal) : kill(handle.pid, signal);
}
