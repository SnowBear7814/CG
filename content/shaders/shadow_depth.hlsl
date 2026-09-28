// Lab 5: depth-only + optional alpha-test for cutout materials
cbuffer ShadowCB : register(b0)
{
    float4x4 gWorldLightViewProj;
    float gAlphaTestEnable;
    float gAlphaTestCutoff;
    float gVertexAnimEnable;
    float gVertexAnimPivotY;
    float gVertexAnimTime;
    float gVertexAnimAmp;
    float gVertexAnimSpeed;
    float _padAnim;
};

float3 ApplyFlowerbedVertexAnim(float3 pos, float enable, float pivotY, float time, float amp, float speed)
{
    if (enable < 0.5f)
    {
        return pos;
    }
    const float scaleY = 1.0f + amp * sin(time * speed);
    pos.y = pivotY + (pos.y - pivotY) * scaleY;
    return pos;
}

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
    const float3 pos = ApplyFlowerbedVertexAnim(
        input.position,
        gVertexAnimEnable,
        gVertexAnimPivotY,
        gVertexAnimTime,
        gVertexAnimAmp,
        gVertexAnimSpeed);
    o.positionH = mul(float4(pos, 1.0f), gWorldLightViewProj);
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
