// TestSequencer.cpp - Tests for cinematic sequencer / timeline logic
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

namespace TestSequencer {

struct Vec3 {
    float x, y, z;
};

static Vec3 LerpVec3(const Vec3& a, const Vec3& b, float t) {
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
}

struct CameraKeyframe {
    float time;
    Vec3 position;
    Vec3 lookAt;
    float fov;
};

CameraKeyframe EvaluateCamera(const std::vector<CameraKeyframe>& keyframes, float time) {
    if (keyframes.empty()) return {0, {0,0,0}, {0,0,-1}, 60.0f};
    if (keyframes.size() == 1 || time <= keyframes.front().time) return keyframes.front();
    if (time >= keyframes.back().time) return keyframes.back();

    for (size_t i = 0; i < keyframes.size() - 1; ++i) {
        if (time >= keyframes[i].time && time < keyframes[i + 1].time) {
            float dur = keyframes[i + 1].time - keyframes[i].time;
            float t = (dur > 0.0001f) ? (time - keyframes[i].time) / dur : 0.0f;
            CameraKeyframe result;
            result.time = time;
            result.position = LerpVec3(keyframes[i].position, keyframes[i + 1].position, t);
            result.lookAt = LerpVec3(keyframes[i].lookAt, keyframes[i + 1].lookAt, t);
            result.fov = keyframes[i].fov + (keyframes[i + 1].fov - keyframes[i].fov) * t;
            return result;
        }
    }
    return keyframes.back();
}

struct SubtitleCue {
    float startTime;
    float endTime;
    std::string text;
};

const SubtitleCue* GetActiveSubtitle(const std::vector<SubtitleCue>& subs, float time) {
    for (const auto& sub : subs) {
        if (time >= sub.startTime && time < sub.endTime) return &sub;
    }
    return nullptr;
}

struct EventCue {
    float time;
    std::string name;
};

std::vector<const EventCue*> GetTriggeredEvents(const std::vector<EventCue>& cues, float prevTime, float curTime) {
    std::vector<const EventCue*> triggered;
    for (const auto& cue : cues) {
        if (cue.time > prevTime && cue.time <= curTime) {
            triggered.push_back(&cue);
        }
    }
    return triggered;
}

} // namespace TestSequencer

TEST(Camera_SingleKeyframe)
{
    std::vector<TestSequencer::CameraKeyframe> kfs = {
        {0.0f, {1, 2, 3}, {0, 0, 0}, 60.0f}
    };
    auto result = TestSequencer::EvaluateCamera(kfs, 0.0f);
    EXPECT_NEAR(result.position.x, 1.0f, 0.001f);
    EXPECT_NEAR(result.fov, 60.0f, 0.001f);
}

TEST(Camera_InterpolateBetweenKeyframes)
{
    std::vector<TestSequencer::CameraKeyframe> kfs = {
        {0.0f, {0, 0, 0}, {0, 0, 0}, 60.0f},
        {2.0f, {10, 0, 0}, {0, 0, 0}, 90.0f}
    };
    auto result = TestSequencer::EvaluateCamera(kfs, 1.0f);
    EXPECT_NEAR(result.position.x, 5.0f, 0.001f);
    EXPECT_NEAR(result.fov, 75.0f, 0.001f);
}

TEST(Camera_BeforeFirstKeyframe)
{
    std::vector<TestSequencer::CameraKeyframe> kfs = {
        {1.0f, {5, 0, 0}, {0, 0, 0}, 60.0f},
        {3.0f, {15, 0, 0}, {0, 0, 0}, 90.0f}
    };
    auto result = TestSequencer::EvaluateCamera(kfs, 0.0f);
    EXPECT_NEAR(result.position.x, 5.0f, 0.001f);
}

TEST(Camera_AfterLastKeyframe)
{
    std::vector<TestSequencer::CameraKeyframe> kfs = {
        {0.0f, {0, 0, 0}, {0, 0, 0}, 60.0f},
        {1.0f, {10, 0, 0}, {0, 0, 0}, 90.0f}
    };
    auto result = TestSequencer::EvaluateCamera(kfs, 5.0f);
    EXPECT_NEAR(result.position.x, 10.0f, 0.001f);
    EXPECT_NEAR(result.fov, 90.0f, 0.001f);
}

TEST(Camera_ThreeKeyframes)
{
    std::vector<TestSequencer::CameraKeyframe> kfs = {
        {0.0f, {0, 0, 0}, {0, 0, 0}, 60.0f},
        {1.0f, {10, 0, 0}, {0, 0, 0}, 60.0f},
        {2.0f, {10, 10, 0}, {0, 0, 0}, 60.0f}
    };
    auto result = TestSequencer::EvaluateCamera(kfs, 1.5f);
    EXPECT_NEAR(result.position.x, 10.0f, 0.001f);
    EXPECT_NEAR(result.position.y, 5.0f, 0.001f);
}

TEST(Subtitle_ActiveDuringRange)
{
    std::vector<TestSequencer::SubtitleCue> subs = {
        {1.0f, 3.0f, "Hello"},
        {5.0f, 7.0f, "World"}
    };
    auto* sub = TestSequencer::GetActiveSubtitle(subs, 2.0f);
    EXPECT_TRUE(sub != nullptr);
    EXPECT_EQ(sub->text, std::string("Hello"));
}

TEST(Subtitle_NullOutsideRange)
{
    std::vector<TestSequencer::SubtitleCue> subs = {
        {1.0f, 3.0f, "Hello"}
    };
    EXPECT_TRUE(TestSequencer::GetActiveSubtitle(subs, 0.5f) == nullptr);
    EXPECT_TRUE(TestSequencer::GetActiveSubtitle(subs, 3.5f) == nullptr);
}

TEST(Events_TriggeredInRange)
{
    std::vector<TestSequencer::EventCue> cues = {
        {1.0f, "explosion"},
        {2.0f, "door_open"},
        {5.0f, "music_start"}
    };
    auto triggered = TestSequencer::GetTriggeredEvents(cues, 0.0f, 2.5f);
    EXPECT_EQ(triggered.size(), 2u);
    EXPECT_EQ(triggered[0]->name, std::string("explosion"));
    EXPECT_EQ(triggered[1]->name, std::string("door_open"));
}

TEST(Events_NoneTriggered)
{
    std::vector<TestSequencer::EventCue> cues = {
        {5.0f, "late_event"}
    };
    auto triggered = TestSequencer::GetTriggeredEvents(cues, 0.0f, 3.0f);
    EXPECT_EQ(triggered.size(), 0u);
}

TEST(Events_ExactBoundary)
{
    std::vector<TestSequencer::EventCue> cues = {
        {2.0f, "exact_event"}
    };
    // prevTime exclusive, currentTime inclusive
    auto triggered = TestSequencer::GetTriggeredEvents(cues, 1.0f, 2.0f);
    EXPECT_EQ(triggered.size(), 1u);
}
