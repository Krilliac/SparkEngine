/**
 * @file ColorUtils.h
 * @brief Color manipulation utilities for Spark Engine
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides color representation, conversion (RGB/HSV/hex), interpolation,
 * and common preset colors used by rendering, UI, debug draw, and editor systems.
 *
 * ## Usage
 * @code
 *   using namespace Spark;
 *
 *   Color red = Color::Red();
 *   Color blue = Color::FromHex("#0000FF");
 *   Color blended = Color::Lerp(red, blue, 0.5f);  // purple
 *
 *   auto [h, s, v] = ColorUtils::RGBToHSV(1.0f, 0.0f, 0.0f); // h=0, s=1, v=1
 *   auto [r, g, b] = ColorUtils::HSVToRGB(120.0f, 1.0f, 1.0f); // green
 *
 *   std::string hex = red.ToHex(); // "#FF0000FF"
 * @endcode
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <tuple>

namespace Spark
{

    /**
     * @struct Color
     * @brief RGBA color with float components in [0, 1].
     */
    struct Color
    {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        float a = 1.0f;

        constexpr Color() = default;
        constexpr Color(float r, float g, float b, float a = 1.0f) : r(r), g(g), b(b), a(a) {}

        // =====================================================================
        // Preset colors
        // =====================================================================

        static constexpr Color White() { return {1.0f, 1.0f, 1.0f, 1.0f}; }
        static constexpr Color Black() { return {0.0f, 0.0f, 0.0f, 1.0f}; }
        static constexpr Color Red() { return {1.0f, 0.0f, 0.0f, 1.0f}; }
        static constexpr Color Green() { return {0.0f, 1.0f, 0.0f, 1.0f}; }
        static constexpr Color Blue() { return {0.0f, 0.0f, 1.0f, 1.0f}; }
        static constexpr Color Yellow() { return {1.0f, 1.0f, 0.0f, 1.0f}; }
        static constexpr Color Cyan() { return {0.0f, 1.0f, 1.0f, 1.0f}; }
        static constexpr Color Magenta() { return {1.0f, 0.0f, 1.0f, 1.0f}; }
        static constexpr Color Orange() { return {1.0f, 0.647f, 0.0f, 1.0f}; }
        static constexpr Color Gray() { return {0.5f, 0.5f, 0.5f, 1.0f}; }
        static constexpr Color Clear() { return {0.0f, 0.0f, 0.0f, 0.0f}; }

        // =====================================================================
        // Conversions
        // =====================================================================

        /**
         * @brief Pack to 32-bit RGBA (8 bits per channel).
         * @return  Packed RGBA as uint32_t (R in MSB).
         */
        uint32_t ToRGBA32() const
        {
            auto clampByte = [](float v) -> uint8_t
            { return static_cast<uint8_t>((std::max)(0.0f, (std::min)(1.0f, v)) * 255.0f + 0.5f); };
            return (static_cast<uint32_t>(clampByte(r)) << 24) | (static_cast<uint32_t>(clampByte(g)) << 16) |
                   (static_cast<uint32_t>(clampByte(b)) << 8) | static_cast<uint32_t>(clampByte(a));
        }

        /**
         * @brief Unpack from 32-bit RGBA.
         * @param rgba  Packed RGBA (R in MSB).
         * @return      Color with float components.
         */
        static Color FromRGBA32(uint32_t rgba)
        {
            return {static_cast<float>((rgba >> 24) & 0xFF) / 255.0f, static_cast<float>((rgba >> 16) & 0xFF) / 255.0f,
                    static_cast<float>((rgba >> 8) & 0xFF) / 255.0f, static_cast<float>(rgba & 0xFF) / 255.0f};
        }

        /**
         * @brief Parse a hex color string (#RGB, #RGBA, #RRGGBB, #RRGGBBAA).
         * @param hex  Hex string with optional '#' prefix.
         * @return     Parsed color, or Black() on failure.
         */
        static Color FromHex(const std::string& hex)
        {
            std::string s = hex;
            if (!s.empty() && s[0] == '#')
                s = s.substr(1);

            unsigned int val = 0;
            if (std::sscanf(s.c_str(), "%x", &val) != 1)
                return Black();

            if (s.size() == 8) // RRGGBBAA
            {
                return FromRGBA32(val);
            }
            else if (s.size() == 6) // RRGGBB
            {
                return FromRGBA32((val << 8) | 0xFF);
            }
            else if (s.size() == 4) // RGBA
            {
                uint8_t rr = static_cast<uint8_t>((val >> 12) & 0xF);
                uint8_t gg = static_cast<uint8_t>((val >> 8) & 0xF);
                uint8_t bb = static_cast<uint8_t>((val >> 4) & 0xF);
                uint8_t aa = static_cast<uint8_t>(val & 0xF);
                return {(rr * 17) / 255.0f, (gg * 17) / 255.0f, (bb * 17) / 255.0f, (aa * 17) / 255.0f};
            }
            else if (s.size() == 3) // RGB
            {
                uint8_t rr = static_cast<uint8_t>((val >> 8) & 0xF);
                uint8_t gg = static_cast<uint8_t>((val >> 4) & 0xF);
                uint8_t bb = static_cast<uint8_t>(val & 0xF);
                return {(rr * 17) / 255.0f, (gg * 17) / 255.0f, (bb * 17) / 255.0f, 1.0f};
            }
            return Black();
        }

        /**
         * @brief Convert to hex string "#RRGGBBAA".
         * @return  Hex representation of the color.
         */
        std::string ToHex() const
        {
            uint32_t packed = ToRGBA32();
            char buf[10];
            std::snprintf(buf, sizeof(buf), "#%08X", packed);
            return std::string(buf);
        }

        /**
         * @brief Create a color with modified alpha.
         * @param alpha  New alpha value.
         * @return       Copy of this color with the new alpha.
         */
        constexpr Color WithAlpha(float alpha) const { return {r, g, b, alpha}; }

        // =====================================================================
        // Operations
        // =====================================================================

        /**
         * @brief Linear interpolation between two colors.
         * @param a  Start color.
         * @param b  End color.
         * @param t  Interpolation factor [0, 1].
         * @return   Blended color.
         */
        static Color Lerp(const Color& a, const Color& b, float t)
        {
            return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
        }

        /**
         * @brief Compute a brightness-adjusted variant of this color.
         * @param factor  Multiplier (1.0 = unchanged, 0.5 = half brightness).
         * @return        Brightness-adjusted color with original alpha.
         */
        Color AdjustBrightness(float factor) const
        {
            return {(std::min)(r * factor, 1.0f), (std::min)(g * factor, 1.0f), (std::min)(b * factor, 1.0f), a};
        }

        bool operator==(const Color& other) const
        {
            return r == other.r && g == other.g && b == other.b && a == other.a;
        }
        bool operator!=(const Color& other) const { return !(*this == other); }
    };

    /**
     * @namespace ColorUtils
     * @brief Free-function color conversion utilities.
     */
    namespace ColorUtils
    {

        /**
         * @brief Convert RGB to HSV.
         * @param r  Red [0, 1].
         * @param g  Green [0, 1].
         * @param b  Blue [0, 1].
         * @return   Tuple of (hue [0,360], saturation [0,1], value [0,1]).
         */
        inline std::tuple<float, float, float> RGBToHSV(float r, float g, float b)
        {
            float maxC = (std::max)({r, g, b});
            float minC = (std::min)({r, g, b});
            float delta = maxC - minC;

            float h = 0.0f;
            if (delta > 0.00001f)
            {
                if (maxC == r)
                    h = 60.0f * ((g - b) / delta);
                else if (maxC == g)
                    h = 60.0f * (2.0f + (b - r) / delta);
                else
                    h = 60.0f * (4.0f + (r - g) / delta);
                if (h < 0.0f)
                    h += 360.0f;
            }

            float s = (maxC > 0.00001f) ? (delta / maxC) : 0.0f;
            float v = maxC;

            return {h, s, v};
        }

        /**
         * @brief Convert HSV to RGB.
         * @param h  Hue [0, 360].
         * @param s  Saturation [0, 1].
         * @param v  Value [0, 1].
         * @return   Tuple of (red, green, blue) each in [0, 1].
         */
        inline std::tuple<float, float, float> HSVToRGB(float h, float s, float v)
        {
            float c = v * s;
            float hh = h / 60.0f;
            float x = c * (1.0f - std::abs(std::fmod(hh, 2.0f) - 1.0f));
            float m = v - c;

            float r = 0.0f, g = 0.0f, b = 0.0f;
            if (hh < 1.0f)
            {
                r = c;
                g = x;
            }
            else if (hh < 2.0f)
            {
                r = x;
                g = c;
            }
            else if (hh < 3.0f)
            {
                g = c;
                b = x;
            }
            else if (hh < 4.0f)
            {
                g = x;
                b = c;
            }
            else if (hh < 5.0f)
            {
                r = x;
                b = c;
            }
            else
            {
                r = c;
                b = x;
            }

            return {r + m, g + m, b + m};
        }

        /**
         * @brief Convert a Color to HSV representation.
         * @param color  Input color.
         * @return       Tuple of (hue, saturation, value).
         */
        inline std::tuple<float, float, float> ToHSV(const Color& color)
        {
            return RGBToHSV(color.r, color.g, color.b);
        }

        /**
         * @brief Create a Color from HSV values.
         * @param h  Hue [0, 360].
         * @param s  Saturation [0, 1].
         * @param v  Value [0, 1].
         * @param a  Alpha [0, 1].
         * @return   RGB Color.
         */
        inline Color FromHSV(float h, float s, float v, float a = 1.0f)
        {
            auto [r, g, b] = HSVToRGB(h, s, v);
            return {r, g, b, a};
        }

        /**
         * @brief Compute perceived luminance (ITU BT.709).
         * @param color  Input color.
         * @return       Luminance in [0, 1].
         */
        inline float Luminance(const Color& color)
        {
            return 0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b;
        }

    } // namespace ColorUtils

} // namespace Spark
