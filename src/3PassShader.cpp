#include "3PassShader.h"
#include "Log.h"

#include <cstring>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

namespace {

static bool CompileHlsl(const char* hlsl, const char* entry, const char* target, ID3DBlob** outBlob) {
    if (!outBlob) return false;
    *outBlob = nullptr;

    // IMPORTANT: Keep shader compilation flags identical across Debug/Release
    // so shader behavior matches between configurations.
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;

    ID3DBlob* shader = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(
        hlsl,
        std::strlen(hlsl),
        nullptr,
        nullptr,
        nullptr,
        entry,
        target,
        flags,
        0,
        &shader,
        &errors);

    if (FAILED(hr) || !shader) {
        if (errors) {
            std::string err((const char*)errors->GetBufferPointer(), errors->GetBufferSize());
            Log::Error("3PassShader D3DCompile failed: " + err);
            errors->Release();
        } else {
            Log::Error("3PassShader D3DCompile failed");
        }
        if (shader) shader->Release();
        return false;
    }

    if (errors) errors->Release();
    *outBlob = shader;
    return true;
}

// This file contains a 3-pass compute pipeline (DepthRaw, DepthSmooth, ParallaxSbs).
// Renderer binds and dispatches all three passes.
static const char* kThreePassHlsl = R"HLSL(
// ------------------------------------------------------------
// Common resources
// ------------------------------------------------------------
Texture2D srcTex              : register(t0);
Texture2D<float> depthRawTex  : register(t1); // NOTE: renderer binds *smoothed depth* here for pass 3
Texture2D<float> depthPrevTex : register(t2);
SamplerState samp0            : register(s0);

RWTexture2D<float>      depthRawOut    : register(u0);
RWTexture2D<float>      depthPrevOut   : register(u1);
RWTexture2D<float>      depthSmoothOut : register(u2);
RWTexture2D<float4>     outImage       : register(u3);

cbuffer CSParams : register(b0)
{
    uint  outWidth;
    uint  outHeight;
    uint  mode3d;      // 0 = fullscreen, 1 = OU, 2 = SBS
    int   zoomLevel;

    float parallaxPx;
    float frame;
    float2 pad0;

    float2 cropOffset;
    float2 cropScale;
};

// ------------------------------------------------------------
// Helpers
// ------------------------------------------------------------
float Luma(float3 c)
{
    // Match historical DepthStereoShader coefficients.
    return dot(c, float3(0.299, 0.587, 0.114));
}

void EyeMapping(uint2 gid, out bool rightEye, out uint localX, out uint viewW, out float2 uvEye)
{
    const uint leftW  = outWidth / 2;
    const uint rightW = outWidth - leftW;

    rightEye = (gid.x >= leftW);
    viewW    = rightEye ? rightW : leftW;
    localX   = rightEye ? (gid.x - leftW) : gid.x;

    float u = (float(localX) + 0.5) / float(max(1u, viewW));
    float v = (float(gid.y) + 0.5) / float(max(1u, outHeight));
    uvEye   = float2(u, v);
}

// ------------------------------------------------------------
// PASS 1: Depth-from-luma
// ------------------------------------------------------------
[numthreads(16, 16, 1)]
void CSDepthRaw(uint3 tid : SV_DispatchThreadID)
{
    uint2 gid = tid.xy;
    if (gid.x >= outWidth || gid.y >= outHeight) return;

    bool  rightEye;
    uint  localX;
    uint  viewW;
    float2 uvEye;
    EyeMapping(gid, rightEye, localX, viewW, uvEye);

    float2 uv0 = cropOffset + uvEye * cropScale;

    // Neighbor offsets in *output pixel* space mapped into UV.
    float2 uvStep = float2(cropScale.x / float(max(1u, viewW)), cropScale.y / float(max(1u, outHeight)));

    float gC = Luma(srcTex.SampleLevel(samp0, uv0, 0).rgb);
    float gL = Luma(srcTex.SampleLevel(samp0, uv0 + float2(-uvStep.x, 0), 0).rgb);
    float gR = Luma(srcTex.SampleLevel(samp0, uv0 + float2(+uvStep.x, 0), 0).rgb);
    float gU = Luma(srcTex.SampleLevel(samp0, uv0 + float2(0, -uvStep.y), 0).rgb);
    float gD = Luma(srcTex.SampleLevel(samp0, uv0 + float2(0, +uvStep.y), 0).rgb);

    float c = max(max(abs(gC - gL), abs(gC - gR)), max(abs(gC - gU), abs(gC - gD)));

    // === STRUCTURE PROTECTION MASK ===
    float structure = smoothstep(0.12, 0.35, c);
    float depth_aggression = lerp(0.55, 0.35, structure);

    // 5-tap cross smoothing
    float g_avg = (gC + gL + gR + gU + gD) * 0.2;

    float w = 1.0 - smoothstep(0.05, 0.25, c);
    float g_soft = lerp(gC, g_avg, w);

    // stronger edge softening
    float soften = smoothstep(0.10, 0.35, c);
    float g_edge_avg = (gC + gL + gR + gU + gD) * 0.2;
    g_soft = lerp(g_soft, g_edge_avg, soften * 0.90);

    // toroidal grayscale field
    float mid_gray = 0.5;
    float dist = abs(g_soft - mid_gray);
    float torus = 1.0 - smoothstep(0.0, 0.025, dist);
    g_soft = lerp(g_soft, mid_gray, torus * 0.50);

    // specular light pop
    float highlight = smoothstep(0.78, 0.95, g_soft);
    float contrast = smoothstep(0.12, 0.32, c);
    float spec_pop = highlight * contrast;
    g_soft = lerp(g_soft, g_soft * 0.92, spec_pop * 0.15);

    // === CURVE LAYERS (5-layer depth) ===
    float dark_push = pow(1.0 - g_soft, 1.15;

    float t = saturate(g_soft);

    float b_near1 = pow(t, 0.22);
    float b_near2 = pow(t, 0.50);
    float b_mid = pow(t, 0.90);
    float b_far1 = pow(1.0 - t, 0.95);
    float b_far2 = pow(1.0 - t, 1.35);

    float w_near1 = smoothstep(0.00, 0.40, t);
    float w_near2 = smoothstep(0.10, 0.60, t);
    float w_mid = smoothstep(0.20, 0.80, t);
    float w_far1 = smoothstep(0.15, 0.85, 1.0 - t);
    float w_far2 = smoothstep(0.35, 1.00, 1.0 - t);

    float w_sum = w_near1 + w_near2 + w_mid + w_far1 + w_far2 + 1e-6;

    float d_near1 = b_near1;
    float d_near2 = b_near2;
    float d_mid = b_mid;
    float d_far1 = 1.0 - b_far1;
    float d_far2 = 1.0 - b_far2;

    float curve_blend = (
        d_near1 * w_near1 +
        d_near2 * w_near2 +
        d_mid * w_mid +
        d_far1 * w_far1 +
        d_far2 * w_far2
    ) / w_sum;

    // === DEPTH SHAPING USING BLENDED CURVE ===
    float depth = lerp(curve_blend, dark_push, 1.00 - g_soft);

    depth = (depth - 0.5) * depth_aggression + 0.5;
    depth = lerp(depth, curve_blend, 0.018);
    depth = (depth - 0.5) * depth_aggression + 0.5;

    // ripple reduction
    float lc_soft = smoothstep(0.030, 0.004, c);
    depth += lc_soft * 0.0000010;

    // bright noise dampening
    float noise_energy = c * g_soft;
    float noise_mask = smoothstep(0.35, 0.75, noise_energy);
    depth = lerp(depth, depth * 0.20, noise_mask * 0.18);

    // Optional global depth boost (can help with very flat/indoor scenes)
    float depth_mid = 0.5;
    float boost = 1.18;   // strong, safe expansion factor
    depth = (depth - depth_mid) * boost + depth_mid;


    depthRawOut[gid] = saturate(depth);

}

// ------------------------------------------------------------
// PASS 2: Temporal + spatial smoothing
// ------------------------------------------------------------
[numthreads(16, 16, 1)]
void CSDepthSmooth(uint3 tid : SV_DispatchThreadID)
{
    uint2 gid = tid.xy;
    if (gid.x >= outWidth || gid.y >= outHeight) return;

    float depth = depthRawTex.Load(int3(gid, 0));
    depth = saturate(depth);

    float d = pow(depth, 0.65);
    float mid = 0.5;
    d = (d - mid) * 2.20 + mid;
    d = saturate(d);
    d = lerp(d, d * d * (3.0 - 2.0 * d), 0.20);
    depth = d;

    float prev = depthPrevTex.Load(int3(gid, 0));
    float prev = depthPrevTex.Load(int3(gid, 0));

    // === TEMPORAL MICRO‑CLAMP (restores text/UI stability) ===
    float blended = lerp(prev, depth, 0.28);   // EMA

    float maxDelta = 0.05; // per‑frame clamp
    float delta = blended - prev;
    delta = clamp(delta, -maxDelta, maxDelta);

    float temporal = prev + delta; // clamped temporal depth

    int2 p = int2(gid);
    int2 pU = int2(p.x, max(0, p.y - 1));
    int2 pD = int2(p.x, min(int(outHeight) - 1, p.y + 1));
    int2 pL = int2(max(0, p.x - 1), p.y);
    int2 pR = int2(min(int(outWidth) - 1, p.x + 1), p.y);

    float v1 = depthPrevTex.Load(int3(pD, 0));
    float v2 = depthPrevTex.Load(int3(pU, 0));

    float vert = temporal * 0.50 + (v1 + v2) * 0.25;

    float h1 = depthPrevTex.Load(int3(pR, 0));
    float h2 = depthPrevTex.Load(int3(pL, 0));

    float horiz = vert * 0.95 + (h1 + h2) * 0.025;
    float final_depth = lerp(vert, horiz, 0.05);

    depthPrevOut[gid] = final_depth;
    depthSmoothOut[gid] = final_depth;
}

// ------------------------------------------------------------
// PASS 3: Parallax SBS using smoothed depth
// ------------------------------------------------------------
[numthreads(16, 16, 1)]
void CSParallaxSbs(uint3 tid : SV_DispatchThreadID)
{
    uint2 gid = tid.xy;
    if (gid.x >= outWidth || gid.y >= outHeight) return;

    bool  rightEye;
    uint  localX;
    uint  viewW;
    float2 uvEye;
    EyeMapping(gid, rightEye, localX, viewW, uvEye);

    // Use depth bound at t1 (renderer binds smoothed depth SRV to slot 1).
    // Address text jitter float depth = depthRawTex.Load(int3(gid, 0));
    float depth = depthSmoothTex.Load(int3(gid, 0));

    depth = saturate(depth);

    // Simple shaping; you can tweak exponent/scale
    float shaped = pow(depth, 0.72);
    float shift = parallaxPx * shaped;

    // Optional clamp for zoom-out (mirrors your original behaviour)
    if (zoomLevel < 0)
    {
        float maxShift = float(max(1u, viewW)) * 0.10;
        shift = clamp(shift, -maxShift, +maxShift);
    }

    float sbsShift = rightEye ? -shift : shift;
    float shiftedRaw = float(localX) + sbsShift;

    bool hitEdge = (shiftedRaw < 0.0) || (shiftedRaw > float(max(1u, viewW) - 1));
    if (hitEdge)
    {
        outImage[gid] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float u = (shiftedRaw + 0.5) / float(max(1u, viewW));
    float v = (float(gid.y) + 0.5) / float(max(1u, outHeight));

    float2 uv0 = cropOffset + float2(u, v) * cropScale;
    float4 c = srcTex.SampleLevel(samp0, uv0, 0);
    outImage[gid] = c;
}
)HLSL";

} // namespace

namespace ThreePassShader {

bool CompileDepthRawCS(ID3DBlob** outCsBlob) {
    return CompileHlsl(kThreePassHlsl, "CSDepthRaw", "cs_5_0", outCsBlob);
}

bool CompileDepthSmoothCS(ID3DBlob** outCsBlob) {
    return CompileHlsl(kThreePassHlsl, "CSDepthSmooth", "cs_5_0", outCsBlob);
}

bool CompileParallaxSbsCS(ID3DBlob** outCsBlob) {
    return CompileHlsl(kThreePassHlsl, "CSParallaxSbs", "cs_5_0", outCsBlob);
}

}
