/**
 * @file AnimationClipManager.cpp
 * @brief AnimationTrack, AnimationClip methods, and AnimationTimeline file I/O
 */

#include "AnimationTimeline.h"
#include "Utils/Validate.h"
#include <algorithm>
#include <fstream>
#include <sstream>

using namespace DirectX;
namespace SparkEditor
{

    // ============================================================================
    // AnimationTrack
    // ============================================================================

    AnimationCurve* AnimationTrack::AddCurve(const std::string& propertyPath, const std::string& displayName)
    {
        auto curve = std::make_unique<AnimationCurve>();
        curve->propertyPath = propertyPath;
        curve->displayName = displayName;
        AnimationCurve* ptr = curve.get();
        curves.push_back(std::move(curve));
        return ptr;
    }

    void AnimationTrack::RemoveCurve(const std::string& propertyPath)
    {
        curves.erase(std::remove_if(curves.begin(), curves.end(), [&](const std::unique_ptr<AnimationCurve>& c)
                                    { return c->propertyPath == propertyPath; }),
                     curves.end());
    }

    AnimationCurve* AnimationTrack::FindCurve(const std::string& propertyPath)
    {
        for (auto& curve : curves)
        {
            if (curve->propertyPath == propertyPath)
            {
                return curve.get();
            }
        }
        return nullptr;
    }

    // ============================================================================
    // AnimationClip
    // ============================================================================

    AnimationTrack* AnimationClip::AddTrack(ObjectID objectID, const std::string& objectName)
    {
        // Check if track already exists
        if (auto* existing = FindTrack(objectID))
        {
            return existing;
        }
        auto track = std::make_unique<AnimationTrack>();
        track->objectID = objectID;
        track->objectName = objectName;
        AnimationTrack* ptr = track.get();
        tracks.push_back(std::move(track));
        return ptr;
    }

    void AnimationClip::RemoveTrack(ObjectID objectID)
    {
        tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                    [&](const std::unique_ptr<AnimationTrack>& t) { return t->objectID == objectID; }),
                     tracks.end());
    }

    AnimationTrack* AnimationClip::FindTrack(ObjectID objectID)
    {
        for (auto& track : tracks)
        {
            if (track->objectID == objectID)
            {
                return track.get();
            }
        }
        return nullptr;
    }

    void AnimationClip::Evaluate(std::unordered_map<std::string, XMFLOAT4>& outValues) const
    {
        for (auto& track : tracks)
        {
            if (track->isMuted)
                continue;
            for (auto& curve : track->curves)
            {
                if (curve->isMuted)
                    continue;
                outValues[curve->propertyPath] = curve->Evaluate(currentTime);
            }
        }
    }

    void AnimationClip::SetTime(float time)
    {
        currentTime = std::max(0.0f, std::min(time, duration));
    }

    // ============================================================================
    // AnimationTimeline — Clip management / file I/O
    // ============================================================================

    void AnimationTimeline::CreateNewClip(const std::string& name, float duration, float frameRate)
    {
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Editor, name);
        auto clip = std::make_unique<AnimationClip>();
        clip->name = name;
        clip->duration = duration;
        clip->frameRate = frameRate;
        clip->currentTime = 0.0f;
        m_currentClip = std::move(clip);
        m_viewStartTime = 0.0f;
        m_viewEndTime = duration;
        m_selection.Clear();
        m_playbackState = PlaybackState::STOPPED;
    }

    bool AnimationTimeline::LoadAnimationClip(const std::string& filePath)
    {
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !filePath.empty(), false);
        std::ifstream file(filePath);
        if (!file.is_open())
            return false;

        auto clip = std::make_unique<AnimationClip>();
        std::string line;

        // Read header
        if (!std::getline(file, line) || line != "SPARK_ANIM")
            return false;

        // Name
        if (std::getline(file, line))
            clip->name = line;
        // Duration, frameRate, looping
        if (std::getline(file, line))
        {
            std::istringstream iss(line);
            iss >> clip->duration >> clip->frameRate;
            int looping = 0;
            if (iss >> looping)
                clip->isLooping = (looping != 0);
        }

        // Read tracks
        int trackCount = 0;
        if (std::getline(file, line))
        {
            std::istringstream iss(line);
            iss >> trackCount;
        }

        for (int ti = 0; ti < trackCount; ++ti)
        {
            ObjectID objID = 0;
            std::string objName;
            int curveCount = 0;

            if (std::getline(file, line))
            {
                std::istringstream iss(line);
                iss >> objID;
                std::getline(iss >> std::ws, objName);
            }
            if (std::getline(file, line))
            {
                std::istringstream iss(line);
                iss >> curveCount;
            }

            AnimationTrack* track = clip->AddTrack(objID, objName);

            for (int ci = 0; ci < curveCount; ++ci)
            {
                std::string propPath, displayName;
                int kfCount = 0;

                if (std::getline(file, propPath))
                {
                }
                if (std::getline(file, displayName))
                {
                }
                if (std::getline(file, line))
                {
                    std::istringstream iss(line);
                    iss >> kfCount;
                }

                AnimationCurve* curve = track->AddCurve(propPath, displayName);
                for (int ki = 0; ki < kfCount; ++ki)
                {
                    if (std::getline(file, line))
                    {
                        AnimationKeyframe kf;
                        int interp = 0;
                        std::istringstream iss(line);
                        iss >> kf.time >> kf.value.x >> kf.value.y >> kf.value.z >> kf.value.w >> kf.inTangent.x >>
                            kf.inTangent.y >> kf.outTangent.x >> kf.outTangent.y >> interp;
                        kf.interpolation = static_cast<AnimationKeyframe::InterpolationType>(interp);
                        curve->AddKeyframe(kf);
                    }
                }
            }
        }

        m_currentClip = std::move(clip);
        AutoFitView();
        m_selection.Clear();
        m_playbackState = PlaybackState::STOPPED;
        return true;
    }

    bool AnimationTimeline::SaveAnimationClip(const std::string& filePath)
    {
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !filePath.empty(), false);
        if (!m_currentClip)
            return false;

        std::ofstream file(filePath);
        if (!file.is_open())
            return false;

        // Header
        file << "SPARK_ANIM\n";
        file << m_currentClip->name << "\n";
        file << m_currentClip->duration << " " << m_currentClip->frameRate << " " << (m_currentClip->isLooping ? 1 : 0)
             << "\n";

        file << m_currentClip->tracks.size() << "\n";

        for (auto& track : m_currentClip->tracks)
        {
            file << track->objectID << " " << track->objectName << "\n";
            file << track->curves.size() << "\n";

            for (auto& curve : track->curves)
            {
                file << curve->propertyPath << "\n";
                file << curve->displayName << "\n";
                file << curve->keyframes.size() << "\n";

                for (auto& kf : curve->keyframes)
                {
                    file << kf.time << " " << kf.value.x << " " << kf.value.y << " " << kf.value.z << " " << kf.value.w
                         << " " << kf.inTangent.x << " " << kf.inTangent.y << " " << kf.outTangent.x << " "
                         << kf.outTangent.y << " " << static_cast<int>(kf.interpolation) << "\n";
                }
            }
        }

        return true;
    }

    void AnimationTimeline::SetCurrentClip(std::unique_ptr<AnimationClip> clip)
    {
        m_currentClip = std::move(clip);
        m_selection.Clear();
        m_playbackState = PlaybackState::STOPPED;
        if (m_currentClip)
        {
            AutoFitView();
        }
    }

} // namespace SparkEditor
