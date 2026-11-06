// SPDX-License-Identifier: MIT
#pragma once

namespace LayerReconstruction
{
// All source samples belong to one array slice. Alpha is opacity; RGB is premultiplied.
inline constexpr unsigned Count = 9;
inline constexpr unsigned PackedSlices = 6;
inline constexpr const char *Shader = R"HLSL(
Texture2DArray<float4> beforeLayers:register(t0);
Texture2DArray<float4> afterLayers:register(t1);
Texture2DArray<float2> vectors:register(t2);
SamplerState vectorSampler:register(s0);
Texture2D<float4> currentWorld:register(t3);
Texture2D<float4> previousWorld:register(t4);
Texture2D<uint> validity:register(t5);
RWTexture2D<float4> outputImage:register(u0);
RWStructuredBuffer<uint> counters:register(u1);
RWTexture2DArray<float4> reconstructed:register(u2);
RWTexture2D<uint> validityOutput:register(u3);
cbuffer Parameters:register(b0)
{
    uint2 size;
    uint2 grid;
    float phase;
    float flowScale;
    uint cloudFilmic;
    uint writeLayers;
    float cloudFadeBefore;
    float cloudFadeAfter;
    uint verifyComposition;
    uint motionMask;
}
float maximum(float3 c){return max(c.r,max(c.g,c.b));}
bool protectedColor(float3 c){return maximum(c)<.02 || (maximum(c)>.93 && min(c.r,min(c.g,c.b))>.8);}
float3 inverseFilm(float3 color)
{
    float3 x=saturate(color);
    float3 a=.0433513559*x-.053199999;
    float3 b=.114082515*x-.00500000082;
    return (-sqrt(max(0,b*b-.0547596104*x*a))-.114082515*x+.00500000082)/((.22816503*x-.280000001)*.379999995);
}
float3 film(float3 x)
{
    return ((x*(.189999998*x+.0500000007)+.00400000019)/(x*(.189999998*x+.5)+.0600000024)-.0666666627)*1.31483781;
}
float4 loadLayer(int2 p,uint layer,bool after)
{
    if(any(p<0)||any(p>=int2(size)))return 0;
    if(layer<6)
    {
        uint kind=uint(round((after?afterLayers.Load(int4(p,1,0)):beforeLayers.Load(int4(p,1,0))).r*255));
        uint owner=kind==1?5:kind==2?1:kind==3?2:kind==4?3:kind==5?4:0;
        if(owner!=layer)return 0;
        float3 color=(after?afterLayers.Load(int4(p,0,0)):beforeLayers.Load(int4(p,0,0))).rgb;
        return float4(color,1);
    }
    return after?afterLayers.Load(int4(p,layer-4,0)):beforeLayers.Load(int4(p,layer-4,0));
}
float4 sampleLayer(float2 p,uint layer,bool after)
{
    int2 q=int2(floor(p));float2 f=frac(p);
    return lerp(lerp(loadLayer(q,layer,after),loadLayer(q+int2(1,0),layer,after),f.x),
                lerp(loadLayer(q+int2(0,1),layer,after),loadLayer(q+1,layer,after),f.x),f.y);
}
float3 sampleTransmission(float2 p,bool after)
{
    int2 q=int2(floor(p));float2 f=frac(p);
    int2 a=clamp(q,0,int2(size)-1),b=clamp(q+1,0,int2(size)-1);
    float3 c00=(after?afterLayers.Load(int4(a,5,0)):beforeLayers.Load(int4(a,5,0))).rgb;
    float3 c10=(after?afterLayers.Load(int4(b.x,a.y,5,0)):beforeLayers.Load(int4(b.x,a.y,5,0))).rgb;
    float3 c01=(after?afterLayers.Load(int4(a.x,b.y,5,0)):beforeLayers.Load(int4(a.x,b.y,5,0))).rgb;
    float3 c11=(after?afterLayers.Load(int4(b,5,0)):beforeLayers.Load(int4(b,5,0))).rgb;
    return lerp(lerp(c00,c10,f.x),lerp(c01,c11,f.x),f.y);
}
float2 flowAt(float2 p,uint layer,bool backward)
{
    float2 q=clamp((p/flowScale+.5)/4-.5,0,float2(grid)-1);
    uint slice=layer*2+(backward?1:0);
    float2 v=vectors.SampleLevel(vectorSampler,float3((q+.5)/float2(grid),slice),0);
    return v*(flowScale/32.0);
}
bool hasData(float4 value,uint layer)
{
    return value.a>.01 || (layer==7 && maximum(abs(value.rgb))>.0001);
}
float3 feature(float4 value,uint layer)
{
    float3 c=layer==7?value.rgb:value.rgb/max(value.a,.01);
    return c/(1+max(c,0));
}
float4 predict(float2 centre,uint layer,out bool valid)
{
    float2 p=centre,q=centre;
    p=centre-phase*flowAt(centre,layer,false);q=centre-(1-phase)*flowAt(centre,layer,true);
    float2 f=flowAt(p,layer,false),b=flowAt(q,layer,true);
    float4 a=sampleLayer(p,layer,false),z=sampleLayer(q,layer,true);
    float error=max(length(f+flowAt(p+f,layer,true)),length(b+flowAt(q+b,layer,false)));
    float photo=maximum(abs(feature(a,layer)-feature(z,layer)));
    bool outside=any(p<0)||any(q<0)||any(p>float2(size)-1)||any(q>float2(size)-1);
    valid=!outside && hasData(a,layer) && hasData(z,layer) && error<=4 && photo<=.4
        && all(isfinite(float4(f,b))) && all(isfinite(a)) && all(isfinite(z));
    // AI-generated comment: No comparison with the unwarped current color is made here.
    // A moving edge is allowed to replace a different endpoint color.
    return valid?lerp(a,z,phase):loadLayer(int2(centre),layer,true);
}
float3 compose(float4 layers[8],float fade)
{
    float3 color=0;
    [unroll]for(uint layer=0;layer<6;layer++)color=layers[layer].rgb+(1-saturate(layers[layer].a))*color;
    float4 cloud=layers[6];
    if(cloudFilmic!=0)
    {
        float3 combined=film(cloud.rgb+(1-saturate(cloud.a))*inverseFilm(color));
        color=lerp(color,combined,saturate(fade));
    }
    else color=cloud.rgb+(1-saturate(cloud.a))*color;
    return layers[7].rgb+(1-saturate(layers[7].a))*color;
}
float3 compositeCloud(float3 base,float4 cloud,float fade)
{
    return cloudFilmic!=0?lerp(base,film(cloud.rgb+(1-saturate(cloud.a))*inverseFilm(base)),saturate(fade))
        :cloud.rgb+(1-saturate(cloud.a))*base;
}
float3 sourceColor(int2 p,bool after)
{
    float3 base=(after?afterLayers.Load(int4(p,0,0)):beforeLayers.Load(int4(p,0,0))).rgb;
    float4 cloud=after?afterLayers.Load(int4(p,2,0)):beforeLayers.Load(int4(p,2,0));
    float4 fire=after?afterLayers.Load(int4(p,3,0)):beforeLayers.Load(int4(p,3,0));
    base=compositeCloud(base,cloud,after?cloudFadeAfter:cloudFadeBefore);
    base=fire.rgb+(1-saturate(fire.a))*base;
    float4 atmosphere=after?afterLayers.Load(int4(p,4,0)):beforeLayers.Load(int4(p,4,0));
    float3 transmission=(after?afterLayers.Load(int4(p,5,0)):beforeLayers.Load(int4(p,5,0))).rgb;
    return atmosphere.a>.01?film(inverseFilm(base)*transmission+atmosphere.rgb):base;
}
bool protectedAt(int2 pixel,float3 color)
{
    bool result=protectedColor(color);
    [unroll]for(int axis=0;axis<4;axis++)
    {
        int2 delta=axis==0?int2(1,0):axis==1?int2(-1,0):axis==2?int2(0,1):int2(0,-1);
        result=result||protectedColor(currentWorld.Load(int3(clamp(pixel+delta,0,int2(size)-1),0)).rgb);
    }
    return result;
}
#ifdef KOE_LAYER_VALIDATE
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID)
{
    if(any(id.xy>=size))return;
    int2 pixel=int2(id.xy);float3 real=currentWorld.Load(int3(pixel,0)).rgb;
    uint flags=protectedAt(pixel,real)?2:0;
    if(flags==0)
    {
        float3 prev=previousWorld.Load(int3(pixel,0)).rgb;
        if(maximum(abs(saturate(sourceColor(pixel,false))-prev))<=1.5/255
            && maximum(abs(saturate(sourceColor(pixel,true))-real))<=1.5/255)flags=1;
    }
    validityOutput[id.xy]=flags;
}
#else
groupshared uint totals[6];
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID,uint lane:SV_GroupIndex)
{
    if(lane<6)totals[lane]=0;
    GroupMemoryBarrierWithGroupSync();
    if(all(id.xy<size))
    {
    int2 pixel=int2(id.xy);float4 real=currentWorld.Load(int3(pixel,0));
    InterlockedAdd(totals[3],1);
    InterlockedAdd(totals[0],1);
    uint flags=verifyComposition!=0?validity.Load(int3(pixel,0)):1;
    bool exact=flags==1;
    bool protectedPixel=verifyComposition!=0?flags==2:protectedAt(pixel,real.rgb);
    float4 result=real;
    if((exact && !protectedPixel)||writeLayers!=0)
    {
        float3 color=0;bool anyValid=false;
        [unroll]for(uint layer=0;layer<8;layer++)
        {
            float4 old=loadLayer(pixel,layer,false),now=loadLayer(pixel,layer,true),value=now;bool valid=false;
            if((motionMask&(1u<<layer))!=0 && (hasData(old,layer)||hasData(now,layer)))value=predict(float2(pixel),layer,valid);
            anyValid=anyValid||valid;
            if(writeLayers!=0)reconstructed[uint3(id.xy,layer)]=value;
            if(layer==6)color=compositeCloud(color,value,lerp(cloudFadeBefore,cloudFadeAfter,phase));
            else color=value.rgb+(1-saturate(value.a))*color;
        }
        float4 atmosphere=loadLayer(pixel,8,true);bool atmosphereValid=false;
        if((motionMask&(1u<<8))!=0 && (hasData(loadLayer(pixel,8,false),8)||hasData(atmosphere,8)))
            atmosphere=predict(float2(pixel),8,atmosphereValid);
        float3 transmission=afterLayers.Load(int4(pixel,5,0)).rgb;
        if(atmosphereValid)
        {
            float2 p=float2(pixel)-phase*flowAt(float2(pixel),8,false);
            float2 q=float2(pixel)-(1-phase)*flowAt(float2(pixel),8,true);
            transmission=lerp(sampleTransmission(p,false),sampleTransmission(q,true),phase);
        }
        if(writeLayers!=0)reconstructed[uint3(id.xy,8)]=atmosphere;
        if(atmosphere.a>.01)color=film(inverseFilm(color)*transmission+atmosphere.rgb);
        anyValid=anyValid||atmosphereValid;
        if(exact && anyValid && !protectedPixel && all(isfinite(color)))
        {result.rgb=saturate(color);InterlockedAdd(totals[4],1);}
    }
    else if(!exact && !protectedPixel)InterlockedAdd(totals[1],1);
    if(any(round(result.rgb*255)!=round(real.rgb*255)))InterlockedAdd(totals[5],1);
    outputImage[id.xy]=result;
    }
    GroupMemoryBarrierWithGroupSync();
    if(lane<6)InterlockedAdd(counters[lane],totals[lane]);
}
#endif
)HLSL";
}
