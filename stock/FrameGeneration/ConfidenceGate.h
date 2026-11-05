#pragma once
#include <cstdint>
namespace ConfidenceGate
{
// AI-generated comment: Deterministic vetoes, not probabilities. One unsafe moving pixel vetoes a frame.
inline bool Accept(const std::uint32_t *c, std::uint64_t pixels)
{
    return c && pixels && c[3] == pixels && c[0] > 0 && c[0] <= pixels && c[1] == 0 && c[2] == 0;
}
inline const char *Shader = R"HLSL(
Texture2D<float4> beforeImage : register(t0);
Texture2D<float4> afterImage : register(t1);
Texture2D<int2> forwardFlow : register(t2);
Texture2D<int2> backwardFlow : register(t3);
SamplerState linearClamp : register(s0);
cbuffer Dimensions : register(b0)
{
    float2 size;
    float2 gridSize;
}
RWStructuredBuffer<uint> result : register(u0);
groupshared uint totals[4];
float largest(float3 v)
{
    return max(v.r, max(v.g, v.b));
}
float3 currentAt(int2 p)
{
    return afterImage.Load(int3(clamp(p, 0, int2(size) - 1), 0)).rgb;
}
bool endpoint(float3 c)
{
    return largest(c) < 0.02 || (largest(c) > 0.93 && min(c.r, min(c.g, c.b)) > 0.8);
}
float2 flowAt(float2 p, bool back)
{
    float2 g = clamp((p - 1.5) * 0.25, 0, gridSize - 1);
    int2 a = int2(floor(g)), b = min(a + 1, int2(gridSize) - 1);
    float2 f = frac(g);
    float2 v00 = back ? backwardFlow.Load(int3(a, 0)) : forwardFlow.Load(int3(a, 0));
    float2 v10 = back ? backwardFlow.Load(int3(b.x, a.y, 0)) : forwardFlow.Load(int3(b.x, a.y, 0));
    float2 v01 = back ? backwardFlow.Load(int3(a.x, b.y, 0)) : forwardFlow.Load(int3(a.x, b.y, 0));
    float2 v11 = back ? backwardFlow.Load(int3(b, 0)) : forwardFlow.Load(int3(b, 0));
    return lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y) / 32.0;
}
bool thin(float3 c, float3 a, float3 b)
{
    return largest(abs(a - b)) <= 2.0 / 255 && min(largest(abs(c - a)), largest(abs(c - b))) > 4.0 / 255;
}
uint classify(int2 pixel)
{
    float3 c = currentAt(pixel), previous = beforeImage.Load(int3(pixel, 0)).rgb;
    if (endpoint(c) || endpoint(currentAt(pixel + int2(1, 0))) || endpoint(currentAt(pixel - int2(1, 0))) ||
        endpoint(currentAt(pixel + int2(0, 1))) || endpoint(currentAt(pixel - int2(0, 1))))
        return 0;
    float difference = largest(abs(previous - c));
    if (difference < 1.5 / 255)
        return 0;
    float2 centre = float2(pixel), p = centre, q = centre;
    [unroll] for (int i = 0; i < 3; i++)
    {
        p = centre - 0.5 * flowAt(p, false);
        q = centre - 0.5 * flowAt(q, true);
    }
    float2 f = flowAt(p, false), b = flowAt(q, true);
    if (any(p < 0) || any(q < 0) || any(p > size - 1) || any(q > size - 1) || any(p + f < 0) || any(q + b < 0) ||
        any(p + f > size - 1) || any(q + b > size - 1))
        return 3;
    float error = max(length(f + flowAt(p + f, true)), length(b + flowAt(q + b, false)));
    float3 a = beforeImage.SampleLevel(linearClamp, (p + 0.5) / size, 0).rgb,
           z = afterImage.SampleLevel(linearClamp, (q + 0.5) / size, 0).rgb;
    float photo = max(largest(abs(a - afterImage.SampleLevel(linearClamp, (p + f + 0.5) / size, 0).rgb)),
                      largest(abs(z - beforeImage.SampleLevel(linearClamp, (q + b + 0.5) / size, 0).rgb)));
    float gradient = 0;
    [unroll] for (int axis = 0; axis < 4; axis++)
    {
        float2 d = axis == 0 ? float2(4, 0) : axis == 1 ? float2(-4, 0) : axis == 2 ? float2(0, 4) : float2(0, -4);
        gradient = max(gradient, max(length(f - flowAt(p + d, false)), length(b - flowAt(q + d, true))));
    }
    // AI-generated comment: Strict source envelope: even two mutually consistent but wrong flow
    // fields may match repeated background textures. Neither sampled endpoint
    // may depart by more than 2.5 UNORM steps from the current real pixel.
    // This intentionally sacrifices interpolation on unproven moving geometry.
    float sourceError = max(largest(abs(a - c)), largest(abs(z - c)));
    uint flags = 1;
    if (error > 0.5 || photo > 2.5 / 255 || sourceError > 2.5 / 255 || gradient > 0.75 ||
        (difference < 0.008 && max(length(f), length(b)) > 3))
        flags |= 2;
    float3 left = currentAt(pixel - int2(1, 0)), right = currentAt(pixel + int2(1, 0)),
           up = currentAt(pixel - int2(0, 1)), down = currentAt(pixel + int2(0, 1));
    float edge = max(largest(abs(left - right)), largest(abs(up - down)));
    // AI-generated comment: Changed thin geometry cannot be certified by a 4x4 optical-flow block.
    bool narrow = thin(c, left, right) || thin(c, up, down) ||
                  thin(c, currentAt(pixel - int2(2, 0)), currentAt(pixel + int2(2, 0))) ||
                  thin(c, currentAt(pixel - int2(0, 2)), currentAt(pixel + int2(0, 2))) ||
                  thin(c, currentAt(pixel - int2(4, 0)), currentAt(pixel + int2(4, 0))) ||
                  thin(c, currentAt(pixel - int2(0, 4)), currentAt(pixel + int2(0, 4)));
    bool lost = edge > 4.0 / 255 && min(largest(abs(a - c)), largest(abs(z - c))) > 4.0 / 255;
    if (narrow || lost)
        flags |= 4;
    return flags;
}
[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID, uint lane : SV_GroupIndex) {
    if (lane < 4)
        totals[lane] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (all(id.xy < uint2(size)))
    {
        uint flags = classify(int2(id.xy));
        InterlockedAdd(totals[3], 1);
        if (flags & 1)
            InterlockedAdd(totals[0], 1);
        if (flags & 2)
            InterlockedAdd(totals[1], 1);
        if (flags & 4)
            InterlockedAdd(totals[2], 1);
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane < 4)
        InterlockedAdd(result[lane], totals[lane]);
}
)HLSL";
}
