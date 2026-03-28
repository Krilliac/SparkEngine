/**
 * @file ParticleSimulate.hlsl
 * @brief GPU compute shader for particle simulation
 *
 * Updates particle positions, velocities, lifetimes, and visual properties.
 * Dead particles are appended to the free list for reuse by the emit shader.
 *
 * Dispatch: ceil(maxParticles / 256) groups of 256 threads each.
 */

// Per-particle data in GPU memory
struct GPUParticle
{
    float3 position;
    float  lifetime;      // remaining lifetime
    float3 velocity;
    float  age;           // total age (lifetime_initial - lifetime)
    float4 color;         // current RGBA
    float  size;          // current billboard size
    float  rotation;      // current rotation angle (radians)
    float  rotationSpeed; // angular velocity
    uint   alive;         // 1 = alive, 0 = dead
};

// Simulation parameters uploaded per-frame
cbuffer SimulationConstants : register(b0)
{
    float  deltaTime;
    float  totalTime;
    float3 gravity;
    float  drag;
    uint   maxParticles;
    float  pad0;

    // Wind
    float3 windDirection;
    float  windStrength;

    // Turbulence
    float  turbulenceStrength;
    float  turbulenceFrequency;
    float  turbulenceOctaves;
    float  pad1;

    // Camera for sorting
    float3 cameraPosition;
    float  pad2;
};

// Color gradient for "color over lifetime" (up to 8 keys)
cbuffer ColorGradient : register(b1)
{
    float4 colorKeys[8]; // xyz = RGB, w = time (0..1)
    uint   colorKeyCount;
    float3 pad3;
};

// Size curve for "size over lifetime" (up to 8 keys)
cbuffer SizeCurve : register(b2)
{
    float4 sizeKeys[8]; // x = time, y = multiplier
    uint   sizeKeyCount;
    float3 pad4;
};

// Particle pool (read-write)
RWStructuredBuffer<GPUParticle> particles : register(u0);

// Dead list: indices of dead particles available for reuse
AppendStructuredBuffer<uint> deadList : register(u1);

// Alive count + indirect draw args: [aliveCount, verticesPerInstance, startVertex, startInstance]
RWBuffer<uint> indirectArgs : register(u2);

// Alive particle indices for rendering (compacted)
RWStructuredBuffer<uint> aliveList : register(u3);

// Sort keys for back-to-front transparency (camera distance)
RWStructuredBuffer<float> sortKeys : register(u4);

// Simple pseudo-random noise based on particle index and time
float Hash(uint seed)
{
    seed = seed * 747796405u + 2891336453u;
    seed = ((seed >> ((seed >> 28u) + 4u)) ^ seed) * 277803737u;
    return float(seed >> 1) / float(0x7FFFFFFF);
}

// Sample color gradient at normalized lifetime t
float4 SampleGradient(float t)
{
    if (colorKeyCount == 0)
        return float4(1, 1, 1, 1);

    // Clamp to first/last key
    if (t <= colorKeys[0].w)
        return float4(colorKeys[0].xyz, 1.0);
    if (t >= colorKeys[colorKeyCount - 1].w)
        return float4(colorKeys[colorKeyCount - 1].xyz, 0.0);

    // Interpolate between adjacent keys
    for (uint i = 0; i < colorKeyCount - 1; i++)
    {
        if (t >= colorKeys[i].w && t < colorKeys[i + 1].w)
        {
            float frac = (t - colorKeys[i].w) / (colorKeys[i + 1].w - colorKeys[i].w);
            return lerp(float4(colorKeys[i].xyz, 1.0),
                        float4(colorKeys[i + 1].xyz, 0.0),
                        frac);
        }
    }
    return float4(1, 1, 1, 1);
}

// Sample size curve at normalized lifetime t
float SampleSizeCurve(float t)
{
    if (sizeKeyCount == 0)
        return 1.0;

    if (t <= sizeKeys[0].x)
        return sizeKeys[0].y;
    if (t >= sizeKeys[sizeKeyCount - 1].x)
        return sizeKeys[sizeKeyCount - 1].y;

    for (uint i = 0; i < sizeKeyCount - 1; i++)
    {
        if (t >= sizeKeys[i].x && t < sizeKeys[i + 1].x)
        {
            float frac = (t - sizeKeys[i].x) / (sizeKeys[i + 1].x - sizeKeys[i].x);
            return lerp(sizeKeys[i].y, sizeKeys[i + 1].y, frac);
        }
    }
    return 1.0;
}

// Simple curl-noise-like turbulence approximation
float3 Turbulence(float3 pos, float time)
{
    float3 offset = float3(
        sin(pos.y * turbulenceFrequency + time * 1.3) * cos(pos.z * turbulenceFrequency * 0.7),
        sin(pos.z * turbulenceFrequency + time * 0.9) * cos(pos.x * turbulenceFrequency * 1.1),
        sin(pos.x * turbulenceFrequency + time * 1.1) * cos(pos.y * turbulenceFrequency * 0.8));
    return offset * turbulenceStrength;
}

[numthreads(256, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    if (index >= maxParticles)
        return;

    GPUParticle p = particles[index];

    if (p.alive == 0)
        return;

    // Age the particle
    p.lifetime -= deltaTime;
    p.age += deltaTime;

    // Kill expired particles
    if (p.lifetime <= 0.0)
    {
        p.alive = 0;
        p.lifetime = 0.0;
        particles[index] = p;
        deadList.Append(index);
        return;
    }

    // Normalized lifetime (0 = born, 1 = dead)
    float initialLifetime = p.age + p.lifetime;
    float normalizedAge = p.age / initialLifetime;

    // Apply forces
    float3 acceleration = gravity;

    // Wind
    acceleration += windDirection * windStrength;

    // Turbulence
    if (turbulenceStrength > 0.0)
    {
        acceleration += Turbulence(p.position, totalTime);
    }

    // Integrate velocity (semi-implicit Euler)
    p.velocity += acceleration * deltaTime;

    // Apply drag
    p.velocity *= max(0.0, 1.0 - drag * deltaTime);

    // Integrate position
    p.position += p.velocity * deltaTime;

    // Update visual properties from curves
    p.color = SampleGradient(normalizedAge);
    p.size *= SampleSizeCurve(normalizedAge);
    p.rotation += p.rotationSpeed * deltaTime;

    // Write back
    particles[index] = p;

    // Add to alive list for rendering (atomic append)
    uint aliveIndex;
    InterlockedAdd(indirectArgs[0], 1, aliveIndex);
    aliveList[aliveIndex] = index;

    // Write sort key (distance to camera, negated for back-to-front)
    float3 toCamera = cameraPosition - p.position;
    sortKeys[aliveIndex] = -dot(toCamera, toCamera);
}
