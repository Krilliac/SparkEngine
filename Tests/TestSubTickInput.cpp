// TestSubTickInput.cpp - Tests for sub-tick input buffering

#include "TestFramework.h"
#include "Engine/Networking/SubTickInput.h"

using namespace Spark::Net;

TEST(SubTickInput_RecordAndGetSamples)
{
    SubTickInputBuffer buffer;

    SubTickInputSample sample;
    sample.sequenceNumber = 1;
    sample.tickFraction = 0.25f;
    sample.inputBitfield = 0x01;
    sample.aimDirection = {0.0f, 0.0f, 1.0f};
    sample.timestamp = 0.016f;

    buffer.RecordInput(sample);
    EXPECT_EQ(buffer.GetSampleCount(), static_cast<size_t>(1));

    const auto& samples = buffer.GetSamples();
    EXPECT_EQ(samples.size(), static_cast<size_t>(1));
    EXPECT_EQ(samples[0].sequenceNumber, static_cast<uint32_t>(1));
    EXPECT_NEAR(samples[0].tickFraction, 0.25f, 0.01f);
}

TEST(SubTickInput_Clear)
{
    SubTickInputBuffer buffer;

    SubTickInputSample sample;
    sample.sequenceNumber = 1;
    buffer.RecordInput(sample);
    EXPECT_EQ(buffer.GetSampleCount(), static_cast<size_t>(1));

    buffer.Clear();
    EXPECT_EQ(buffer.GetSampleCount(), static_cast<size_t>(0));
}

TEST(SubTickInput_MultipleSamples)
{
    SubTickInputBuffer buffer;

    for (uint32_t i = 0; i < 5; ++i)
    {
        SubTickInputSample sample;
        sample.sequenceNumber = i;
        sample.tickFraction = static_cast<float>(i) * 0.2f;
        sample.timestamp = static_cast<float>(i) * 0.003f;
        buffer.RecordInput(sample);
    }

    EXPECT_EQ(buffer.GetSampleCount(), static_cast<size_t>(5));
}

TEST(SubTickInput_ProcessInputsForTick)
{
    SubTickInputBuffer buffer;

    SubTickInputSample s1;
    s1.sequenceNumber = 1;
    s1.tickFraction = 0.25f;
    s1.inputBitfield = 0x01;
    buffer.RecordInput(s1);

    SubTickInputSample s2;
    s2.sequenceNumber = 2;
    s2.tickFraction = 0.75f;
    s2.inputBitfield = 0x03;
    buffer.RecordInput(s2);

    int processedCount = 0;
    buffer.ProcessInputsForTick(0, 0.016f,
                                [&processedCount](const SubTickInputSample& sample, float exactTime)
                                {
                                    (void)sample;
                                    (void)exactTime;
                                    processedCount++;
                                });

    EXPECT_EQ(processedCount, 2);
}

TEST(SubTickInput_EmptyBufferProcess)
{
    SubTickInputBuffer buffer;

    int processedCount = 0;
    buffer.ProcessInputsForTick(0, 0.016f, [&processedCount](const SubTickInputSample&, float) { processedCount++; });
    EXPECT_EQ(processedCount, 0);
}
