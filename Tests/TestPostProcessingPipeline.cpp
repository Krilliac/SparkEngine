// TestPostProcessingPipeline.cpp - Tests for post-processing pipeline configuration
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

namespace TestPostProc
{

    enum class PostProcessPass
    {
        FXAA,
        DepthOfField,
        MotionBlur,
        Vignette,
        ChromaticAberration,
        FilmGrain,
        LensDistortion,
        LightShafts,
        LensFlare,
        Sharpen,
        Count
    };

    struct PassConfig
    {
        PostProcessPass pass;
        bool enabled = false;
        int priority = 0; // Lower = earlier in pipeline
        float intensity = 1.0f;
    };

    struct FXAASettings
    {
        float qualitySubpix = 0.75f;
        float qualityEdgeThreshold = 0.166f;
        float qualityEdgeThresholdMin = 0.0833f;
    };

    struct DepthOfFieldSettings
    {
        float focusDistance = 10.0f;
        float focusRange = 5.0f;
        float bokehSize = 4.0f;
        int sampleCount = 16;
        bool autofocus = false;
    };

    struct VignetteSettings
    {
        float intensity = 0.3f;
        float smoothness = 2.0f;
        float roundness = 1.0f;
    };

    struct FilmGrainSettings
    {
        float intensity = 0.1f;
        float size = 1.6f;
        float luminanceContribution = 0.8f;
        bool colored = false;
    };

    struct SharpenSettings
    {
        float amount = 0.5f;
        float threshold = 0.1f;
    };

    class PostProcessingPipeline
    {
      public:
        PostProcessingPipeline()
        {
            // Default pass order
            m_passes = {
                {PostProcessPass::FXAA, false, 0, 1.0f},
                {PostProcessPass::DepthOfField, false, 1, 1.0f},
                {PostProcessPass::MotionBlur, false, 2, 1.0f},
                {PostProcessPass::Vignette, false, 3, 1.0f},
                {PostProcessPass::ChromaticAberration, false, 4, 1.0f},
                {PostProcessPass::FilmGrain, false, 5, 1.0f},
                {PostProcessPass::LensDistortion, false, 6, 1.0f},
                {PostProcessPass::LightShafts, false, 7, 1.0f},
                {PostProcessPass::LensFlare, false, 8, 1.0f},
                {PostProcessPass::Sharpen, false, 9, 1.0f},
            };
        }

        void SetPassEnabled(PostProcessPass pass, bool enabled)
        {
            for (auto& p : m_passes)
            {
                if (p.pass == pass)
                {
                    p.enabled = enabled;
                    return;
                }
            }
        }

        bool IsPassEnabled(PostProcessPass pass) const
        {
            for (const auto& p : m_passes)
            {
                if (p.pass == pass)
                    return p.enabled;
            }
            return false;
        }

        void SetPassPriority(PostProcessPass pass, int priority)
        {
            for (auto& p : m_passes)
            {
                if (p.pass == pass)
                {
                    p.priority = priority;
                    return;
                }
            }
        }

        void SetPassIntensity(PostProcessPass pass, float intensity)
        {
            for (auto& p : m_passes)
            {
                if (p.pass == pass)
                {
                    p.intensity = std::clamp(intensity, 0.0f, 2.0f);
                    return;
                }
            }
        }

        float GetPassIntensity(PostProcessPass pass) const
        {
            for (const auto& p : m_passes)
            {
                if (p.pass == pass)
                    return p.intensity;
            }
            return 0.0f;
        }

        std::vector<PostProcessPass> GetOrderedPasses() const
        {
            std::vector<PassConfig> sorted = m_passes;
            std::sort(sorted.begin(), sorted.end(),
                      [](const PassConfig& a, const PassConfig& b) { return a.priority < b.priority; });
            std::vector<PostProcessPass> result;
            for (const auto& p : sorted)
            {
                if (p.enabled)
                    result.push_back(p.pass);
            }
            return result;
        }

        int GetActivePassCount() const
        {
            int count = 0;
            for (const auto& p : m_passes)
                if (p.enabled)
                    count++;
            return count;
        }

        int GetTotalPassCount() const { return static_cast<int>(m_passes.size()); }

        FXAASettings& GetFXAASettings() { return m_fxaa; }
        DepthOfFieldSettings& GetDoFSettings() { return m_dof; }
        VignetteSettings& GetVignetteSettings() { return m_vignette; }
        FilmGrainSettings& GetFilmGrainSettings() { return m_filmGrain; }
        SharpenSettings& GetSharpenSettings() { return m_sharpen; }

      private:
        std::vector<PassConfig> m_passes;
        FXAASettings m_fxaa;
        DepthOfFieldSettings m_dof;
        VignetteSettings m_vignette;
        FilmGrainSettings m_filmGrain;
        SharpenSettings m_sharpen;
    };

} // namespace TestPostProc

// =============================================================================
// Tests
// =============================================================================

TEST(PostProc_DefaultAllDisabled)
{
    TestPostProc::PostProcessingPipeline pp;
    EXPECT_EQ(pp.GetActivePassCount(), 0);
    EXPECT_EQ(pp.GetTotalPassCount(), 10);
}

TEST(PostProc_EnableDisablePass)
{
    TestPostProc::PostProcessingPipeline pp;

    pp.SetPassEnabled(TestPostProc::PostProcessPass::FXAA, true);
    EXPECT_TRUE(pp.IsPassEnabled(TestPostProc::PostProcessPass::FXAA));
    EXPECT_EQ(pp.GetActivePassCount(), 1);

    pp.SetPassEnabled(TestPostProc::PostProcessPass::FXAA, false);
    EXPECT_FALSE(pp.IsPassEnabled(TestPostProc::PostProcessPass::FXAA));
    EXPECT_EQ(pp.GetActivePassCount(), 0);
}

TEST(PostProc_MultiplePasses)
{
    TestPostProc::PostProcessingPipeline pp;

    pp.SetPassEnabled(TestPostProc::PostProcessPass::FXAA, true);
    pp.SetPassEnabled(TestPostProc::PostProcessPass::Vignette, true);
    pp.SetPassEnabled(TestPostProc::PostProcessPass::FilmGrain, true);

    EXPECT_EQ(pp.GetActivePassCount(), 3);
}

TEST(PostProc_PassOrdering)
{
    TestPostProc::PostProcessingPipeline pp;

    pp.SetPassEnabled(TestPostProc::PostProcessPass::Sharpen, true);
    pp.SetPassEnabled(TestPostProc::PostProcessPass::FXAA, true);

    pp.SetPassPriority(TestPostProc::PostProcessPass::FXAA, 0);
    pp.SetPassPriority(TestPostProc::PostProcessPass::Sharpen, 10);

    auto ordered = pp.GetOrderedPasses();
    EXPECT_EQ((int)ordered.size(), 2);
    EXPECT_EQ((int)ordered[0], (int)TestPostProc::PostProcessPass::FXAA);
    EXPECT_EQ((int)ordered[1], (int)TestPostProc::PostProcessPass::Sharpen);
}

TEST(PostProc_PassIntensity)
{
    TestPostProc::PostProcessingPipeline pp;

    pp.SetPassIntensity(TestPostProc::PostProcessPass::Vignette, 0.5f);
    EXPECT_NEAR(pp.GetPassIntensity(TestPostProc::PostProcessPass::Vignette), 0.5f, 0.001f);

    // Should clamp to [0, 2]
    pp.SetPassIntensity(TestPostProc::PostProcessPass::Vignette, 5.0f);
    EXPECT_LE(pp.GetPassIntensity(TestPostProc::PostProcessPass::Vignette), 2.0f);

    pp.SetPassIntensity(TestPostProc::PostProcessPass::Vignette, -1.0f);
    EXPECT_GE(pp.GetPassIntensity(TestPostProc::PostProcessPass::Vignette), 0.0f);
}

TEST(PostProc_FXAASettings)
{
    TestPostProc::PostProcessingPipeline pp;
    auto& fxaa = pp.GetFXAASettings();

    EXPECT_GT(fxaa.qualitySubpix, 0.0f);
    EXPECT_GT(fxaa.qualityEdgeThreshold, 0.0f);
    EXPECT_GT(fxaa.qualityEdgeThresholdMin, 0.0f);

    fxaa.qualitySubpix = 0.5f;
    EXPECT_NEAR(pp.GetFXAASettings().qualitySubpix, 0.5f, 0.001f);
}

TEST(PostProc_DepthOfFieldSettings)
{
    TestPostProc::PostProcessingPipeline pp;
    auto& dof = pp.GetDoFSettings();

    EXPECT_GT(dof.focusDistance, 0.0f);
    EXPECT_GT(dof.focusRange, 0.0f);
    EXPECT_GT(dof.sampleCount, 0);

    dof.focusDistance = 25.0f;
    EXPECT_NEAR(pp.GetDoFSettings().focusDistance, 25.0f, 0.001f);
}

TEST(PostProc_VignetteSettings)
{
    TestPostProc::PostProcessingPipeline pp;
    auto& v = pp.GetVignetteSettings();

    EXPECT_GT(v.intensity, 0.0f);
    EXPECT_GT(v.smoothness, 0.0f);
}

TEST(PostProc_FilmGrainSettings)
{
    TestPostProc::PostProcessingPipeline pp;
    auto& fg = pp.GetFilmGrainSettings();

    EXPECT_GT(fg.intensity, 0.0f);
    EXPECT_FALSE(fg.colored);
}

TEST(PostProc_SharpenSettings)
{
    TestPostProc::PostProcessingPipeline pp;
    auto& s = pp.GetSharpenSettings();

    EXPECT_GT(s.amount, 0.0f);
    EXPECT_GE(s.threshold, 0.0f);
}

TEST(PostProc_OrderedPassesOnlyEnabled)
{
    TestPostProc::PostProcessingPipeline pp;

    pp.SetPassEnabled(TestPostProc::PostProcessPass::FXAA, true);
    pp.SetPassEnabled(TestPostProc::PostProcessPass::FilmGrain, true);

    auto ordered = pp.GetOrderedPasses();
    EXPECT_EQ((int)ordered.size(), 2);

    // Disabled passes should not appear
    for (auto pass : ordered)
    {
        EXPECT_TRUE(pass == TestPostProc::PostProcessPass::FXAA || pass == TestPostProc::PostProcessPass::FilmGrain);
    }
}
