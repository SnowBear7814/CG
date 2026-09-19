// Lab 7: Depth of Field + Chromatic Aberration (fullscreen post)

Texture2D gSceneColor : register(t0);
Texture2D gSceneDepth : register(t1);
SamplerState gLinearClamp : register(s0);

cbuffer PostCB : register(b0)
{
    float gNearZ;
    float gFarZ;
    float gFocusDistance;
    float gFocusRange;
    float gMaxBlurPixels;
    float gChromaticStrength;
    float gChromaticRadial;
    float gDofEnabled;
    float gCaEnabled;
    float3 gPad;
};

struct VSOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD0;
};

VSOut VS_Post(uint vid : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((vid << 1u) & 2u, vid & 2u);
    o.PosH = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    o.TexC = uv * 0.5f;
    return o;
}

float LinearViewZ(float depth01)
{
    return (gNearZ * gFarZ) / max(gFarZ - depth01 * (gFarZ - gNearZ), 1e-4f);
}

float CircleOfConfusion(float viewZ)
{
    float coc = abs(viewZ - gFocusDistance) / max(gFocusRange, 1.0f);
    return saturate(coc) * gMaxBlurPixels;
}

float3 SampleDof(float2 uv, float2 texSize)
{
    float depth = gSceneDepth.SampleLevel(gLinearClamp, uv, 0).r;
    float viewZ = LinearViewZ(depth);
    float blurRadius = CircleOfConfusion(viewZ);

    float3 center = gSceneColor.SampleLevel(gLinearClamp, uv, 0).rgb;
    if (gDofEnabled < 0.5f || blurRadius < 0.35f)
        return center;

    static const float2 kOffsets[8] = {
        float2( 0.000f,  1.000f),
        float2( 0.707f,  0.707f),
        float2( 1.000f,  0.000f),
        float2( 0.707f, -0.707f),
        float2( 0.000f, -1.000f),
        float2(-0.707f, -0.707f),
        float2(-1.000f,  0.000f),
        float2(-0.707f,  0.707f),
    };

    float2 texel = blurRadius / texSize;
    float3 sum = center;
    float wsum = 1.0f;

    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        float2 sampleUv = uv + kOffsets[i] * texel;
        float sampleDepth = gSceneDepth.SampleLevel(gLinearClamp, sampleUv, 0).r;
        float sampleZ = LinearViewZ(sampleDepth);
        float w = 1.0f - saturate(abs(sampleZ - viewZ) / max(gFocusRange * 0.5f, 1.0f));
        w = 0.35f + 0.65f * w;
        sum += gSceneColor.SampleLevel(gLinearClamp, sampleUv, 0).rgb * w;
        wsum += w;
    }

    return sum / wsum;
}

float4 PS_Post(VSOut pin, float4 posSs : SV_Position) : SV_TARGET
{
    uint w, h, mips;
    gSceneColor.GetDimensions(0, w, h, mips);
    float2 texSize = float2(max(w, 1u), max(h, 1u));
    float2 uv = posSs.xy / texSize;

    // Chromatic aberration samples the DoF result at shifted UVs (not the sharp scene).
    if (gCaEnabled > 0.5f)
    {
        float2 toCenter = uv - 0.5f;
        float radial = pow(length(toCenter) * 2.0f, gChromaticRadial);
        float2 dir = normalize(toCenter + 1e-5f) * (gChromaticStrength * radial);

        float r = SampleDof(uv - dir, texSize).r;
        float g = SampleDof(uv, texSize).g;
        float b = SampleDof(uv + dir, texSize).b;
        return float4(r, g, b, 1.0f);
    }

    return float4(SampleDof(uv, texSize), 1.0f);
}
