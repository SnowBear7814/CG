// Lab 5: depth-only + optional alpha-test for cutout materials
cbuffer ShadowCB : register(b0)
{
    float4x4 gWorldLightViewProj;
    float gAlphaTestEnable;
    float gAlphaTestCutoff;
    float2 _pad;
};

Texture2D gDiffuseMap : register(t0);
SamplerState gSamLinearWrap : register(s0);

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct VSOutput
{
    float4 positionH : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput o;
    o.positionH = mul(float4(input.position, 1.0f), gWorldLightViewProj);
    o.uv = input.uv;
    return o;
}

void PSMain(VSOutput input)
{
    if (gAlphaTestEnable > 0.5f)
    {
        float4 tex = gDiffuseMap.Sample(gSamLinearWrap, input.uv);
        float mask = max(tex.a, tex.r);
        clip(mask - gAlphaTestCutoff);
    }
}
