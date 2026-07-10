/**
 * @file CpuNeuralInference.cpp
 * @brief SIMD-optimized CPU neural network inference implementation
 */

#include "CpuNeuralInference.h"

#include "Utils/JobSystem.h"
#include "Utils/MultiISA.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

// SIMD headers — guarded per platform
#if defined(__SSE2__) || defined(_MSC_VER)
#include <emmintrin.h> // SSE2
#endif

#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))
#include <immintrin.h> // AVX2 + FMA
#endif

namespace Spark::Graphics::Neural
{

    // =========================================================================
    // Function pointer type for ISA-dispatched layer evaluation
    // =========================================================================

    using LayerEvalFunc = void (*)(const float* __restrict input, float* __restrict output,
                                   const float* __restrict weights, const float* __restrict biases, uint32_t inputSize,
                                   uint32_t paddedInputSize, uint32_t outputSize, ActivationType activation);

    static LayerEvalFunc s_evalLayerFunc = nullptr;
    static std::atomic<bool> s_initialized{false};
    static ISALevel s_activeISA = ISALevel::SSE2;

    // =========================================================================
    // Scalar activation helpers
    // =========================================================================

    static inline float ApplyActivation(float x, ActivationType act)
    {
        switch (act)
        {
        case ActivationType::ReLU:
            return x > 0.0f ? x : 0.0f;
        case ActivationType::LeakyReLU:
            return x > 0.0f ? x : 0.01f * x;
        case ActivationType::Sigmoid:
            return 1.0f / (1.0f + std::exp(-x));
        case ActivationType::Tanh:
            return std::tanh(x);
        case ActivationType::None:
            return x;
        }
        return x;
    }

    // =========================================================================
    // Scalar fallback (non-x86 or baseline)
    // =========================================================================

    static void EvaluateLayer_Scalar(const float* __restrict input, float* __restrict output,
                                     const float* __restrict weights, const float* __restrict biases,
                                     uint32_t inputSize, uint32_t paddedInputSize, uint32_t outputSize,
                                     ActivationType activation)
    {
        for (uint32_t j = 0; j < outputSize; ++j)
        {
            float sum = biases[j];
            const float* row = weights + j * paddedInputSize;
            for (uint32_t i = 0; i < inputSize; ++i)
            {
                sum += row[i] * input[i];
            }
            output[j] = ApplyActivation(sum, activation);
        }
    }

    // =========================================================================
    // SSE2 layer evaluation
    // =========================================================================

#if defined(__SSE2__) || defined(_MSC_VER)

    static void EvaluateLayer_SSE2(const float* __restrict input, float* __restrict output,
                                   const float* __restrict weights, const float* __restrict biases, uint32_t inputSize,
                                   uint32_t paddedInputSize, uint32_t outputSize, ActivationType activation)
    {
        for (uint32_t j = 0; j < outputSize; ++j)
        {
            const float* row = weights + j * paddedInputSize;

            __m128 acc = _mm_setzero_ps();
            uint32_t i = 0;
            for (; i + 4 <= paddedInputSize; i += 4)
            {
                __m128 w = _mm_loadu_ps(row + i);
                __m128 x = _mm_loadu_ps(input + i);
                acc = _mm_add_ps(acc, _mm_mul_ps(w, x));
            }

            // Horizontal sum of 4 floats
            __m128 shuf = _mm_shuffle_ps(acc, acc, _MM_SHUFFLE(2, 3, 0, 1));
            __m128 sums = _mm_add_ps(acc, shuf);
            shuf = _mm_movehl_ps(shuf, sums);
            sums = _mm_add_ss(sums, shuf);

            float sum;
            _mm_store_ss(&sum, sums);
            sum += biases[j];
            output[j] = ApplyActivation(sum, activation);
        }
    }

#endif // SSE2

    // =========================================================================
    // AVX2 + FMA layer evaluation
    // =========================================================================

#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))

    static void EvaluateLayer_AVX2(const float* __restrict input, float* __restrict output,
                                   const float* __restrict weights, const float* __restrict biases, uint32_t inputSize,
                                   uint32_t paddedInputSize, uint32_t outputSize, ActivationType activation)
    {
        for (uint32_t j = 0; j < outputSize; ++j)
        {
            const float* row = weights + j * paddedInputSize;

            __m256 acc = _mm256_setzero_ps();
            uint32_t i = 0;
            for (; i + 8 <= paddedInputSize; i += 8)
            {
                __m256 w = _mm256_loadu_ps(row + i);
                __m256 x = _mm256_loadu_ps(input + i);
                acc = _mm256_fmadd_ps(w, x, acc);
            }

            // Handle 4-float tail (paddedInputSize is multiple of 8, but just in case)
            if (i + 4 <= paddedInputSize)
            {
                __m128 w4 = _mm_loadu_ps(row + i);
                __m128 x4 = _mm_loadu_ps(input + i);
                __m128 lo = _mm256_castps256_ps128(acc);
                lo = _mm_fmadd_ps(w4, x4, lo);
                acc = _mm256_insertf128_ps(acc, lo, 0);
            }

            // Horizontal sum of 8 floats
            __m128 hi = _mm256_extractf128_ps(acc, 1);
            __m128 lo = _mm256_castps256_ps128(acc);
            __m128 sum128 = _mm_add_ps(lo, hi);
            __m128 shuf = _mm_shuffle_ps(sum128, sum128, _MM_SHUFFLE(2, 3, 0, 1));
            sum128 = _mm_add_ps(sum128, shuf);
            shuf = _mm_movehl_ps(shuf, sum128);
            sum128 = _mm_add_ss(sum128, shuf);

            float sum;
            _mm_store_ss(&sum, sum128);
            sum += biases[j];
            output[j] = ApplyActivation(sum, activation);
        }
    }

#endif // AVX2

    // =========================================================================
    // Initialize — ISA detection and function pointer binding
    // =========================================================================

    void CpuNeuralInference::Initialize()
    {
        if (s_initialized.load(std::memory_order_acquire))
        {
            return;
        }

        auto& dispatch = MultiISADispatch::GetInstance();
        dispatch.Initialize();

        ISAFunctionEntry<LayerEvalFunc> variants{};
        variants.variants[static_cast<size_t>(ISALevel::SSE2)] = EvaluateLayer_Scalar;

#if defined(__SSE2__) || defined(_MSC_VER)
        variants.variants[static_cast<size_t>(ISALevel::SSE2)] = EvaluateLayer_SSE2;
#endif

#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))
        variants.variants[static_cast<size_t>(ISALevel::AVX2)] = EvaluateLayer_AVX2;
#endif

        s_evalLayerFunc = dispatch.Select(variants.variants);
        s_activeISA = dispatch.GetDetectedLevel();
        s_initialized.store(true, std::memory_order_release);
    }

    bool CpuNeuralInference::IsInitialized()
    {
        return s_initialized.load(std::memory_order_acquire);
    }

    const char* CpuNeuralInference::GetActiveISAName()
    {
        return MultiISADispatch::GetISAName(s_activeISA);
    }

    // =========================================================================
    // PrepareWeights — repack into padded layout
    // =========================================================================

    AlignedWeightLayout CpuNeuralInference::PrepareWeights(const NetworkDesc& desc, const float* weights)
    {
        AlignedWeightLayout layout;
        layout.layerCount = static_cast<uint32_t>(desc.layers.size());

        // First pass: compute total padded size
        uint32_t totalSize = 0;
        for (uint32_t l = 0; l < layout.layerCount; ++l)
        {
            const auto& layer = desc.layers[l];
            uint32_t paddedInput = (layer.inputSize + 7u) & ~7u; // Round up to 8
            uint32_t weightCount = layer.outputSize * paddedInput;
            uint32_t biasCount = layer.outputSize;
            totalSize += weightCount + biasCount;
        }

        layout.data.resize(totalSize, 0.0f);

        // Second pass: copy weights with padding
        uint32_t srcOffset = 0;
        uint32_t dstOffset = 0;

        for (uint32_t l = 0; l < layout.layerCount; ++l)
        {
            const auto& layer = desc.layers[l];
            uint32_t paddedInput = (layer.inputSize + 7u) & ~7u;

            auto& info = layout.layers[l];
            info.inputSize = layer.inputSize;
            info.outputSize = layer.outputSize;
            info.paddedInputSize = paddedInput;
            info.weightOffset = dstOffset;
            info.activation = layer.activation;

            // Copy each row, padding with zeros
            for (uint32_t row = 0; row < layer.outputSize; ++row)
            {
                std::memcpy(&layout.data[dstOffset + row * paddedInput], &weights[srcOffset + row * layer.inputSize],
                            layer.inputSize * sizeof(float));
                // Padding is already zero from resize
            }
            srcOffset += layer.inputSize * layer.outputSize;
            dstOffset += layer.outputSize * paddedInput;

            // Copy biases
            info.biasOffset = dstOffset;
            std::memcpy(&layout.data[dstOffset], &weights[srcOffset], layer.outputSize * sizeof(float));
            srcOffset += layer.outputSize;
            dstOffset += layer.outputSize;
        }

        return layout;
    }

    // =========================================================================
    // EvaluateSingle — one sample through all layers
    // =========================================================================

    void CpuNeuralInference::EvaluateSingle(const AlignedWeightLayout& layout, const float* input, float* output)
    {
        // Thread-local scratch buffers to avoid per-call allocation
        thread_local std::vector<float> bufA(kMaxNeuronsPerLayer, 0.0f);
        thread_local std::vector<float> bufB(kMaxNeuronsPerLayer, 0.0f);

        // Ensure buffers are large enough (should always be, but safe)
        if (bufA.size() < kMaxNeuronsPerLayer)
        {
            bufA.resize(kMaxNeuronsPerLayer, 0.0f);
            bufB.resize(kMaxNeuronsPerLayer, 0.0f);
        }

        // Copy input into padded buffer A
        uint32_t paddedInput0 = layout.layers[0].paddedInputSize;
        std::memset(bufA.data(), 0, paddedInput0 * sizeof(float));
        std::memcpy(bufA.data(), input, layout.layers[0].inputSize * sizeof(float));

        for (uint32_t l = 0; l < layout.layerCount; ++l)
        {
            const auto& info = layout.layers[l];
            const float* weights = &layout.data[info.weightOffset];
            const float* biases = &layout.data[info.biasOffset];

            float* src = (l % 2 == 0) ? bufA.data() : bufB.data();
            float* dst = (l % 2 == 0) ? bufB.data() : bufA.data();

            s_evalLayerFunc(src, dst, weights, biases, info.inputSize, info.paddedInputSize, info.outputSize,
                            info.activation);

            // Pad the output for next layer's input (zero the tail)
            if (l + 1 < layout.layerCount)
            {
                uint32_t nextPadded = layout.layers[l + 1].paddedInputSize;
                for (uint32_t i = info.outputSize; i < nextPadded; ++i)
                {
                    dst[i] = 0.0f;
                }
            }
        }

        // Copy final output
        float* finalBuf = (layout.layerCount % 2 != 0) ? bufB.data() : bufA.data();
        std::memcpy(output, finalBuf, layout.GetOutputSize() * sizeof(float));
    }

    // =========================================================================
    // EvaluateBatch — parallel multi-sample evaluation
    // =========================================================================

    void CpuNeuralInference::EvaluateBatch(const AlignedWeightLayout& layout, const float* input, float* output,
                                           uint32_t batchSize)
    {
        if (batchSize == 0 || layout.layerCount == 0 || !s_evalLayerFunc)
        {
            return;
        }

        uint32_t inputSize = layout.GetInputSize();
        uint32_t outputSize = layout.GetOutputSize();

        auto evalRange = [&](int begin, int end)
        {
            for (int s = begin; s < end; ++s)
            {
                EvaluateSingle(layout, input + s * inputSize, output + s * outputSize);
            }
        };

        // Use JobSystem for large batches
        auto& jobs = JobSystem::Get();
        if (batchSize >= kParallelBatchThreshold && jobs.IsInitialized() && jobs.GetWorkerCount() > 0)
        {
            jobs.ParallelFor(
                0, static_cast<int>(batchSize),
                [&](int i) { EvaluateSingle(layout, input + i * inputSize, output + i * outputSize); },
                8); // min batch size of 8 samples per thread
        }
        else
        {
            evalRange(0, static_cast<int>(batchSize));
        }
    }

} // namespace Spark::Graphics::Neural
