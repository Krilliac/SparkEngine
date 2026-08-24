/**
 * @file StartupSplash.cpp
 * @brief Deterministic CPU startup animation with Win32 and SDL2 presenters.
 */

#include "StartupSplash.h"

#include "Graphics/ProjectAssetPath.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <string_view>
#include <thread>

#ifdef SPARK_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <mmsystem.h>
#elif defined(SPARK_SDL2_AVAILABLE)
#include <SDL.h>
#endif

namespace Spark
{
    namespace
    {
        constexpr int kRenderWidth = 960;
        constexpr int kRenderHeight = 540;
        constexpr int kMaximumBitmapDimension = 4096;
        constexpr uint64_t kMaximumBitmapBytes = 64ull * 1024ull * 1024ull;
        float Saturate(float value)
        {
            return std::clamp(value, 0.0f, 1.0f);
        }

        float EaseOutCubic(float value)
        {
            const float inverse = 1.0f - Saturate(value);
            return 1.0f - inverse * inverse * inverse;
        }

        std::string Trim(std::string text)
        {
            const auto isSpace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
            auto first = std::find_if_not(text.begin(), text.end(), isSpace);
            auto last = std::find_if_not(text.rbegin(), text.rend(), isSpace).base();
            return first < last ? std::string(first, last) : std::string{};
        }

        std::string Lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        bool IsSwitch(std::string_view value, std::string_view expected)
        {
            return Lower(std::string(value)) == Lower(std::string(expected));
        }

        std::optional<std::string> ArgumentValue(const std::vector<std::string>& arguments, std::string_view name)
        {
            const std::string prefix = Lower(std::string(name)) + "=";
            for (size_t i = 0; i < arguments.size(); ++i)
            {
                const std::string lowered = Lower(arguments[i]);
                if (lowered == Lower(std::string(name)) && i + 1 < arguments.size())
                    return arguments[i + 1];
                if (lowered.starts_with(prefix))
                    return arguments[i].substr(prefix.size());
            }
            return std::nullopt;
        }

        bool HasArgument(const std::vector<std::string>& arguments, std::string_view name)
        {
            return std::any_of(arguments.begin(), arguments.end(),
                               [&](const std::string& argument) { return IsSwitch(argument, name); });
        }

        std::filesystem::path ResolveProjectRoot(const StartupSplashContext& context)
        {
            std::filesystem::path root = context.projectRoot;
            if (root.empty())
            {
                if (const auto project = ArgumentValue(context.arguments, "-project"))
                    root = std::filesystem::u8path(*project);
                else if (const auto project = ArgumentValue(context.arguments, "--project"))
                    root = std::filesystem::u8path(*project);
                else if (const char* environmentRoot = std::getenv("SPARK_PROJECT_ROOT"))
                    root = std::filesystem::u8path(environmentRoot);
            }

            std::error_code ec;
            if (root.empty())
                root = std::filesystem::current_path(ec);
            if (root.extension() == ".sparkproject")
                root = root.parent_path();
            return root.lexically_normal();
        }

        std::optional<std::string> PathToUtf8(const std::filesystem::path& path)
        {
#ifdef SPARK_PLATFORM_WINDOWS
            const std::wstring& wide = path.native();
            if (wide.empty() || wide.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
                return std::nullopt;
            const int inputLength = static_cast<int>(wide.size());
            const int utf8Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), inputLength, nullptr,
                                                       0, nullptr, nullptr);
            if (utf8Length <= 0)
                return std::nullopt;
            std::string utf8(static_cast<size_t>(utf8Length), '\0');
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), inputLength, utf8.data(), utf8Length,
                                    nullptr, nullptr) != utf8Length)
                return std::nullopt;
            return utf8;
#else
            return path.generic_string();
#endif
        }

        std::filesystem::path ResolveConfiguredAssetPath(const std::string& value,
                                                         const std::filesystem::path& projectRoot)
        {
            if (value.empty())
                return {};
            const auto rootUtf8 = PathToUtf8(projectRoot);
            if (!rootUtf8)
                return {};
            const auto resolved = ResolveProjectAssetPath(*rootUtf8, value);
            return resolved ? resolved->nativePath : std::filesystem::path{};
        }

        double ParseDuration(const std::string& value, double fallback)
        {
            try
            {
                const double parsed = std::stod(value);
                return std::isfinite(parsed) ? std::clamp(parsed, 0.25, 15.0) : fallback;
            }
            catch (...)
            {
                return fallback;
            }
        }

        uint32_t ParseRgb(const std::string& value, uint32_t fallback)
        {
            std::string digits = Trim(value);
            if (!digits.empty() && digits.front() == '#')
                digits.erase(digits.begin());
            if (digits.size() != 6)
                return fallback;
            try
            {
                const auto parsed = std::stoul(digits, nullptr, 16);
                return parsed <= 0xFFFFFFu ? static_cast<uint32_t>(parsed) : fallback;
            }
            catch (...)
            {
                return fallback;
            }
        }

        void LoadIni(const std::filesystem::path& path, const std::filesystem::path& projectRoot,
                     StartupSplashConfig& config)
        {
            std::ifstream stream(path);
            if (!stream)
                return;

            std::string line;
            while (std::getline(stream, line))
            {
                line = Trim(line);
                if (line.empty() || line.front() == '#' || line.front() == ';' || line.front() == '[')
                    continue;
                const size_t separator = line.find('=');
                if (separator == std::string::npos)
                    continue;

                const std::string key = Lower(Trim(line.substr(0, separator)));
                const std::string value = Trim(line.substr(separator + 1));
                if (key == "enabled")
                {
                    const std::string lowered = Lower(value);
                    config.enabled = lowered != "false" && lowered != "0" && lowered != "off" && lowered != "no";
                }
                else if (key == "mute" || key == "muted")
                {
                    const std::string lowered = Lower(value);
                    config.muted = lowered == "true" || lowered == "1" || lowered == "on" || lowered == "yes";
                }
                else if (key == "duration" || key == "durationseconds")
                    config.durationSeconds = ParseDuration(value, config.durationSeconds);
                else if (key == "image")
                    config.imagePath = ResolveConfiguredAssetPath(value, projectRoot);
                else if (key == "audio")
                    config.audioPath = ResolveConfiguredAssetPath(value, projectRoot);
                else if (key == "accent" || key == "accentcolor")
                    config.accentRgb = ParseRgb(value, config.accentRgb);
            }
        }

        uint8_t Channel(uint32_t color, unsigned shift)
        {
            return static_cast<uint8_t>((color >> shift) & 0xFFu);
        }

        uint32_t WithOpacity(uint32_t rgb, float opacity)
        {
            return (static_cast<uint32_t>(Saturate(opacity) * 255.0f) << 24) | (rgb & 0xFFFFFFu);
        }

        void BlendPixel(std::vector<uint32_t>& pixels, int width, int height, int x, int y, uint32_t source)
        {
            if (x < 0 || y < 0 || x >= width || y >= height)
                return;
            const uint32_t alpha = source >> 24;
            if (alpha == 0)
                return;
            uint32_t& destination = pixels[static_cast<size_t>(y) * width + x];
            if (alpha == 255)
            {
                destination = source;
                return;
            }
            const uint32_t inverse = 255 - alpha;
            const uint32_t red = (Channel(source, 16) * alpha + Channel(destination, 16) * inverse) / 255;
            const uint32_t green = (Channel(source, 8) * alpha + Channel(destination, 8) * inverse) / 255;
            const uint32_t blue = (Channel(source, 0) * alpha + Channel(destination, 0) * inverse) / 255;
            destination = 0xFF000000u | (red << 16) | (green << 8) | blue;
        }

        void DrawLine(std::vector<uint32_t>& pixels, int width, int height, int x0, int y0, int x1, int y1,
                      uint32_t color, int thickness = 1)
        {
            const int dx = std::abs(x1 - x0);
            const int sx = x0 < x1 ? 1 : -1;
            const int dy = -std::abs(y1 - y0);
            const int sy = y0 < y1 ? 1 : -1;
            int error = dx + dy;
            for (;;)
            {
                for (int oy = -thickness / 2; oy <= thickness / 2; ++oy)
                    for (int ox = -thickness / 2; ox <= thickness / 2; ++ox)
                        BlendPixel(pixels, width, height, x0 + ox, y0 + oy, color);
                if (x0 == x1 && y0 == y1)
                    break;
                const int doubled = error * 2;
                if (doubled >= dy)
                {
                    error += dy;
                    x0 += sx;
                }
                if (doubled <= dx)
                {
                    error += dx;
                    y0 += sy;
                }
            }
        }

        std::array<uint8_t, 7> Glyph(char value)
        {
            switch (value)
            {
            case 'A':
                return {14, 17, 17, 31, 17, 17, 17};
            case 'B':
                return {30, 17, 17, 30, 17, 17, 30};
            case 'C':
                return {14, 17, 16, 16, 16, 17, 14};
            case 'D':
                return {30, 17, 17, 17, 17, 17, 30};
            case 'E':
                return {31, 16, 16, 30, 16, 16, 31};
            case 'G':
                return {14, 17, 16, 23, 17, 17, 14};
            case 'I':
                return {31, 4, 4, 4, 4, 4, 31};
            case 'K':
                return {17, 18, 20, 24, 20, 18, 17};
            case 'M':
                return {17, 27, 21, 21, 17, 17, 17};
            case 'N':
                return {17, 25, 21, 19, 17, 17, 17};
            case 'O':
                return {14, 17, 17, 17, 17, 17, 14};
            case 'P':
                return {30, 17, 17, 30, 16, 16, 16};
            case 'R':
                return {30, 17, 17, 30, 20, 18, 17};
            case 'S':
                return {15, 16, 16, 14, 1, 1, 30};
            case 'T':
                return {31, 4, 4, 4, 4, 4, 4};
            case 'U':
                return {17, 17, 17, 17, 17, 17, 14};
            case 'W':
                return {17, 17, 17, 21, 21, 21, 10};
            case 'Y':
                return {17, 17, 10, 4, 4, 4, 4};
            case '+':
                return {0, 4, 4, 31, 4, 4, 0};
            case '/':
                return {1, 2, 2, 4, 8, 8, 16};
            case '2':
                return {14, 17, 1, 2, 4, 8, 31};
            case '3':
                return {30, 1, 1, 14, 1, 1, 30};
            default:
                return {};
            }
        }

        void DrawText(std::vector<uint32_t>& pixels, int width, int height, std::string_view text, int x, int y,
                      int scale, uint32_t color, int revealRight)
        {
            for (char character : text)
            {
                if (character == ' ')
                {
                    x += 4 * scale;
                    continue;
                }
                const auto glyph = Glyph(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
                for (int row = 0; row < 7; ++row)
                {
                    for (int column = 0; column < 5; ++column)
                    {
                        if ((glyph[row] & (1 << (4 - column))) == 0)
                            continue;
                        const int left = x + column * scale;
                        for (int py = 0; py < scale; ++py)
                            for (int px = 0; px < scale && left + px <= revealRight; ++px)
                                BlendPixel(pixels, width, height, left + px, y + row * scale + py, color);
                    }
                }
                x += 6 * scale;
            }
        }

        void DrawBitmap(std::vector<uint32_t>& destination, int destinationWidth, int destinationHeight,
                        const StartupSplashBitmap& bitmap, float reveal, float opacity)
        {
            const StartupSplashRect rect = CalculateStartupSplashLetterbox(
                destinationWidth * 4 / 5, destinationHeight * 4 / 5, bitmap.width, bitmap.height);
            const int originX = destinationWidth / 10 + rect.x;
            const int originY = destinationHeight / 10 + rect.y;
            const int visibleWidth = static_cast<int>(rect.width * Saturate(reveal));
            for (int y = 0; y < rect.height; ++y)
            {
                const int sourceY = std::min(bitmap.height - 1, y * bitmap.height / rect.height);
                for (int x = 0; x < visibleWidth; ++x)
                {
                    const int sourceX = std::min(bitmap.width - 1, x * bitmap.width / rect.width);
                    uint32_t color = bitmap.pixels[static_cast<size_t>(sourceY) * bitmap.width + sourceX];
                    const float sourceAlpha = static_cast<float>(color >> 24) / 255.0f;
                    color = WithOpacity(color, opacity * sourceAlpha);
                    BlendPixel(destination, destinationWidth, destinationHeight, originX + x, originY + y, color);
                }
            }
        }

        std::filesystem::path ExistingDefaultAudio(const StartupSplashContext& context,
                                                   const std::filesystem::path& projectRoot)
        {
            const std::array<std::filesystem::path, 3> candidates = {
                projectRoot / "Assets/Engine/Branding/sparkengine_splash.wav",
                context.executableDirectory / "Assets/Engine/Branding/sparkengine_splash.wav",
                context.executableDirectory.parent_path() / "Assets/Engine/Branding/sparkengine_splash.wav"};
            std::error_code ec;
            for (const auto& candidate : candidates)
            {
                if (!candidate.empty() && std::filesystem::is_regular_file(candidate, ec))
                    return candidate;
                ec.clear();
            }
            return {};
        }
    } // namespace

    StartupSplashConfig ResolveStartupSplashConfig(const StartupSplashContext& context)
    {
        StartupSplashConfig config;
        const auto projectRoot = ResolveProjectRoot(context);
        config.audioPath = ExistingDefaultAudio(context, projectRoot);
        LoadIni(projectRoot / "Config/StartupSplash.ini", projectRoot, config);

        if (const auto image = ArgumentValue(context.arguments, "-splash"))
        {
            config.imagePath = ResolveConfiguredAssetPath(*image, projectRoot);
            config.enabled = true;
        }
        if (const auto image = ArgumentValue(context.arguments, "--splash"))
        {
            config.imagePath = ResolveConfiguredAssetPath(*image, projectRoot);
            config.enabled = true;
        }
        if (const auto audio = ArgumentValue(context.arguments, "-splash-audio"))
            config.audioPath = ResolveConfiguredAssetPath(*audio, projectRoot);
        if (const auto duration = ArgumentValue(context.arguments, "-splash-duration"))
            config.durationSeconds = ParseDuration(*duration, config.durationSeconds);
        // An explicit opt-out always wins, even if a wrapper also supplied an
        // image override earlier on the command line.
        if (HasArgument(context.arguments, "-no-splash") || HasArgument(context.arguments, "--no-splash"))
            config.enabled = false;
        if (HasArgument(context.arguments, "-no-splash-audio") || HasArgument(context.arguments, "--no-splash-audio"))
            config.muted = true;
        return config;
    }

    bool ShouldShowStartupSplash(const StartupSplashContext& context, const StartupSplashConfig& config)
    {
        const bool testArgument =
            HasArgument(context.arguments, "-test-frames") || HasArgument(context.arguments, "--test-frames") ||
            HasArgument(context.arguments, "-test-seconds") || HasArgument(context.arguments, "--test-seconds") ||
            HasArgument(context.arguments, "-minimal-init") || HasArgument(context.arguments, "--minimal-init");
        const bool serverArgument =
            HasArgument(context.arguments, "-headless") || HasArgument(context.arguments, "--headless") ||
            HasArgument(context.arguments, "-dedicated") || HasArgument(context.arguments, "--dedicated");
        return config.enabled && !context.headless && !context.automatedTest && !testArgument && !serverArgument;
    }

    StartupSplashRect CalculateStartupSplashLetterbox(int destinationWidth, int destinationHeight, int contentWidth,
                                                      int contentHeight)
    {
        if (destinationWidth <= 0 || destinationHeight <= 0 || contentWidth <= 0 || contentHeight <= 0)
            return {};
        const double scale = std::min(static_cast<double>(destinationWidth) / contentWidth,
                                      static_cast<double>(destinationHeight) / contentHeight);
        StartupSplashRect result;
        result.width = std::max(1, static_cast<int>(std::floor(contentWidth * scale)));
        result.height = std::max(1, static_cast<int>(std::floor(contentHeight * scale)));
        result.x = (destinationWidth - result.width) / 2;
        result.y = (destinationHeight - result.height) / 2;
        return result;
    }

    StartupSplashBitmap LoadStartupSplashBmp(const std::filesystem::path& path)
    {
        StartupSplashBitmap result;
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            return result;

        std::array<uint8_t, 54> header{};
        stream.read(reinterpret_cast<char*>(header.data()), header.size());
        if (stream.gcount() != static_cast<std::streamsize>(header.size()) || header[0] != 'B' || header[1] != 'M')
            return result;
        const auto read16 = [&](size_t offset)
        { return static_cast<uint16_t>(header[offset] | (static_cast<uint16_t>(header[offset + 1]) << 8)); };
        const auto read32 = [&](size_t offset)
        {
            return static_cast<uint32_t>(header[offset] | (static_cast<uint32_t>(header[offset + 1]) << 8) |
                                         (static_cast<uint32_t>(header[offset + 2]) << 16) |
                                         (static_cast<uint32_t>(header[offset + 3]) << 24));
        };
        const uint32_t declaredFileSize = read32(2);
        const uint32_t pixelOffset = read32(10);
        const uint32_t dibHeaderSize = read32(14);
        const int32_t width = static_cast<int32_t>(read32(18));
        const int32_t signedHeight = static_cast<int32_t>(read32(22));
        const uint16_t planes = read16(26);
        const uint16_t bitsPerPixel = read16(28);
        const uint32_t compression = read32(30);
        if (dibHeaderSize < 40 || width <= 0 || signedHeight == 0 ||
            signedHeight == std::numeric_limits<int32_t>::min() || width > kMaximumBitmapDimension ||
            std::abs(signedHeight) > kMaximumBitmapDimension || planes != 1 ||
            (bitsPerPixel != 24 && bitsPerPixel != 32) || compression != 0 || pixelOffset < 14ull + dibHeaderSize)
            return result;

        const int height = std::abs(signedHeight);
        const uint64_t rowBytes64 = ((static_cast<uint64_t>(width) * bitsPerPixel + 31ull) / 32ull) * 4ull;
        const uint64_t pixelBytes64 = rowBytes64 * static_cast<uint64_t>(height);
        if (rowBytes64 == 0 || pixelBytes64 > kMaximumBitmapBytes ||
            pixelBytes64 > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
            static_cast<uint64_t>(pixelOffset) + pixelBytes64 < pixelOffset)
            return result;
        stream.seekg(0, std::ios::end);
        const std::streamoff actualFileSize = stream.tellg();
        if (actualFileSize < 0 ||
            static_cast<uint64_t>(actualFileSize) < static_cast<uint64_t>(pixelOffset) + pixelBytes64 ||
            (declaredFileSize != 0 && declaredFileSize > static_cast<uint64_t>(actualFileSize)))
            return result;
        const size_t rowBytes = static_cast<size_t>(rowBytes64);
        std::vector<uint8_t> bytes(rowBytes * static_cast<size_t>(height));
        stream.seekg(pixelOffset, std::ios::beg);
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (stream.gcount() != static_cast<std::streamsize>(bytes.size()))
            return result;

        result.width = width;
        result.height = height;
        result.pixels.resize(static_cast<size_t>(width) * height);
        const size_t bytesPerPixel = bitsPerPixel / 8;
        bool hasMeaningfulAlpha = false;
        if (bitsPerPixel == 32)
        {
            for (int y = 0; y < height && !hasMeaningfulAlpha; ++y)
            {
                const uint8_t* row = bytes.data() + static_cast<size_t>(y) * rowBytes;
                for (int x = 0; x < width; ++x)
                {
                    if (row[static_cast<size_t>(x) * bytesPerPixel + 3] != 0)
                    {
                        hasMeaningfulAlpha = true;
                        break;
                    }
                }
            }
        }
        for (int y = 0; y < height; ++y)
        {
            const int sourceY = signedHeight > 0 ? height - 1 - y : y;
            const uint8_t* row = bytes.data() + static_cast<size_t>(sourceY) * rowBytes;
            for (int x = 0; x < width; ++x)
            {
                const uint8_t* pixel = row + static_cast<size_t>(x) * bytesPerPixel;
                const uint8_t alpha = bitsPerPixel == 32 && hasMeaningfulAlpha ? pixel[3] : 255;
                result.pixels[static_cast<size_t>(y) * width + x] = (static_cast<uint32_t>(alpha) << 24) |
                                                                    (static_cast<uint32_t>(pixel[2]) << 16) |
                                                                    (static_cast<uint32_t>(pixel[1]) << 8) | pixel[0];
            }
        }
        return result;
    }

    void RenderStartupSplashFrame(const StartupSplashConfig& config, double elapsedSeconds, int width, int height,
                                  std::vector<uint32_t>& pixels, const StartupSplashBitmap* projectBitmap)
    {
        if (width <= 0 || height <= 0)
        {
            pixels.clear();
            return;
        }
        if (width > kMaximumBitmapDimension || height > kMaximumBitmapDimension)
        {
            pixels.clear();
            return;
        }
        pixels.assign(static_cast<size_t>(width) * height, 0xFF050608u);

        const double duration = std::max(0.001, config.durationSeconds);
        const float normalized = Saturate(static_cast<float>(elapsedSeconds / duration));
        const float fade = 1.0f - Saturate((normalized - 0.875f) / 0.125f);
        const uint32_t accent = config.accentRgb & 0xFFFFFFu;

        const int gridSpacing = std::max(18, width / 32);
        for (int x = width % gridSpacing / 2; x < width; x += gridSpacing)
            DrawLine(pixels, width, height, x, 0, x, height - 1, WithOpacity(0x304050, 0.11f * fade));
        for (int y = height % gridSpacing / 2; y < height; y += gridSpacing)
            DrawLine(pixels, width, height, 0, y, width - 1, y, WithOpacity(0x304050, 0.11f * fade));

        const float ignition = EaseOutCubic(static_cast<float>(elapsedSeconds / 0.58));
        const float reveal = EaseOutCubic(static_cast<float>((elapsedSeconds - 0.52) / 0.90));
        if (projectBitmap && projectBitmap->IsValid())
        {
            DrawBitmap(pixels, width, height, *projectBitmap, reveal, fade);
        }
        else
        {
            const int centerX = static_cast<int>(width * 0.285f);
            const int centerY = height / 2;
            const int radius = static_cast<int>(std::min(width, height) * 0.17f * ignition);
            constexpr std::array<std::array<float, 2>, 8> directions = {std::array<float, 2>{1.0f, 0.0f},
                                                                        {-1.0f, 0.0f},
                                                                        {0.0f, 1.0f},
                                                                        {0.0f, -1.0f},
                                                                        {0.71f, 0.71f},
                                                                        {-0.71f, 0.71f},
                                                                        {0.71f, -0.71f},
                                                                        {-0.71f, -0.71f}};
            for (size_t i = 0; i < directions.size(); ++i)
            {
                const float length = (i < 4 ? 1.0f : 0.72f) * radius;
                DrawLine(pixels, width, height, centerX, centerY, centerX + static_cast<int>(directions[i][0] * length),
                         centerY + static_cast<int>(directions[i][1] * length),
                         WithOpacity(accent, fade * (0.72f + 0.28f * ignition)), std::max(1, width / 240));
            }
            const int core = std::max(2, static_cast<int>(8 * ignition));
            for (int y = -core; y <= core; ++y)
                for (int x = -core; x <= core; ++x)
                    if (x * x + y * y <= core * core)
                        BlendPixel(pixels, width, height, centerX + x, centerY + y, WithOpacity(0xFFF2D6, fade));

            const int wordX = static_cast<int>(width * 0.385f);
            const int wordY = centerY - std::max(16, width / 42);
            const int scale = std::max(3, width / 150);
            const int revealRight = wordX + static_cast<int>(width * 0.52f * reveal);
            DrawText(pixels, width, height, "SPARKENGINE", wordX, wordY, scale, WithOpacity(0xE9EEF2, fade),
                     revealRight);
            DrawText(pixels, width, height, "POWERED BY", wordX, wordY - scale * 5, std::max(1, scale / 3),
                     WithOpacity(0x98A1AA, fade * reveal), revealRight);
            DrawText(pixels, width, height, "C++23 / RUNTIME", wordX, wordY + scale * 10, std::max(1, scale / 3),
                     WithOpacity(0x6F7882, fade * reveal), revealRight);
        }

        if (reveal > 0.0f && reveal < 1.0f)
        {
            const int scanX = static_cast<int>(width * (0.12f + 0.78f * reveal));
            DrawLine(pixels, width, height, scanX, height / 5, scanX, height * 4 / 5,
                     WithOpacity(0xFFF0D0, fade * (1.0f - reveal) * 0.75f), std::max(1, width / 480));
        }

        if (fade < 1.0f)
        {
            const uint32_t black = WithOpacity(0x000000, 1.0f - fade);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    BlendPixel(pixels, width, height, x, y, black);
        }
    }

    bool PlayStartupSplash(const StartupSplashContext& context)
    {
        const StartupSplashConfig config = ResolveStartupSplashConfig(context);
        if (!ShouldShowStartupSplash(context, config))
            return false;

        StartupSplashBitmap projectBitmap;
        if (!config.imagePath.empty())
            projectBitmap = LoadStartupSplashBmp(config.imagePath);

#ifdef SPARK_PLATFORM_WINDOWS
        RECT workArea{};
        if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0))
        {
            workArea.right = GetSystemMetrics(SM_CXSCREEN);
            workArea.bottom = GetSystemMetrics(SM_CYSCREEN);
        }
        const int availableWidth = static_cast<int>(std::max(640L, workArea.right - workArea.left));
        const int availableHeight = static_cast<int>(std::max(360L, workArea.bottom - workArea.top));
        const auto windowRect = CalculateStartupSplashLetterbox(availableWidth * 4 / 5, availableHeight * 4 / 5);
        const int windowX = workArea.left + (availableWidth - windowRect.width) / 2;
        const int windowY = workArea.top + (availableHeight - windowRect.height) / 2;
        // Keep this as an ordinary top-level window. WS_EX_TOOLWINDOW caused
        // the splash to be omitted by Windows' normal foreground/window
        // surfaces on some systems even though the render loop still waited.
        HWND window =
            CreateWindowExW(WS_EX_TOPMOST, L"STATIC", L"SparkEngine", WS_POPUP | WS_VISIBLE, windowX, windowY,
                            windowRect.width, windowRect.height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window)
            return false;
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);

        std::error_code ec;
        if (!config.muted && !config.audioPath.empty() && std::filesystem::is_regular_file(config.audioPath, ec))
            PlaySoundW(config.audioPath.c_str(), nullptr, SND_ASYNC | SND_FILENAME | SND_NODEFAULT);

        std::vector<uint32_t> pixels;
        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = kRenderWidth;
        bitmapInfo.bmiHeader.biHeight = -kRenderHeight;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        const auto started = std::chrono::steady_clock::now();
        bool quitReceived = false;
        while (true)
        {
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            if (elapsed >= config.durationSeconds)
                break;
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                if (message.message == WM_QUIT)
                {
                    quitReceived = true;
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (quitReceived)
                break;

            RenderStartupSplashFrame(config, elapsed, kRenderWidth, kRenderHeight, pixels,
                                     projectBitmap.IsValid() ? &projectBitmap : nullptr);
            RECT client{};
            GetClientRect(window, &client);
            HDC deviceContext = GetDC(window);
            if (deviceContext)
            {
                FillRect(deviceContext, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
                const auto destination = CalculateStartupSplashLetterbox(
                    client.right - client.left, client.bottom - client.top, kRenderWidth, kRenderHeight);
                StretchDIBits(deviceContext, destination.x, destination.y, destination.width, destination.height, 0, 0,
                              kRenderWidth, kRenderHeight, pixels.data(), &bitmapInfo, DIB_RGB_COLORS, SRCCOPY);
                ReleaseDC(window, deviceContext);
            }
            std::this_thread::sleep_until(started +
                                          std::chrono::milliseconds(static_cast<int64_t>((elapsed * 1000.0) + 16.0)));
        }
        PlaySoundW(nullptr, nullptr, 0);
        DestroyWindow(window);
        if (quitReceived)
            PostQuitMessage(0);
        return true;
#elif defined(SPARK_SDL2_AVAILABLE)
        if ((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0)
            return false;
        SDL_Window* window =
            SDL_CreateWindow("SparkEngine", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kRenderWidth, kRenderHeight,
                             SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALLOW_HIGHDPI);
        if (!window)
            return false;

        const bool audioWasInitialized = (SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) != 0;
        SDL_AudioDeviceID audioDevice = 0;
        SDL_AudioSpec waveSpec{};
        Uint8* waveBuffer = nullptr;
        Uint32 waveLength = 0;
        if (!config.muted && !config.audioPath.empty() &&
            (audioWasInitialized || SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) &&
            SDL_LoadWAV(config.audioPath.string().c_str(), &waveSpec, &waveBuffer, &waveLength))
        {
            audioDevice = SDL_OpenAudioDevice(nullptr, 0, &waveSpec, nullptr, 0);
            if (audioDevice != 0)
            {
                SDL_QueueAudio(audioDevice, waveBuffer, waveLength);
                SDL_PauseAudioDevice(audioDevice, 0);
            }
        }

        std::vector<uint32_t> pixels;
        const auto started = std::chrono::steady_clock::now();
        bool quitReceived = false;
        while (true)
        {
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            if (elapsed >= config.durationSeconds)
                break;
            SDL_Event event{};
            while (SDL_PollEvent(&event))
            {
                if (event.type == SDL_QUIT)
                {
                    quitReceived = true;
                    break;
                }
            }
            if (quitReceived)
                break;
            RenderStartupSplashFrame(config, elapsed, kRenderWidth, kRenderHeight, pixels,
                                     projectBitmap.IsValid() ? &projectBitmap : nullptr);
            SDL_Surface* destination = SDL_GetWindowSurface(window);
            if (destination)
            {
                SDL_FillRect(destination, nullptr, SDL_MapRGB(destination->format, 0, 0, 0));
                SDL_Surface* source =
                    SDL_CreateRGBSurfaceFrom(pixels.data(), kRenderWidth, kRenderHeight, 32, kRenderWidth * 4,
                                             0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u);
                if (source)
                {
                    const auto fitted =
                        CalculateStartupSplashLetterbox(destination->w, destination->h, kRenderWidth, kRenderHeight);
                    SDL_Rect rect{fitted.x, fitted.y, fitted.width, fitted.height};
                    SDL_BlitScaled(source, nullptr, destination, &rect);
                    SDL_FreeSurface(source);
                    SDL_UpdateWindowSurface(window);
                }
            }
            std::this_thread::sleep_until(started +
                                          std::chrono::milliseconds(static_cast<int64_t>((elapsed * 1000.0) + 16.0)));
        }
        if (quitReceived)
        {
            SDL_Event event{};
            event.type = SDL_QUIT;
            SDL_PushEvent(&event);
        }
        if (audioDevice != 0)
            SDL_CloseAudioDevice(audioDevice);
        if (waveBuffer)
            SDL_FreeWAV(waveBuffer);
        if (!audioWasInitialized && (SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) != 0)
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
        SDL_DestroyWindow(window);
        return true;
#else
        (void)projectBitmap;
        return false;
#endif
    }
} // namespace Spark
