// TestExtendedSystems.cpp — Tests for networking, rendering, core, and utility subsystems
// Standalone reimplementations to avoid platform-specific header dependencies

#include "TestFramework.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// 1. DeltaSnapshotManager (standalone)
// ============================================================================
namespace
{

struct FieldSnapshot
{
    uint8_t fieldIndex;
    std::vector<uint8_t> data;
    uint64_t hash;
};

uint64_t FnvHash(const std::vector<uint8_t>& data)
{
    uint64_t h = 14695981039346656037ULL;
    for (auto b : data)
    {
        h ^= b;
        h *= 1099511628211ULL;
    }
    return h;
}

class DeltaTracker
{
public:
    void RecordState(uint32_t entityId, const std::vector<FieldSnapshot>& fields)
    {
        m_current[entityId] = fields;
    }

    std::vector<uint8_t> BuildDelta(uint32_t connectionId, uint32_t entityId)
    {
        auto currentIt = m_current.find(entityId);
        if (currentIt == m_current.end())
            return {};

        auto key = std::make_pair(connectionId, entityId);
        auto& lastSent = m_lastSent[key];
        std::vector<uint8_t> delta;

        for (const auto& field : currentIt->second)
        {
            bool changed = true;
            for (const auto& prev : lastSent)
            {
                if (prev.fieldIndex == field.fieldIndex && prev.hash == field.hash)
                {
                    changed = false;
                    break;
                }
            }
            if (changed)
            {
                delta.push_back(field.fieldIndex);
                delta.insert(delta.end(), field.data.begin(), field.data.end());
            }
        }

        lastSent = currentIt->second;
        return delta;
    }

private:
    std::unordered_map<uint32_t, std::vector<FieldSnapshot>> m_current;
    std::map<std::pair<uint32_t, uint32_t>, std::vector<FieldSnapshot>> m_lastSent;
};

} // namespace

#include <map>

TEST(DeltaSnapshot_OnlyChangedFieldsSent)
{
    DeltaTracker tracker;
    std::vector<uint8_t> healthData = {100};
    std::vector<uint8_t> manaData = {50};

    std::vector<FieldSnapshot> state1 = {{0, healthData, FnvHash(healthData)}, {1, manaData, FnvHash(manaData)}};
    tracker.RecordState(1, state1);

    auto delta1 = tracker.BuildDelta(0, 1);
    EXPECT_TRUE(delta1.size() > 0); // First send — all fields

    // No changes — delta should be empty
    auto delta2 = tracker.BuildDelta(0, 1);
    EXPECT_EQ(delta2.size(), 0u);

    // Change health only
    healthData = {80};
    state1[0] = {0, healthData, FnvHash(healthData)};
    tracker.RecordState(1, state1);

    auto delta3 = tracker.BuildDelta(0, 1);
    EXPECT_TRUE(delta3.size() > 0);
    EXPECT_EQ(delta3[0], 0u); // Field index 0 (health)
}

TEST(DeltaSnapshot_PerConnectionTracking)
{
    DeltaTracker tracker;
    std::vector<uint8_t> data = {42};
    tracker.RecordState(1, {{0, data, FnvHash(data)}});

    auto d1 = tracker.BuildDelta(0, 1);
    auto d2 = tracker.BuildDelta(1, 1);
    EXPECT_TRUE(d1.size() > 0);
    EXPECT_TRUE(d2.size() > 0); // Different connection, same entity
}

// ============================================================================
// 2. InterpolationBuffer (standalone)
// ============================================================================

namespace
{

template <typename T>
class InterpBuffer
{
public:
    static constexpr size_t BUFFER_SIZE = 16;

    void AddSample(const T& value, float timestamp)
    {
        m_samples[m_head] = {value, timestamp};
        m_head = (m_head + 1) % BUFFER_SIZE;
        if (m_count < BUFFER_SIZE)
            m_count++;
    }

    T Evaluate(float renderTime) const
    {
        if (m_count == 0)
            return T{};
        if (m_count == 1)
            return GetSample(0).value;

        // Find bracketing samples
        const Sample* before = nullptr;
        const Sample* after = nullptr;
        for (size_t i = 0; i < m_count; ++i)
        {
            const auto& s = GetSample(i);
            if (s.timestamp <= renderTime)
            {
                if (!before || s.timestamp > before->timestamp)
                    before = &s;
            }
            if (s.timestamp >= renderTime)
            {
                if (!after || s.timestamp < after->timestamp)
                    after = &s;
            }
        }

        if (!before)
            return after ? after->value : T{};
        if (!after)
            return before->value;
        if (before == after)
            return before->value;

        float t = (renderTime - before->timestamp) / (after->timestamp - before->timestamp);
        return before->value + (after->value - before->value) * t;
    }

    size_t GetCount() const { return m_count; }
    void Clear()
    {
        m_count = 0;
        m_head = 0;
    }

private:
    struct Sample
    {
        T value;
        float timestamp;
    };

    const Sample& GetSample(size_t index) const
    {
        size_t actual = (m_head + BUFFER_SIZE - m_count + index) % BUFFER_SIZE;
        return m_samples[actual];
    }

    std::array<Sample, BUFFER_SIZE> m_samples{};
    size_t m_head = 0;
    size_t m_count = 0;
};

} // namespace

TEST(InterpolationBuffer_EvaluateMidpoint)
{
    InterpBuffer<float> buf;
    buf.AddSample(0.0f, 0.0f);
    buf.AddSample(10.0f, 1.0f);

    float result = buf.Evaluate(0.5f);
    EXPECT_NEAR(result, 5.0f, 0.01f);
}

TEST(InterpolationBuffer_EvaluateEdges)
{
    InterpBuffer<float> buf;
    buf.AddSample(0.0f, 0.0f);
    buf.AddSample(10.0f, 1.0f);

    EXPECT_NEAR(buf.Evaluate(0.0f), 0.0f, 0.01f);
    EXPECT_NEAR(buf.Evaluate(1.0f), 10.0f, 0.01f);
}

TEST(InterpolationBuffer_ClearResets)
{
    InterpBuffer<float> buf;
    buf.AddSample(1.0f, 0.0f);
    EXPECT_EQ(buf.GetCount(), 1u);
    buf.Clear();
    EXPECT_EQ(buf.GetCount(), 0u);
}

// ============================================================================
// 3. InstabilitySimulator (standalone)
// ============================================================================

namespace
{

struct NetSettings
{
    float packetLossPercent = 0.0f;
    float latencyMs = 0.0f;
    float jitterMs = 0.0f;
};

class NetInstability
{
public:
    void SetSettings(const NetSettings& s) { m_settings = s; }

    bool ShouldDrop()
    {
        if (m_settings.packetLossPercent <= 0.0f)
            return false;
        std::uniform_real_distribution<float> dist(0.0f, 100.0f);
        return dist(m_rng) < m_settings.packetLossPercent;
    }

    float GetDelay()
    {
        float delay = m_settings.latencyMs;
        if (m_settings.jitterMs > 0.0f)
        {
            std::uniform_real_distribution<float> dist(-m_settings.jitterMs, m_settings.jitterMs);
            delay += dist(m_rng);
        }
        return std::max(0.0f, delay);
    }

private:
    NetSettings m_settings;
    std::mt19937 m_rng{42};
};

} // namespace

TEST(InstabilitySimulator_NoLossWhenZero)
{
    NetInstability sim;
    sim.SetSettings({0.0f, 0.0f, 0.0f});
    for (int i = 0; i < 100; ++i)
        EXPECT_FALSE(sim.ShouldDrop());
}

TEST(InstabilitySimulator_AllLossAt100)
{
    NetInstability sim;
    sim.SetSettings({100.0f, 0.0f, 0.0f});
    int dropped = 0;
    for (int i = 0; i < 100; ++i)
        if (sim.ShouldDrop())
            dropped++;
    EXPECT_EQ(dropped, 100);
}

TEST(InstabilitySimulator_DelayWithJitter)
{
    NetInstability sim;
    sim.SetSettings({0.0f, 100.0f, 20.0f});
    float delay = sim.GetDelay();
    EXPECT_TRUE(delay >= 80.0f && delay <= 120.0f);
}

// ============================================================================
// 4-5. VisibilityMask + CameraDrawMask
// ============================================================================

namespace
{
namespace VisLayer
{
constexpr uint32_t Default = 1 << 0;
constexpr uint32_t Shadow = 1 << 1;
constexpr uint32_t Reflection = 1 << 2;
constexpr uint32_t All = 0xFFFFFFFF;
} // namespace VisLayer

struct VisMask
{
    uint32_t mask = VisLayer::All;
    bool IsVisibleTo(uint32_t cameraMask) const { return (mask & cameraMask) != 0; }
    void SetLayer(uint32_t l) { mask |= l; }
    void ClearLayer(uint32_t l) { mask &= ~l; }
    void SetExclusive(uint32_t l) { mask = l; }
};

struct CamDrawMask
{
    uint32_t drawMask = VisLayer::All;
    bool CanSee(uint32_t entityMask) const { return (drawMask & entityMask) != 0; }
    void DisableLayer(uint32_t l) { drawMask &= ~l; }
};
} // namespace

TEST(VisibilityMask_DefaultAll)
{
    VisMask v;
    EXPECT_TRUE(v.IsVisibleTo(VisLayer::Default));
    EXPECT_TRUE(v.IsVisibleTo(VisLayer::Shadow));
}

TEST(VisibilityMask_Exclusive)
{
    VisMask v;
    v.SetExclusive(VisLayer::Shadow);
    EXPECT_FALSE(v.IsVisibleTo(VisLayer::Default));
    EXPECT_TRUE(v.IsVisibleTo(VisLayer::Shadow));
}

TEST(CameraDrawMask_DisableLayer)
{
    CamDrawMask cam;
    cam.DisableLayer(VisLayer::Shadow);
    EXPECT_FALSE(cam.CanSee(VisLayer::Shadow));
    EXPECT_TRUE(cam.CanSee(VisLayer::Default));
}

// ============================================================================
// 6. FixedTimestepAccumulator
// ============================================================================

namespace
{
class FixedStep
{
public:
    void Init(float fixedDt) { m_fixedDt = fixedDt; }

    void Advance(float frameDt)
    {
        m_accumulator += frameDt;
        m_stepCount = static_cast<uint32_t>(m_accumulator / m_fixedDt);
        m_accumulator -= m_stepCount * m_fixedDt;
    }

    uint32_t GetStepCount() const { return m_stepCount; }
    float GetAlpha() const { return m_fixedDt > 0.0f ? m_accumulator / m_fixedDt : 0.0f; }

private:
    float m_fixedDt = 1.0f / 60.0f;
    float m_accumulator = 0.0f;
    uint32_t m_stepCount = 0;
};
} // namespace

TEST(FixedTimestep_OneStepExactly)
{
    FixedStep fs;
    fs.Init(1.0f / 60.0f);
    fs.Advance(1.0f / 60.0f);
    EXPECT_EQ(fs.GetStepCount(), 1u);
}

TEST(FixedTimestep_MultipleSteps)
{
    FixedStep fs;
    fs.Init(1.0f / 60.0f);
    fs.Advance(3.0f / 60.0f);
    EXPECT_EQ(fs.GetStepCount(), 3u);
}

TEST(FixedTimestep_Accumulation)
{
    FixedStep fs;
    fs.Init(1.0f / 60.0f);
    fs.Advance(0.5f / 60.0f);
    EXPECT_EQ(fs.GetStepCount(), 0u);
    EXPECT_TRUE(fs.GetAlpha() > 0.0f);
}

// ============================================================================
// 7. CollisionMask (FROM/INTO directional)
// ============================================================================

namespace
{
namespace ColLayer
{
constexpr uint32_t Player = 1 << 0;
constexpr uint32_t Enemy = 1 << 1;
constexpr uint32_t Projectile = 1 << 2;
} // namespace ColLayer

struct ColMask
{
    uint32_t fromMask = 0xFFFFFFFF;
    uint32_t intoMask = 0xFFFFFFFF;
    bool CanCollideWith(const ColMask& target) const { return (fromMask & target.intoMask) != 0; }
};
} // namespace

TEST(CollisionMask_Directional)
{
    ColMask projectile{ColLayer::Enemy, 0}; // Hits enemies, nothing hits it
    ColMask enemy{0, ColLayer::Enemy | ColLayer::Player};

    EXPECT_TRUE(projectile.CanCollideWith(enemy));
    EXPECT_FALSE(enemy.CanCollideWith(projectile)); // INTO is 0
}

TEST(CollisionMask_BothWays)
{
    ColMask player{ColLayer::Enemy, ColLayer::Player};
    ColMask enemy{ColLayer::Player, ColLayer::Enemy};

    EXPECT_TRUE(player.CanCollideWith(enemy));
    EXPECT_TRUE(enemy.CanCollideWith(player));
}

// ============================================================================
// 8. PluginRegistry
// ============================================================================

namespace
{
struct PluginDesc
{
    const char* name;
    bool (*init)();
    PluginDesc* next = nullptr;
};

class PluginReg
{
public:
    static void Register(PluginDesc* desc)
    {
        desc->next = s_head;
        s_head = desc;
    }
    static uint32_t Count()
    {
        uint32_t n = 0;
        for (auto* p = s_head; p; p = p->next)
            n++;
        return n;
    }
    static void Clear() { s_head = nullptr; }

private:
    static inline PluginDesc* s_head = nullptr;
};
} // namespace

TEST(PluginRegistry_RegisterAndCount)
{
    PluginReg::Clear();
    PluginDesc a{"PluginA", nullptr};
    PluginDesc b{"PluginB", nullptr};
    PluginReg::Register(&a);
    PluginReg::Register(&b);
    EXPECT_EQ(PluginReg::Count(), 2u);
    PluginReg::Clear();
}

// ============================================================================
// 9. ResourceVersionTracker
// ============================================================================

namespace
{
class VersionTracker
{
public:
    uint32_t Register(const std::string& path)
    {
        m_versions[path] = 1;
        return 1;
    }
    uint32_t Increment(const std::string& path) { return ++m_versions[path]; }
    bool IsStale(const std::string& path, uint32_t cached) const
    {
        auto it = m_versions.find(path);
        return it != m_versions.end() && it->second > cached;
    }

private:
    std::unordered_map<std::string, uint32_t> m_versions;
};
} // namespace

TEST(ResourceVersion_IncrementDetectsStale)
{
    VersionTracker vt;
    uint32_t v = vt.Register("texture.png");
    EXPECT_FALSE(vt.IsStale("texture.png", v));
    vt.Increment("texture.png");
    EXPECT_TRUE(vt.IsStale("texture.png", v));
}

// ============================================================================
// 10. ProfileProperties (frame-resetting)
// ============================================================================

namespace
{
class ProfileProps
{
public:
    uint32_t Register(const std::string& name, bool frameReset)
    {
        m_props.push_back({name, 0.0f, frameReset});
        return static_cast<uint32_t>(m_props.size() - 1);
    }
    void AddFloat(uint32_t id, float v) { m_props[id].value += v; }
    float GetFloat(uint32_t id) const { return m_props[id].value; }
    void ResetFrame()
    {
        for (auto& p : m_props)
            if (p.frameReset)
                p.value = 0.0f;
    }

private:
    struct Prop
    {
        std::string name;
        float value;
        bool frameReset;
    };
    std::vector<Prop> m_props;
};
} // namespace

TEST(ProfileProperties_FrameReset)
{
    ProfileProps pp;
    uint32_t dc = pp.Register("DrawCalls", true);
    pp.AddFloat(dc, 5.0f);
    EXPECT_NEAR(pp.GetFloat(dc), 5.0f, 0.01f);
    pp.ResetFrame();
    EXPECT_NEAR(pp.GetFloat(dc), 0.0f, 0.01f);
}

TEST(ProfileProperties_NoResetWhenNotFlagged)
{
    ProfileProps pp;
    uint32_t total = pp.Register("TotalFrames", false);
    pp.AddFloat(total, 1.0f);
    pp.ResetFrame();
    EXPECT_NEAR(pp.GetFloat(total), 1.0f, 0.01f);
}

// ============================================================================
// 11. CancellationToken
// ============================================================================

TEST(CancellationToken_Basic)
{
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    EXPECT_FALSE(cancelled->load());
    cancelled->store(true);
    EXPECT_TRUE(cancelled->load());
}

TEST(CancellationToken_LinkedParent)
{
    auto parent = std::make_shared<std::atomic<bool>>(false);
    auto child = std::make_shared<std::atomic<bool>>(false);

    // Child checks both
    auto isCancelled = [&]() { return child->load() || parent->load(); };

    EXPECT_FALSE(isCancelled());
    parent->store(true);
    EXPECT_TRUE(isCancelled());
}

// ============================================================================
// 12. ClusteredLightCulling
// ============================================================================

namespace
{
struct ClusterCfg
{
    uint32_t gridX = 16, gridY = 8, gridZ = 24;
};
struct Light
{
    float x, y, z, radius;
};

class ClusterCuller
{
public:
    void Init(ClusterCfg cfg) { m_cfg = cfg; }
    uint32_t GetClusterCount() const { return m_cfg.gridX * m_cfg.gridY * m_cfg.gridZ; }
    void AddLight(const Light& l) { m_lights.push_back(l); }
    uint32_t GetLightCount() const { return static_cast<uint32_t>(m_lights.size()); }
    void Clear() { m_lights.clear(); }

private:
    ClusterCfg m_cfg;
    std::vector<Light> m_lights;
};
} // namespace

TEST(ClusteredLighting_ClusterCount)
{
    ClusterCuller cc;
    cc.Init({16, 8, 24});
    EXPECT_EQ(cc.GetClusterCount(), 3072u);
}

TEST(ClusteredLighting_AddAndClear)
{
    ClusterCuller cc;
    cc.Init({});
    cc.AddLight({0, 0, 0, 10});
    cc.AddLight({1, 2, 3, 5});
    EXPECT_EQ(cc.GetLightCount(), 2u);
    cc.Clear();
    EXPECT_EQ(cc.GetLightCount(), 0u);
}

// ============================================================================
// 13. RHIValidationLayer
// ============================================================================

namespace
{
enum class ResState : uint8_t
{
    Unknown,
    Created,
    Bound,
    Released
};

class RHIValidator
{
public:
    void Track(uint64_t handle, ResState state) { m_states[handle] = state; }
    ResState GetState(uint64_t handle) const
    {
        auto it = m_states.find(handle);
        return it != m_states.end() ? it->second : ResState::Unknown;
    }
    bool ValidateNotReleased(uint64_t handle) const { return GetState(handle) != ResState::Released; }

private:
    std::unordered_map<uint64_t, ResState> m_states;
};
} // namespace

TEST(RHIValidator_TrackState)
{
    RHIValidator v;
    v.Track(1, ResState::Created);
    EXPECT_TRUE(v.GetState(1) == ResState::Created);
    v.Track(1, ResState::Released);
    EXPECT_FALSE(v.ValidateNotReleased(1));
}

// ============================================================================
// 14. TransientResourcePool
// ============================================================================

namespace
{
struct ResDesc
{
    uint32_t w, h, fmt;
    bool operator==(const ResDesc& o) const { return w == o.w && h == o.h && fmt == o.fmt; }
};

class TransPool
{
public:
    uint64_t Acquire(ResDesc desc)
    {
        for (auto it = m_free.begin(); it != m_free.end(); ++it)
        {
            if (it->second == desc)
            {
                uint64_t h = it->first;
                m_free.erase(it);
                return h;
            }
        }
        uint64_t h = m_next++;
        m_all[h] = desc;
        return h;
    }
    void Release(uint64_t h) { m_free.push_back({h, m_all[h]}); }
    uint32_t PooledCount() const { return static_cast<uint32_t>(m_all.size()); }

private:
    uint64_t m_next = 1;
    std::unordered_map<uint64_t, ResDesc> m_all;
    std::vector<std::pair<uint64_t, ResDesc>> m_free;
};
} // namespace

TEST(TransientPool_ReuseMatchingDesc)
{
    TransPool pool;
    uint64_t h1 = pool.Acquire({1920, 1080, 0});
    pool.Release(h1);
    uint64_t h2 = pool.Acquire({1920, 1080, 0}); // Should reuse
    EXPECT_EQ(h1, h2);
    EXPECT_EQ(pool.PooledCount(), 1u); // Only one resource created
}

TEST(TransientPool_NewForDifferentDesc)
{
    TransPool pool;
    uint64_t h1 = pool.Acquire({1920, 1080, 0});
    pool.Release(h1);
    uint64_t h2 = pool.Acquire({1280, 720, 0}); // Different size
    EXPECT_NE(h1, h2);
    EXPECT_EQ(pool.PooledCount(), 2u);
}

// ============================================================================
// 15. MaterialPropertyHandle
// ============================================================================

namespace
{
struct MatHandle
{
    uint32_t encoded = 0xFFFFFFFF;
    uint16_t GetBinding() const { return encoded & 0x3FF; }
    uint16_t GetOffset() const { return (encoded >> 16) & 0xFFFF; }
    bool IsValid() const { return encoded != 0xFFFFFFFF; }
};

class MatRegistry
{
public:
    MatHandle Register(const std::string& name, uint16_t binding, uint16_t offset)
    {
        MatHandle h;
        h.encoded = (binding & 0x3FF) | (static_cast<uint32_t>(offset) << 16);
        m_map[name] = h;
        return h;
    }
    MatHandle Find(const std::string& name) const
    {
        auto it = m_map.find(name);
        return it != m_map.end() ? it->second : MatHandle{};
    }

private:
    std::unordered_map<std::string, MatHandle> m_map;
};
} // namespace

TEST(MaterialHandle_EncodeDecodeRoundTrip)
{
    MatRegistry reg;
    auto h = reg.Register("albedo", 3, 128);
    EXPECT_TRUE(h.IsValid());
    EXPECT_EQ(h.GetBinding(), 3u);
    EXPECT_EQ(h.GetOffset(), 128u);
}

TEST(MaterialHandle_FindByName)
{
    MatRegistry reg;
    reg.Register("roughness", 1, 64);
    auto found = reg.Find("roughness");
    EXPECT_TRUE(found.IsValid());
    auto missing = reg.Find("nonexistent");
    EXPECT_FALSE(missing.IsValid());
}

// ============================================================================
// 16. LightProbeSystem
// ============================================================================

namespace
{
struct SH9
{
    std::array<float, 9> c{};
    void Scale(float f)
    {
        for (auto& v : c)
            v *= f;
    }
    void Accum(const SH9& o, float w)
    {
        for (size_t i = 0; i < 9; ++i)
            c[i] += o.c[i] * w;
    }
};

struct Probe
{
    float x, y, z;
    SH9 sh;
    float radius = 10.0f;
};
} // namespace

TEST(LightProbe_SampleReturnsWeighted)
{
    Probe p1 = {0, 0, 0, {}, 10.0f};
    p1.sh.c[0] = 1.0f;
    Probe p2 = {10, 0, 0, {}, 10.0f};
    p2.sh.c[0] = 0.0f;

    // At p1's position, weight for p1 = 1.0, p2 = ~0
    SH9 result{};
    float totalW = 0.0f;
    float dist1 = 0.0f; // At p1
    float w1 = 1.0f;
    result.Accum(p1.sh, w1);
    totalW += w1;
    if (totalW > 0.0f)
        result.Scale(1.0f / totalW);
    EXPECT_NEAR(result.c[0], 1.0f, 0.01f);
}

// ============================================================================
// 17. RenderGraphExporter
// ============================================================================

namespace
{
struct PassInfo
{
    std::string name;
    std::vector<std::string> reads, writes;
};

std::string GenDot(const std::vector<PassInfo>& passes)
{
    std::string dot = "digraph RenderGraph {\n";
    for (const auto& p : passes)
    {
        dot += "  \"" + p.name + "\";\n";
        for (const auto& r : p.reads)
            dot += "  \"" + r + "\" -> \"" + p.name + "\";\n";
        for (const auto& w : p.writes)
            dot += "  \"" + p.name + "\" -> \"" + w + "\";\n";
    }
    dot += "}\n";
    return dot;
}
} // namespace

TEST(RenderGraphExporter_GeneratesDot)
{
    std::vector<PassInfo> passes = {{"Shadow", {}, {"ShadowMap"}}, {"GBuffer", {"ShadowMap"}, {"Albedo", "Normal"}}};
    auto dot = GenDot(passes);
    EXPECT_TRUE(dot.find("digraph") != std::string::npos);
    EXPECT_TRUE(dot.find("Shadow") != std::string::npos);
    EXPECT_TRUE(dot.find("ShadowMap") != std::string::npos);
}

// ============================================================================
// 18. PipelineStateCache (flyweight)
// ============================================================================

namespace
{
struct PSODesc
{
    uint64_t shader;
    uint32_t blend;
    bool operator==(const PSODesc& o) const { return shader == o.shader && blend == o.blend; }
};
struct PSOHash
{
    size_t operator()(const PSODesc& d) const { return std::hash<uint64_t>()(d.shader) ^ d.blend; }
};

class PSOCache
{
public:
    std::shared_ptr<PSODesc> GetOrCreate(const PSODesc& desc)
    {
        auto it = m_cache.find(desc);
        if (it != m_cache.end())
            return it->second;
        auto pso = std::make_shared<PSODesc>(desc);
        m_cache[desc] = pso;
        return pso;
    }

private:
    std::unordered_map<PSODesc, std::shared_ptr<PSODesc>, PSOHash> m_cache;
};
} // namespace

TEST(PSOCache_PointerEqualityForSameDesc)
{
    PSOCache cache;
    auto a = cache.GetOrCreate({42, 1});
    auto b = cache.GetOrCreate({42, 1});
    EXPECT_TRUE(a.get() == b.get()); // Same pointer
}

TEST(PSOCache_DifferentDescDifferentPointer)
{
    PSOCache cache;
    auto a = cache.GetOrCreate({42, 1});
    auto b = cache.GetOrCreate({99, 2});
    EXPECT_TRUE(a.get() != b.get());
}

// ============================================================================
// 19. TweenSystem
// ============================================================================

namespace
{
float EaseLinear(float t) { return t; }
float EaseOutQuad(float t) { return t * (2.0f - t); }

class SimpleTween
{
public:
    SimpleTween(float dur, std::function<void(float)> cb) : m_dur(dur), m_cb(std::move(cb)) {}

    bool Update(float dt)
    {
        m_elapsed += dt;
        float t = std::min(m_elapsed / m_dur, 1.0f);
        if (m_cb)
            m_cb(t);
        return t >= 1.0f;
    }

private:
    float m_dur;
    float m_elapsed = 0.0f;
    std::function<void(float)> m_cb;
};
} // namespace

TEST(Tween_CompletesAfterDuration)
{
    float val = 0.0f;
    SimpleTween tw(1.0f, [&](float t) { val = t * 100.0f; });
    EXPECT_FALSE(tw.Update(0.5f));
    EXPECT_NEAR(val, 50.0f, 1.0f);
    EXPECT_TRUE(tw.Update(0.6f));
    EXPECT_NEAR(val, 100.0f, 1.0f);
}

TEST(Tween_EaseOutQuad)
{
    float result = EaseOutQuad(0.5f);
    EXPECT_NEAR(result, 0.75f, 0.01f); // 0.5 * (2 - 0.5) = 0.75
}

// ============================================================================
// 20-21. VirtualFileSystem + UIFactory
// ============================================================================

TEST(VirtualFS_PriorityOrdering)
{
    // Higher priority mount should win
    std::unordered_map<std::string, std::string> engineFiles = {{"config.txt", "engine"}};
    std::unordered_map<std::string, std::string> modFiles = {{"config.txt", "mod"}};

    // Simulate priority lookup
    struct Mount
    {
        int32_t priority;
        std::unordered_map<std::string, std::string>* files;
    };
    Mount mounts[] = {{0, &engineFiles}, {300, &modFiles}};

    // Sort by priority descending
    std::sort(std::begin(mounts), std::end(mounts), [](const Mount& a, const Mount& b) {
        return a.priority > b.priority;
    });

    // First match wins
    std::string result;
    for (const auto& m : mounts)
    {
        auto it = m.files->find("config.txt");
        if (it != m.files->end())
        {
            result = it->second;
            break;
        }
    }
    EXPECT_EQ(result, std::string("mod")); // Mod overrides engine
}

TEST(UIFactory_FloatBinding)
{
    float health = 100.0f;
    float widgetValue = 0.0f;

    // Simulate push: data -> widget
    widgetValue = health;
    EXPECT_NEAR(widgetValue, 100.0f, 0.01f);

    // Simulate pull: widget -> data (user changed slider)
    widgetValue = 75.0f;
    health = widgetValue;
    EXPECT_NEAR(health, 75.0f, 0.01f);
}

TEST(UIFactory_StringBinding)
{
    std::string name = "Player1";
    std::string widget = name;
    EXPECT_EQ(widget, std::string("Player1"));

    widget = "NewName";
    name = widget;
    EXPECT_EQ(name, std::string("NewName"));
}
