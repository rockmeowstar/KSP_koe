// SPDX-License-Identifier: MIT
#pragma once

namespace SceneContours
{
// AI-generated comment: The two low bits describe links to the right and below.
// A missing link is a boundary, not an object classification inferred from RGB.
inline constexpr const char *Shader = R"HLSL(
Texture2D<float4> image0 : register(t0);
Texture2D<float4> image1 : register(t1);
Texture2D<float4> layers0 : register(t2);
Texture2D<float4> layers1 : register(t3);
Texture2D<int2> flow0 : register(t4);
Texture2D<int2> flow1 : register(t5);
RWTexture2D<uint> links0 : register(u0);
RWTexture2D<uint> links1 : register(u1);
cbuffer Dimensions : register(b0)
{
    float2 size;
    float2 gridSize;
    float phase;
    uint hasLayers;
    uint unused0;
    uint unused1;
}
groupshared float4 colors[200];
groupshared uint labels[200];
uint label(float4 value)
{
    uint4 bytes = uint4(round(saturate(value) * 255));
    return bytes.x | (bytes.y << 8) | (bytes.z << 16) | (bytes.w << 24);
}
uint link(uint center, uint neighbor, int2 a, int2 b, uint frame)
{
    if (labels[center] != labels[neighbor]) return 0;
    float3 delta = abs(colors[center].rgb - colors[neighbor].rgb);
    if (max(delta.r, max(delta.g, delta.b)) > 12.0 / 255) return 0;
    int2 ga = clamp(a / 4, 0, int2(gridSize) - 1);
    int2 gb = clamp(b / 4, 0, int2(gridSize) - 1);
    if (any(ga != gb))
    {
        float2 va = frame ? flow1.Load(int3(ga, 0)) : flow0.Load(int3(ga, 0));
        float2 vb = frame ? flow1.Load(int3(gb, 0)) : flow0.Load(int3(gb, 0));
        if (dot(va - vb, va - vb) > 32 * 32) return 0;
    }
    return 1;
}
[numthreads(8, 8, 1)] void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    int2 origin = int2(group.xy * 8);
    for (uint i = lane; i < 100; i += 64)
    {
        int2 p = clamp(origin + int2(i % 10, i / 10) - 1, 0, int2(size) - 1);
        colors[i] = image0.Load(int3(p, 0));
        colors[i + 100] = image1.Load(int3(p, 0));
        labels[i] = hasLayers ? label(layers0.Load(int3(p, 0))) : 0;
        labels[i + 100] = hasLayers ? label(layers1.Load(int3(p, 0))) : 0;
    }
    GroupMemoryBarrierWithGroupSync();
    int2 p = origin + int2(lane % 8, lane / 8);
    if (any(p >= int2(size))) return;
    uint center = (lane / 8 + 1) * 10 + lane % 8 + 1;
    uint result[2];
    [unroll] for (uint frame = 0; frame < 2; ++frame)
    {
        uint c = center + frame * 100;
        result[frame] = link(c, c + 1, p, p + int2(1, 0), frame)
                      | (link(c, c + 10, p, p + int2(0, 1), frame) << 1);
    }
    links0[p] = result[0];
    links1[p] = result[1];
}
)HLSL";
}
