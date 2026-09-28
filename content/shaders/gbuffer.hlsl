// Lab 2 geometry → G-buffer (albedo / normal / world position)
// Lab 1: UV tiling/scroll on sponza_320
// Lab 8: RT3 ORM (ao, roughness, metallic)

cbuffer FrameConstants : register(b0)
{
    float4x4 worldViewProj;
    float timeSeconds;
    float3 _padTime;
};

cbuffer MaterialConstants : register(b1)
{
    float2 uvScale;
    float2 uvOffset;
    float roughness;
    float metallic;
    float ao;
    float vertexAnimEnable;
    float vertexAnimPivotY;
    float vertexAnimAmp;
    float vertexAnimSpeed;
    float _padAnim;
};

// Vertex animation for the sponza_01 flowerbed: squash/stretch along Y around the base.
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

float3 ApplyFlowerbedNormalAnim(float3 normal, float enable, float time, float amp, float speed)
{
    if (enable < 0.5f)
    {
        return normal;
    }
    const float scaleY = 1.0f + amp * sin(time * speed);
    normal.y *= rcp(max(scaleY, 0.2f));
    return normalize(normal);
}

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct PSInput
{
    float4 positionH : SV_POSITION;
    float3 positionW : TEXCOORD0;
    float3 normalW : NORMAL;
    float2 uv : TEXCOORD1;
};

struct PSOutput
{
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
    float4 position : SV_Target2;
    float4 material : SV_Target3;
};

Texture2D diffuseMap : register(t0);
SamplerState linearSampler : register(s0);

PSInput VSMain(VSInput input)
{
    PSInput output;
    // Sponza is already in world space (identity world matrix).
    const float3 posW = ApplyFlowerbedVertexAnim(
        input.position, vertexAnimEnable, vertexAnimPivotY, timeSeconds, vertexAnimAmp, vertexAnimSpeed);
    output.positionH = mul(float4(posW, 1.0f), worldViewProj);
    output.positionW = posW;
    output.normalW = ApplyFlowerbedNormalAnim(
        input.normal, vertexAnimEnable, timeSeconds, vertexAnimAmp, vertexAnimSpeed);
    output.uv = input.uv * uvScale + uvOffset;
    return output;
}

PSOutput PSMain(PSInput input)
{
    float4 albedo = diffuseMap.Sample(linearSampler, input.uv);
    if (albedo.a < 0.2f)
    {
        discard;
    }

    PSOutput output;
    output.albedo = float4(albedo.rgb, 1.0f);
    output.normal = float4(normalize(input.normalW), 0.0f);
    output.position = float4(input.positionW, 1.0f);
    output.material = float4(saturate(ao), saturate(roughness), saturate(metallic), 1.0f);
    return output;
}
