/**
 * @file TFWeaponMath.h
 * @brief Small float[3] vector helpers + capsule/cone math shared by the
 *        client and server halves of TFWeaponSystem (internal to Game/).
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <random>

namespace Terrafront::WeaponMath
{

    // Pawn capsule dimensions. Must stay in sync with the RewindPose radius/height
    // that TFPlayerSystem records into Spark::Net::LagCompensation (W1 defaults).
    constexpr float kPawnHeightM = 1.8f;
    constexpr float kPawnRadiusM = 0.4f;
    constexpr float kHeadZoneM = 0.3f; // top of capsule that counts as a headshot
    constexpr float kEyeHeightM = 1.62f;

    inline float Dot3(const float a[3], const float b[3])
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    inline float Len3(const float v[3])
    {
        return std::sqrt(Dot3(v, v));
    }

    inline float Dist2(const float a[3], const float b[3])
    {
        const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
        return Dot3(d, d);
    }

    inline bool Normalize3(float v[3])
    {
        const float len = Len3(v);
        if (len < 1.0e-6f || !std::isfinite(len))
            return false;
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
        return true;
    }

    inline void Cross3(float out[3], const float a[3], const float b[3])
    {
        out[0] = a[1] * b[2] - a[2] * b[1];
        out[1] = a[2] * b[0] - a[0] * b[2];
        out[2] = a[0] * b[1] - a[1] * b[0];
    }

    /// Linear damage falloff between start and end range.
    inline float LinearFalloff(float damage, float minDamage, float startM, float endM, float distM)
    {
        if (distM <= startM || endM <= startM)
            return damage;
        if (distM >= endM)
            return minDamage;
        const float k = (distM - startM) / (endM - startM);
        return damage + (minDamage - damage) * k;
    }

    /// Randomly deviate a normalized direction inside a cone (uniform over the disk).
    inline void PerturbCone(float dir[3], float halfAngleDeg, std::mt19937& rng)
    {
        if (halfAngleDeg <= 0.0f)
            return;
        std::uniform_real_distribution<float> uni(0.0f, 1.0f);
        const float maxTan = std::tan(halfAngleDeg * 0.017453293f);
        const float r = maxTan * std::sqrt(uni(rng));
        const float theta = uni(rng) * 6.2831853f;

        float up[3] = {0.0f, 1.0f, 0.0f};
        if (std::fabs(dir[1]) > 0.99f)
        {
            up[0] = 1.0f;
            up[1] = 0.0f;
        }
        float right[3];
        Cross3(right, up, dir);
        if (!Normalize3(right))
            return;
        float realUp[3];
        Cross3(realUp, dir, right);

        const float dx = r * std::cos(theta);
        const float dy = r * std::sin(theta);
        dir[0] += right[0] * dx + realUp[0] * dy;
        dir[1] += right[1] * dx + realUp[1] * dy;
        dir[2] += right[2] * dx + realUp[2] * dy;
        Normalize3(dir);
    }

    /// Squared closest distance between segments p1->q1 and p2->q2 (Ericson 5.1.9).
    /// s/t are the normalized params of the closest points on each segment.
    inline float ClosestSegSeg2(const float p1[3], const float q1[3], const float p2[3], const float q2[3], float& s,
                                float& t)
    {
        const float d1[3] = {q1[0] - p1[0], q1[1] - p1[1], q1[2] - p1[2]};
        const float d2[3] = {q2[0] - p2[0], q2[1] - p2[1], q2[2] - p2[2]};
        const float r[3] = {p1[0] - p2[0], p1[1] - p2[1], p1[2] - p2[2]};
        const float a = Dot3(d1, d1);
        const float e = Dot3(d2, d2);
        const float f = Dot3(d2, r);
        constexpr float kEps = 1.0e-8f;

        if (a <= kEps && e <= kEps)
        {
            s = t = 0.0f;
            return Dot3(r, r);
        }
        if (a <= kEps)
        {
            s = 0.0f;
            t = std::clamp(f / e, 0.0f, 1.0f);
        }
        else
        {
            const float c = Dot3(d1, r);
            if (e <= kEps)
            {
                t = 0.0f;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            }
            else
            {
                const float b = Dot3(d1, d2);
                const float denom = a * e - b * b;
                s = denom > kEps ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
                t = (b * s + f) / e;
                if (t < 0.0f)
                {
                    t = 0.0f;
                    s = std::clamp(-c / a, 0.0f, 1.0f);
                }
                else if (t > 1.0f)
                {
                    t = 1.0f;
                    s = std::clamp((b - c) / a, 0.0f, 1.0f);
                }
            }
        }

        const float c1[3] = {p1[0] + d1[0] * s, p1[1] + d1[1] * s, p1[2] + d1[2] * s};
        const float c2[3] = {p2[0] + d2[0] * t, p2[1] + d2[1] * t, p2[2] + d2[2] * t};
        return Dist2(c1, c2);
    }

    /// Does segment a->b pass within `radius` of the vertical capsule axis
    /// starting at capBase (feet) with the given height? outT = param on a->b.
    inline bool SegmentHitsCapsule(const float a[3], const float b[3], const float capBase[3], float height,
                                   float radius, float& outT)
    {
        const float top[3] = {capBase[0], capBase[1] + height, capBase[2]};
        float s = 0.0f, t = 0.0f;
        const float d2 = ClosestSegSeg2(a, b, capBase, top, s, t);
        if (d2 > radius * radius)
            return false;
        outT = s;
        return true;
    }

} // namespace Terrafront::WeaponMath
