#pragma once

// Approximate measured sequential NR with model-specific RGB band coefficients.
static const char kFastMultiPassHlsl[] = R"hlsl(
Texture2D<float4> gOriginal : register(t0);
Texture2D<float4> gNr : register(t1);
Texture2D<float2> gMotion : register(t2);
RWTexture2D<float4> gDst : register(u0);
cbuffer PC : register(b0)
{
    uint gW; uint gH; float gReserved0; float gLimit;
    uint gMvW; uint gMvH;
    uint gReserved1; float gReserved2;
    float4 gRefFine; float4 gRefStructure; float4 gRefLow;
    float4 gRefNrFine; float4 gRefNrStructure; float4 gRefBias;
};

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gW || id.y >= gH) return;
    int2 p = int2(id.xy), hi = int2(gW - 1, gH - 1);
    float4 nr = gNr[p];
    float3 residual = nr.rgb - gOriginal[p].rgb;
    float3 low = 0.0f, fineLow = 0.0f, nrFineLow = 0.0f, nrLow = 0.0f;
    // A fine 3x3 band and a sparse 9x9 band include texture and small NR
    // structures. A one-pixel band discarded most real-model residuals,
    // especially the broad changes on a still, stylized game picture.
    [unroll] for (int fy = -1; fy <= 1; ++fy)
    [unroll] for (int fx = -1; fx <= 1; ++fx)
    {
        int2 q = clamp(p + int2(fx, fy), int2(0, 0), hi);
        float weight = float((fx == 0 ? 2 : 1) * (fy == 0 ? 2 : 1));
        float3 src = gOriginal[q].rgb, n = gNr[q].rgb;
        fineLow += (n - src) * (weight / 16.0f);
        nrFineLow += n * (weight / 16.0f);
    }
    static const float weights[5] = { 1.0f, 4.0f, 6.0f, 4.0f, 1.0f };
    [unroll] for (int y = -2; y <= 2; ++y)
    [unroll] for (int x = -2; x <= 2; ++x)
    {
        int2 q = clamp(p + int2(x, y) * 2, int2(0, 0), hi);
        float3 src = gOriginal[q].rgb;
        float3 n = gNr[q].rgb;
        low += (n - src) * (weights[x + 2] * weights[y + 2] / 256.0f);
        nrLow += n * (weights[x + 2] * weights[y + 2] / 256.0f);
    }
    float3 fine = residual - fineLow;
    float3 structure = fineLow - low;
    float motionGate = 1.0f;
    if (gMvW > 0 && gMvH > 0)
    {
        int2 q = clamp(int2((float2(p) + 0.5f) * float2(gMvW, gMvH) / float2(gW, gH)),
                       int2(0, 0), int2(gMvW - 1, gMvH - 1));
        // Work-resolution pixels per frame, consistent across NR resolutions.
        float2 mv = gMotion[q];
        // With full stabilization, source-color correspondence validates the
        // motion. Do not pump the effect gain when camera-flow speed fluctuates.
        motionGate = all(isfinite(mv)) ? (gRefFine.w > .5f ? 1.0f :
            1.0f - smoothstep(2.0f, 24.0f, length(mv))) : 0.0f;
    }
    // Signed RGB coefficients fitted to actual sequential NR output.
    float3 matched = gRefFine.rgb*fine + gRefStructure.rgb*structure +
        gRefLow.rgb*low + gRefNrFine.rgb*(nr.rgb-nrFineLow) +
        gRefNrStructure.rgb*(nrFineLow-nrLow) + gRefBias.rgb;
    float3 boost = clamp(matched*motionGate, -gLimit, gLimit);
    // Leave a quarter of the available headroom to avoid forcing shadows or
    // highlights into clipping when stronger lighting extrapolation is used.
    gDst[p] = float4(nr.rgb + clamp(boost, -0.75f * nr.rgb, 0.75f * (1.0f - nr.rgb)), nr.a);
}
)hlsl";
