// TestVRSystem.cpp - Tests for VR system logic
// Standalone reimplementation for CI testing (no DirectXMath/OpenXR dependency)

#include "TestFramework.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

namespace
{

    // Lightweight math types mirroring DirectX types used by VRSystem
    struct Float2
    {
        float x = 0.0f, y = 0.0f;
    };
    struct Float3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
    };
    struct Float4
    {
        float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
    };

    struct Float4x4
    {
        float m[4][4] = {};
        static Float4x4 Identity()
        {
            Float4x4 mat{};
            mat.m[0][0] = mat.m[1][1] = mat.m[2][2] = mat.m[3][3] = 1.0f;
            return mat;
        }
    };

    // VR types mirroring Spark::VR
    struct VRController
    {
        bool connected = false;
        Float3 position{};
        Float4 orientation{0, 0, 0, 1};
        float triggerValue = 0.0f;
        float gripValue = 0.0f;
        Float2 thumbstick{};
        uint32_t buttonMask = 0;
    };

    struct VREye
    {
        Float4x4 viewMatrix{};
        Float4x4 projectionMatrix{};
        Float3 position{};
        int viewportWidth = 0;
        int viewportHeight = 0;
    };

    enum class VRTrackingSpace
    {
        Seated,
        RoomScale
    };

    // VRSystem reimplementation
    class VRSystem
    {
      public:
        VRSystem() = default;
        ~VRSystem() { Shutdown(); }

        bool Initialize()
        {
            if (m_initialized)
                return true;

            m_leftEye.viewMatrix = Float4x4::Identity();
            m_leftEye.projectionMatrix = Float4x4::Identity();
            m_leftEye.viewportWidth = m_recommendedWidth;
            m_leftEye.viewportHeight = m_recommendedHeight;
            m_leftEye.position = {-0.032f, 0.0f, 0.0f};

            m_rightEye.viewMatrix = Float4x4::Identity();
            m_rightEye.projectionMatrix = Float4x4::Identity();
            m_rightEye.viewportWidth = m_recommendedWidth;
            m_rightEye.viewportHeight = m_recommendedHeight;
            m_rightEye.position = {0.032f, 0.0f, 0.0f};

            m_initialized = true;
            return true;
        }

        void Shutdown()
        {
            if (!m_initialized)
                return;

            m_leftController = {};
            m_rightController = {};
            m_headPosition = {};
            m_headOrientation = {0, 0, 0, 1};
            m_initialized = false;
        }

        bool IsAvailable() const { return m_initialized; }

        void SetHeadPosition(const Float3& pos) { m_headPosition = pos; }
        Float3 GetHeadPosition() const { return m_headPosition; }

        void SetHeadOrientation(const Float4& ori) { m_headOrientation = ori; }
        Float4 GetHeadOrientation() const { return m_headOrientation; }

        const VREye& GetLeftEye() const { return m_leftEye; }
        const VREye& GetRightEye() const { return m_rightEye; }

        std::pair<int, int> GetRecommendedRenderSize() const { return {m_recommendedWidth, m_recommendedHeight}; }

        const VRController& GetLeftController() const { return m_leftController; }
        const VRController& GetRightController() const { return m_rightController; }

        VRController& MutableLeftController() { return m_leftController; }
        VRController& MutableRightController() { return m_rightController; }

        void TriggerHaptic(bool isLeft, float amplitude, float duration)
        {
            m_lastHapticLeft = isLeft;
            m_lastHapticAmplitude = amplitude;
            m_lastHapticDuration = duration;
        }

        void SetTrackingSpace(VRTrackingSpace space) { m_trackingSpace = space; }
        VRTrackingSpace GetTrackingSpace() const { return m_trackingSpace; }

        void RecenterTracking()
        {
            m_headPosition = {0.0f, 0.0f, 0.0f};
            m_headOrientation = {0.0f, 0.0f, 0.0f, 1.0f};
        }

        std::string Console_GetStatus() const
        {
            std::string status = "VR System: ";
            status += m_initialized ? "Active" : "Inactive";
            status += " | Tracking: ";
            status += (m_trackingSpace == VRTrackingSpace::RoomScale) ? "RoomScale" : "Seated";
            status += " | Render: " + std::to_string(m_recommendedWidth) + "x" + std::to_string(m_recommendedHeight);
            return status;
        }

        // Accessors for haptic test verification
        bool LastHapticWasLeft() const { return m_lastHapticLeft; }
        float LastHapticAmplitude() const { return m_lastHapticAmplitude; }
        float LastHapticDuration() const { return m_lastHapticDuration; }

      private:
        std::atomic<bool> m_initialized{false};
        Float3 m_headPosition{};
        Float4 m_headOrientation{0, 0, 0, 1};
        VREye m_leftEye;
        VREye m_rightEye;
        VRController m_leftController;
        VRController m_rightController;
        VRTrackingSpace m_trackingSpace = VRTrackingSpace::RoomScale;
        int m_recommendedWidth = 1440;
        int m_recommendedHeight = 1600;

        bool m_lastHapticLeft = false;
        float m_lastHapticAmplitude = 0.0f;
        float m_lastHapticDuration = 0.0f;
    };

} // anonymous namespace

TEST(VRSystem_DefaultState)
{
    VRSystem vr;

    EXPECT_FALSE(vr.IsAvailable());
    EXPECT_NEAR(vr.GetHeadPosition().x, 0.0f, 0.001f);
    EXPECT_NEAR(vr.GetHeadPosition().y, 0.0f, 0.001f);
    EXPECT_NEAR(vr.GetHeadPosition().z, 0.0f, 0.001f);
    EXPECT_TRUE(vr.GetTrackingSpace() == VRTrackingSpace::RoomScale);
}

TEST(VRSystem_InitializeShutdown)
{
    VRSystem vr;

    EXPECT_FALSE(vr.IsAvailable());
    EXPECT_TRUE(vr.Initialize());
    EXPECT_TRUE(vr.IsAvailable());

    EXPECT_TRUE(vr.Initialize()); // double init is safe
    EXPECT_TRUE(vr.IsAvailable());

    vr.Shutdown();
    EXPECT_FALSE(vr.IsAvailable());

    EXPECT_TRUE(vr.Initialize()); // re-init after shutdown
    EXPECT_TRUE(vr.IsAvailable());
}

TEST(VRSystem_HeadTracking)
{
    VRSystem vr;
    vr.Initialize();

    vr.SetHeadPosition({1.0f, 1.8f, -0.5f});
    auto pos = vr.GetHeadPosition();
    EXPECT_NEAR(pos.x, 1.0f, 0.001f);
    EXPECT_NEAR(pos.y, 1.8f, 0.001f);
    EXPECT_NEAR(pos.z, -0.5f, 0.001f);

    vr.SetHeadOrientation({0.0f, 0.3827f, 0.0f, 0.9239f}); // ~45 deg around Y
    auto ori = vr.GetHeadOrientation();
    EXPECT_NEAR(ori.x, 0.0f, 0.001f);
    EXPECT_NEAR(ori.y, 0.3827f, 0.001f);
    EXPECT_NEAR(ori.z, 0.0f, 0.001f);
    EXPECT_NEAR(ori.w, 0.9239f, 0.001f);
}

TEST(VRSystem_EyeRenderingData)
{
    VRSystem vr;
    vr.Initialize();

    const auto& leftEye = vr.GetLeftEye();
    const auto& rightEye = vr.GetRightEye();

    EXPECT_TRUE(leftEye.position.x < 0.0f);  // IPD offset left
    EXPECT_TRUE(rightEye.position.x > 0.0f); // IPD offset right

    EXPECT_GT(leftEye.viewportWidth, 0);
    EXPECT_GT(leftEye.viewportHeight, 0);
    EXPECT_GT(rightEye.viewportWidth, 0);
    EXPECT_GT(rightEye.viewportHeight, 0);

    EXPECT_NEAR(leftEye.viewMatrix.m[0][0], 1.0f, 0.001f);
    EXPECT_NEAR(rightEye.viewMatrix.m[0][0], 1.0f, 0.001f);
}

TEST(VRSystem_RecommendedRenderSize)
{
    VRSystem vr;
    vr.Initialize();

    auto [width, height] = vr.GetRecommendedRenderSize();
    EXPECT_EQ(width, 1440);
    EXPECT_EQ(height, 1600);
}

TEST(VRSystem_ControllerState)
{
    VRSystem vr;
    vr.Initialize();

    EXPECT_FALSE(vr.GetLeftController().connected);
    EXPECT_FALSE(vr.GetRightController().connected);

    vr.MutableLeftController().connected = true;
    vr.MutableRightController().connected = true;
    EXPECT_TRUE(vr.GetLeftController().connected);
    EXPECT_TRUE(vr.GetRightController().connected);

    EXPECT_NEAR(vr.GetLeftController().triggerValue, 0.0f, 0.001f);
    EXPECT_NEAR(vr.GetRightController().gripValue, 0.0f, 0.001f);

    vr.MutableRightController().triggerValue = 0.75f;
    vr.MutableLeftController().gripValue = 1.0f;
    EXPECT_NEAR(vr.GetRightController().triggerValue, 0.75f, 0.001f);
    EXPECT_NEAR(vr.GetLeftController().gripValue, 1.0f, 0.001f);
}

TEST(VRSystem_ControllerThumbstick)
{
    VRSystem vr;
    vr.Initialize();
    vr.MutableLeftController().connected = true;

    EXPECT_NEAR(vr.GetLeftController().thumbstick.x, 0.0f, 0.001f);
    EXPECT_NEAR(vr.GetLeftController().thumbstick.y, 0.0f, 0.001f);

    vr.MutableLeftController().thumbstick = {-1.0f, 1.0f};
    EXPECT_NEAR(vr.GetLeftController().thumbstick.x, -1.0f, 0.001f);
    EXPECT_NEAR(vr.GetLeftController().thumbstick.y, 1.0f, 0.001f);
    EXPECT_GE(vr.GetLeftController().thumbstick.x, -1.0f);
    EXPECT_LE(vr.GetLeftController().thumbstick.y, 1.0f);

    vr.MutableLeftController().thumbstick = {0.5f, -0.5f};
    EXPECT_NEAR(vr.GetLeftController().thumbstick.x, 0.5f, 0.001f);
    EXPECT_NEAR(vr.GetLeftController().thumbstick.y, -0.5f, 0.001f);
}

TEST(VRSystem_ButtonMask)
{
    VRSystem vr;
    vr.Initialize();
    vr.MutableRightController().connected = true;

    constexpr uint32_t kBtnA = 0x01, kBtnB = 0x02, kBtnMenu = 0x04;

    EXPECT_EQ(vr.GetRightController().buttonMask, 0u);

    vr.MutableRightController().buttonMask |= kBtnA;
    EXPECT_TRUE((vr.GetRightController().buttonMask & kBtnA) != 0);
    EXPECT_FALSE((vr.GetRightController().buttonMask & kBtnB) != 0);

    vr.MutableRightController().buttonMask |= kBtnB;
    EXPECT_TRUE((vr.GetRightController().buttonMask & kBtnA) != 0);
    EXPECT_TRUE((vr.GetRightController().buttonMask & kBtnB) != 0);

    vr.MutableRightController().buttonMask &= ~kBtnA;
    EXPECT_FALSE((vr.GetRightController().buttonMask & kBtnA) != 0);
    EXPECT_TRUE((vr.GetRightController().buttonMask & kBtnB) != 0);

    vr.MutableRightController().buttonMask = kBtnA | kBtnB | kBtnMenu;
    EXPECT_EQ(vr.GetRightController().buttonMask, 0x07u);
}

TEST(VRSystem_HapticFeedback)
{
    VRSystem vr;
    vr.Initialize();

    vr.TriggerHaptic(true, 0.8f, 0.25f);
    EXPECT_TRUE(vr.LastHapticWasLeft());
    EXPECT_NEAR(vr.LastHapticAmplitude(), 0.8f, 0.001f);
    EXPECT_NEAR(vr.LastHapticDuration(), 0.25f, 0.001f);

    vr.TriggerHaptic(false, 0.5f, 1.0f);
    EXPECT_FALSE(vr.LastHapticWasLeft());
    EXPECT_NEAR(vr.LastHapticAmplitude(), 0.5f, 0.001f);
    EXPECT_NEAR(vr.LastHapticDuration(), 1.0f, 0.001f);

    vr.TriggerHaptic(true, 0.0f, 0.0f); // zero amplitude = stop vibration
    EXPECT_NEAR(vr.LastHapticAmplitude(), 0.0f, 0.001f);
}

TEST(VRSystem_TrackingSpace)
{
    VRSystem vr;

    EXPECT_TRUE(vr.GetTrackingSpace() == VRTrackingSpace::RoomScale); // default

    vr.SetTrackingSpace(VRTrackingSpace::Seated);
    EXPECT_TRUE(vr.GetTrackingSpace() == VRTrackingSpace::Seated);

    vr.SetTrackingSpace(VRTrackingSpace::RoomScale);
    EXPECT_TRUE(vr.GetTrackingSpace() == VRTrackingSpace::RoomScale);
}

TEST(VRSystem_RecenterTracking)
{
    VRSystem vr;
    vr.Initialize();

    vr.SetHeadPosition({2.0f, 1.5f, -3.0f});
    vr.SetHeadOrientation({0.1f, 0.2f, 0.3f, 0.9f});

    vr.RecenterTracking();

    auto pos = vr.GetHeadPosition();
    EXPECT_NEAR(pos.x, 0.0f, 0.001f);
    EXPECT_NEAR(pos.y, 0.0f, 0.001f);
    EXPECT_NEAR(pos.z, 0.0f, 0.001f);

    auto ori = vr.GetHeadOrientation();
    EXPECT_NEAR(ori.x, 0.0f, 0.001f);
    EXPECT_NEAR(ori.y, 0.0f, 0.001f);
    EXPECT_NEAR(ori.z, 0.0f, 0.001f);
    EXPECT_NEAR(ori.w, 1.0f, 0.001f);
}

TEST(VRSystem_ConsoleStatus)
{
    VRSystem vr;

    std::string status = vr.Console_GetStatus();
    EXPECT_STR_CONTAINS(status, "VR System");
    EXPECT_STR_CONTAINS(status, "Inactive");
    EXPECT_STR_CONTAINS(status, "RoomScale");
    EXPECT_STR_CONTAINS(status, "1440");
    EXPECT_STR_CONTAINS(status, "1600");

    vr.Initialize();
    status = vr.Console_GetStatus();
    EXPECT_STR_CONTAINS(status, "Active");

    vr.SetTrackingSpace(VRTrackingSpace::Seated);
    status = vr.Console_GetStatus();
    EXPECT_STR_CONTAINS(status, "Seated");
}
