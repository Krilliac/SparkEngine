/**
 * @file ParticleEmit.hlsl
 * @brief GPU compute shader for particle emission
 *
 * Spawns new particles by consuming indices from the dead list
 * and initializing particle data with randomized properties.
 *
 * Dispatch: ceil(emitCount / 64) groups of 64 threads each.
 */

struct GPUParticle
{
    float3 position;
    float  lifetime;
    float3 velocity;
    float  age;
    float4 color;
    float  size;
    float  rotation;
    float  rotationSpeed;
    uint   alive;
};

cbuffer EmitConstants : register(b0)
{
    float3 emitterPosition;
    float  emitCount;

    float3 emitterDirection; // forward direction for cone emitters
    float  emitShape;        // 0=point, 1=sphere, 2=cone, 3=box, 4=circle

    float3 emitterExtents; // box half-extents or sphere radius
    float  coneAngle;      // half-angle in radians for cone emitters

    float  minLifetime;
    float  maxLifetime;
    float  minSpeed;
    float  maxSpeed;

    float  minSize;
    float  maxSize;
    float  minRotation;
    float  maxRotation;

    float  minRotationSpeed;
    float  maxRotationSpeed;
    float  randomSeed; // per-frame random offset
    float  pad0;

    float4 startColor;
};

RWStructuredBuffer<GPUParticle> particles : register(u0);
ConsumeStructuredBuffer<uint>   deadList : register(u1);

// PCG hash for deterministic randomness
float PCGHash(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return float(word >> 1) / float(0x7FFFFFFF);
}

// Random float in [min, max]
float RandomRange(inout uint seed, float minVal, float maxVal)
{
    return lerp(minVal, maxVal, PCGHash(seed));
}

// Random point on unit sphere surface
float3 RandomOnUnitSphere(inout uint seed)
{
    float z = PCGHash(seed) * 2.0 - 1.0;
    float phi = PCGHash(seed) * 6.28318530718;
    float r = sqrt(max(0.0, 1.0 - z * z));
    return float3(r * cos(phi), r * sin(phi), z);
}

// Random point inside unit sphere
float3 RandomInUnitSphere(inout uint seed)
{
    return RandomOnUnitSphere(seed) * pow(PCGHash(seed), 1.0 / 3.0);
}

// Random direction within a cone around 'forward'
float3 RandomInCone(inout uint seed, float3 forward, float halfAngle)
{
    float cosAngle = cos(halfAngle);
    float z = lerp(cosAngle, 1.0, PCGHash(seed));
    float phi = PCGHash(seed) * 6.28318530718;
    float sinTheta = sqrt(max(0.0, 1.0 - z * z));

    float3 localDir = float3(sinTheta * cos(phi), sinTheta * sin(phi), z);

    // Build basis from forward
    float3 up = abs(forward.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 right = normalize(cross(up, forward));
    up = cross(forward, right);

    return normalize(right * localDir.x + up * localDir.y + forward * localDir.z);
}

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint threadIndex = dispatchThreadId.x;
    if (threadIndex >= (uint)emitCount)
        return;

    // Consume a dead particle index
    uint particleIndex = deadList.Consume();

    // Initialize random seed from thread index and per-frame seed
    uint seed = uint(threadIndex * 1973 + randomSeed * 9277 + particleIndex * 26699);

    GPUParticle p;
    p.alive = 1;
    p.age = 0.0;

    // Spawn position based on emitter shape
    uint shape = (uint)emitShape;
    if (shape == 0) // Point
    {
        p.position = emitterPosition;
    }
    else if (shape == 1) // Sphere
    {
        p.position = emitterPosition + RandomInUnitSphere(seed) * emitterExtents.x;
    }
    else if (shape == 2) // Cone
    {
        p.position = emitterPosition;
    }
    else if (shape == 3) // Box
    {
        float3 offset = float3(
            RandomRange(seed, -emitterExtents.x, emitterExtents.x),
            RandomRange(seed, -emitterExtents.y, emitterExtents.y),
            RandomRange(seed, -emitterExtents.z, emitterExtents.z));
        p.position = emitterPosition + offset;
    }
    else // Circle
    {
        float angle = PCGHash(seed) * 6.28318530718;
        float radius = sqrt(PCGHash(seed)) * emitterExtents.x;
        p.position = emitterPosition + float3(cos(angle) * radius, 0, sin(angle) * radius);
    }

    // Spawn velocity
    float speed = RandomRange(seed, minSpeed, maxSpeed);
    if (shape == 2) // Cone
    {
        p.velocity = RandomInCone(seed, emitterDirection, coneAngle) * speed;
    }
    else
    {
        p.velocity = RandomOnUnitSphere(seed) * speed;
    }

    // Properties
    p.lifetime = RandomRange(seed, minLifetime, maxLifetime);
    p.size = RandomRange(seed, minSize, maxSize);
    p.rotation = RandomRange(seed, minRotation, maxRotation);
    p.rotationSpeed = RandomRange(seed, minRotationSpeed, maxRotationSpeed);
    p.color = startColor;

    particles[particleIndex] = p;
}
