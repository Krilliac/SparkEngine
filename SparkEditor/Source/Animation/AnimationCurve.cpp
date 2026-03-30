/**
 * @file AnimationCurve.cpp
 * @brief AnimationCurve class methods: Evaluate, AddKeyframe, RemoveKeyframe, FindKeyframe
 */

#include "AnimationTimeline.h"
#include "Utils/LogMacros.h"
#include <algorithm>
#include <cmath>

using namespace DirectX;
namespace SparkEditor
{

    XMFLOAT4 AnimationCurve::Evaluate(float time) const
    {
        if (keyframes.empty())
        {
            return {0, 0, 0, 0};
        }

        // Before first keyframe
        if (time <= keyframes.front().time)
        {
            return keyframes.front().value;
        }

        // After last keyframe
        if (time >= keyframes.back().time)
        {
            return keyframes.back().value;
        }

        // Binary search for surrounding keyframes
        size_t lo = 0;
        size_t hi = keyframes.size() - 1;
        while (lo + 1 < hi)
        {
            size_t mid = (lo + hi) / 2;
            if (keyframes[mid].time <= time)
            {
                lo = mid;
            }
            else
            {
                hi = mid;
            }
        }

        const AnimationKeyframe& k0 = keyframes[lo];
        const AnimationKeyframe& k1 = keyframes[hi];

        float segmentDuration = k1.time - k0.time;
        if (segmentDuration <= 0.0f)
        {
            return k0.value;
        }

        float t = (time - k0.time) / segmentDuration;

        // Helper lambda for component-wise lerp
        auto lerpF4 = [](const XMFLOAT4& a, const XMFLOAT4& b, float s) -> XMFLOAT4
        { return {a.x + (b.x - a.x) * s, a.y + (b.y - a.y) * s, a.z + (b.z - a.z) * s, a.w + (b.w - a.w) * s}; };

        switch (k0.interpolation)
        {
        case AnimationKeyframe::LINEAR:
        {
            return lerpF4(k0.value, k1.value, t);
        }

        case AnimationKeyframe::BEZIER:
        {
            // Cubic bezier: B(t) = (1-t)^3*P0 + 3*(1-t)^2*t*P1 + 3*(1-t)*t^2*P2 + t^3*P3
            // P0 = k0.value, P3 = k1.value
            // P1 = k0.value + outTangent * (segmentDuration/3)
            // P2 = k1.value - inTangent * (segmentDuration/3)
            float oneMinusT = 1.0f - t;
            float b0 = oneMinusT * oneMinusT * oneMinusT;
            float b1 = 3.0f * oneMinusT * oneMinusT * t;
            float b2 = 3.0f * oneMinusT * t * t;
            float b3 = t * t * t;

            float tangentScale = segmentDuration / 3.0f;
            XMFLOAT4 result;
            result.x = b0 * k0.value.x + b1 * (k0.value.x + k0.outTangent.y * tangentScale) +
                       b2 * (k1.value.x - k1.inTangent.y * tangentScale) + b3 * k1.value.x;
            result.y = b0 * k0.value.y + b1 * (k0.value.y + k0.outTangent.y * tangentScale) +
                       b2 * (k1.value.y - k1.inTangent.y * tangentScale) + b3 * k1.value.y;
            result.z = b0 * k0.value.z + b1 * (k0.value.z + k0.outTangent.y * tangentScale) +
                       b2 * (k1.value.z - k1.inTangent.y * tangentScale) + b3 * k1.value.z;
            result.w = b0 * k0.value.w + b1 * (k0.value.w + k0.outTangent.y * tangentScale) +
                       b2 * (k1.value.w - k1.inTangent.y * tangentScale) + b3 * k1.value.w;
            return result;
        }

        case AnimationKeyframe::STEP:
        {
            return k0.value;
        }

        case AnimationKeyframe::EASE_IN:
        {
            // Quadratic ease in: t^2
            float eased = t * t;
            return lerpF4(k0.value, k1.value, eased);
        }

        case AnimationKeyframe::EASE_OUT:
        {
            // Quadratic ease out: 1 - (1-t)^2
            float eased = 1.0f - (1.0f - t) * (1.0f - t);
            return lerpF4(k0.value, k1.value, eased);
        }

        case AnimationKeyframe::EASE_IN_OUT:
        {
            // Cubic ease in-out: t < 0.5 ? 4t^3 : 1 - (-2t+2)^3/2
            float eased;
            if (t < 0.5f)
            {
                eased = 4.0f * t * t * t;
            }
            else
            {
                float f = -2.0f * t + 2.0f;
                eased = 1.0f - (f * f * f) / 2.0f;
            }
            return lerpF4(k0.value, k1.value, eased);
        }

        case AnimationKeyframe::CUSTOM:
        default:
        {
            return lerpF4(k0.value, k1.value, t);
        }
        }
    }

    void AnimationCurve::AddKeyframe(const AnimationKeyframe& keyframe)
    {
        SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Adding keyframe at time=%.3f to curve '%s'", keyframe.time,
                        propertyPath.c_str());
        // Insert maintaining sorted order by time
        auto it = std::lower_bound(keyframes.begin(), keyframes.end(), keyframe.time,
                                   [](const AnimationKeyframe& kf, float t) { return kf.time < t; });
        keyframes.insert(it, keyframe);
    }

    void AnimationCurve::RemoveKeyframe(size_t index)
    {
        if (index < keyframes.size())
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Editor, "Removing keyframe at index %zu from curve '%s'", index,
                            propertyPath.c_str());
            keyframes.erase(keyframes.begin() + static_cast<ptrdiff_t>(index));
        }
        else
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "Invalid keyframe index %zu (curve has %zu keyframes)", index,
                           keyframes.size());
        }
    }

    int AnimationCurve::FindKeyframe(float time, float tolerance) const
    {
        for (size_t i = 0; i < keyframes.size(); ++i)
        {
            if (std::abs(time - keyframes[i].time) < tolerance)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

} // namespace SparkEditor
