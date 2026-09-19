// Lab 2 geometry → G-buffer (albedo / normal / world position)
// Lab 1: UV tiling/scroll on sponza_320
// Lab 8: RT3 ORM (ao, roughness, metallic)

cbuffer FrameConstants : register(b0)
{
    float4x4 worldViewProj;
};

cbuffer MaterialConstants : register(b1)
{
    float2 uvScale;
    float2 uvOffset;
    float roughness;
    float metallic;
    float ao;
    float _padMat;
};

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
    // Sponza is already in world space (identity world), same as PCG wvp path.
    output.positionH = mul(float4(input.position, 1.0f), worldViewProj);
    output.positionW = input.position;
    output.normalW = input.normal;
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
