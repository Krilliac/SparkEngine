/**
 * @file Color.cpp
 * @brief sRGB transfer, WCAG contrast, OKLab and Machado 2009 CVD simulation.
 */

#include "SparkUI/Color.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Spark::Ui
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;

        using Matrix3 = std::array<std::array<double, 3>, 3>;

        // Machado, Oliveira and Fernandes (2009), severity 1.0, applied to linear RGB.
        constexpr Matrix3 kProtan = {
            {{0.152286, 1.052583, -0.204868}, {0.114503, 0.786281, 0.099216}, {-0.003882, -0.048116, 1.051998}}};
        constexpr Matrix3 kDeutan = {
            {{0.367322, 0.860646, -0.227968}, {0.280085, 0.672501, 0.047413}, {-0.011820, 0.042940, 0.968881}}};
        constexpr Matrix3 kTritan = {
            {{1.255528, -0.076749, -0.178779}, {-0.078411, 0.930809, 0.147602}, {0.004733, 0.691367, 0.303900}}};

        const Matrix3& MatrixFor(Cvd kind) noexcept
        {
            switch (kind)
            {
            case Cvd::Protan:
                return kProtan;
            case Cvd::Deutan:
                return kDeutan;
            case Cvd::Tritan:
                return kTritan;
            }
            return kDeutan;
        }

        double ToLinear(double c) noexcept
        {
            return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
        }

        double ToEncoded(double c) noexcept
        {
            c = std::clamp(c, 0.0, 1.0);
            return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
        }

        OkLab LabFromLinear(double r, double g, double b) noexcept
        {
            const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
            const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
            const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
            return OkLab{static_cast<float>(0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s),
                         static_cast<float>(1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s),
                         static_cast<float>(0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s)};
        }

        std::array<double, 3> SimulatedLinear(Rgba c, Cvd kind) noexcept
        {
            const std::array<double, 3> linear = {ToLinear(c.r), ToLinear(c.g), ToLinear(c.b)};
            const Matrix3& m = MatrixFor(kind);
            std::array<double, 3> out{};
            for (std::size_t row = 0; row < 3; ++row)
            {
                const double v = m[row][0] * linear[0] + m[row][1] * linear[1] + m[row][2] * linear[2];
                out[row] = std::clamp(v, 0.0, 1.0);
            }
            return out;
        }
    } // namespace

    float SrgbToLinear(float c) noexcept
    {
        return static_cast<float>(ToLinear(c));
    }

    float LinearToSrgb(float c) noexcept
    {
        return static_cast<float>(ToEncoded(c));
    }

    float RelativeLuminance(Rgba c) noexcept
    {
        return static_cast<float>(0.2126 * ToLinear(c.r) + 0.7152 * ToLinear(c.g) + 0.0722 * ToLinear(c.b));
    }

    Rgba Composite(Rgba top, Rgba bottom) noexcept
    {
        const float a = std::clamp(top.a, 0.0f, 1.0f);
        const float outA = a + bottom.a * (1.0f - a);
        if (outA <= 0.0f)
        {
            return Rgba{0.0f, 0.0f, 0.0f, 0.0f};
        }
        const auto channel = [&](float t, float b) { return (t * a + b * bottom.a * (1.0f - a)) / outA; };
        return Rgba{channel(top.r, bottom.r), channel(top.g, bottom.g), channel(top.b, bottom.b), outA};
    }

    float ContrastRatio(Rgba fg, Rgba bg) noexcept
    {
        const Rgba visible = fg.a < 1.0f ? Composite(fg, bg) : fg;
        const float lf = RelativeLuminance(visible);
        const float lb = RelativeLuminance(bg);
        const float hi = std::max(lf, lb);
        const float lo = std::min(lf, lb);
        return (hi + 0.05f) / (lo + 0.05f);
    }

    OkLab ToOkLab(Rgba c) noexcept
    {
        return LabFromLinear(ToLinear(c.r), ToLinear(c.g), ToLinear(c.b));
    }

    Rgba FromOkLab(OkLab lab, float a) noexcept
    {
        const double l = std::pow(lab.L + 0.3963377774 * lab.a + 0.2158037573 * lab.b, 3.0);
        const double m = std::pow(lab.L - 0.1055613458 * lab.a - 0.0638541728 * lab.b, 3.0);
        const double s = std::pow(lab.L - 0.0894841775 * lab.a - 1.2914855480 * lab.b, 3.0);
        return Rgba{static_cast<float>(ToEncoded(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s)),
                    static_cast<float>(ToEncoded(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s)),
                    static_cast<float>(ToEncoded(-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s)), a};
    }

    float HueDegrees(Rgba c) noexcept
    {
        const OkLab lab = ToOkLab(c);
        double degrees = std::atan2(static_cast<double>(lab.b), static_cast<double>(lab.a)) * 180.0 / kPi;
        if (degrees < 0.0)
        {
            degrees += 360.0;
        }
        return static_cast<float>(degrees);
    }

    float HueDistanceDegrees(Rgba x, Rgba y) noexcept
    {
        const float d = std::fmod(std::fabs(HueDegrees(x) - HueDegrees(y)), 360.0f);
        return std::min(d, 360.0f - d);
    }

    OkLab SimulateCvdLab(Rgba c, Cvd kind) noexcept
    {
        const std::array<double, 3> linear = SimulatedLinear(c, kind);
        return LabFromLinear(linear[0], linear[1], linear[2]);
    }

    Rgba SimulateCvd(Rgba c, Cvd kind) noexcept
    {
        const std::array<double, 3> linear = SimulatedLinear(c, kind);
        return Rgba{static_cast<float>(ToEncoded(linear[0])), static_cast<float>(ToEncoded(linear[1])),
                    static_cast<float>(ToEncoded(linear[2])), c.a};
    }

    float DeltaE(OkLab x, OkLab y) noexcept
    {
        const float dl = x.L - y.L;
        const float da = x.a - y.a;
        const float db = x.b - y.b;
        return std::sqrt(dl * dl + da * da + db * db);
    }

    Rgba Mix(Rgba x, Rgba y, float t) noexcept
    {
        return Rgba{x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, 1.0f};
    }

    Rgba ShiftLightness(Rgba c, float amount) noexcept
    {
        OkLab lab = ToOkLab(c);
        lab.L += amount;
        return FromOkLab(lab, c.a);
    }

    Rgba EnsureContrast(Rgba fg, std::span<const Rgba> backgrounds, float minRatio, bool* converged) noexcept
    {
        const auto worstRatio = [&](Rgba candidate)
        {
            float worst = std::numeric_limits<float>::max();
            for (const Rgba& bg : backgrounds)
            {
                worst = std::min(worst, ContrastRatio(candidate, bg));
            }
            return worst;
        };
        const auto passes = [&](Rgba candidate) { return worstRatio(candidate) >= minRatio; };

        if (backgrounds.empty() || passes(fg))
        {
            if (converged != nullptr)
            {
                *converged = true;
            }
            return fg;
        }

        const OkLab lab = ToOkLab(fg);
        struct Candidate
        {
            bool ok = false;
            float move = 0.0f;
            Rgba colour;
        };
        // 1 extreme probe + 31 bisection steps per direction = at most 64 contrast evaluations.
        const auto search = [&](float targetL)
        {
            const auto at = [&](float fraction)
            { return FromOkLab(OkLab{lab.L + (targetL - lab.L) * fraction, lab.a, lab.b}, fg.a); };
            Candidate result;
            result.colour = at(1.0f);
            if (!passes(result.colour))
            {
                return result;
            }
            float lo = 0.0f;
            float hi = 1.0f;
            for (int step = 0; step < 31; ++step)
            {
                const float mid = 0.5f * (lo + hi);
                if (passes(at(mid)))
                {
                    hi = mid;
                }
                else
                {
                    lo = mid;
                }
            }
            result.ok = true;
            result.move = std::fabs(targetL - lab.L) * hi;
            result.colour = at(hi);
            return result;
        };

        const Candidate lighter = search(1.0f);
        const Candidate darker = search(0.0f);
        if (converged != nullptr)
        {
            *converged = lighter.ok || darker.ok;
        }
        if (lighter.ok && darker.ok)
        {
            return lighter.move <= darker.move ? lighter.colour : darker.colour;
        }
        if (lighter.ok)
        {
            return lighter.colour;
        }
        if (darker.ok)
        {
            return darker.colour;
        }
        return worstRatio(lighter.colour) >= worstRatio(darker.colour) ? lighter.colour : darker.colour;
    }
} // namespace Spark::Ui
