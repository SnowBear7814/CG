cbuffer FrameConstants : register(b0)
{
    float4x4 mvp;
    float3 sunDirection;
    float padding;
};

cbuffer UvConstants : register(b1)
{
    float2 uvScale;
    float2 uvOffset;
};

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

Texture2D diffuseMap : register(t0);
SamplerState linearSampler : register(s0);

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.position = mul(float4(input.position, 1.0f), mvp);
    output.normal = input.normal;
    // Lab 1: tiling + scroll (WRAP sampler)
    output.uv = input.uv * uvScale + uvOffset;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float4 albedo = diffuseMap.Sample(linearSampler, input.uv);
    if (albedo.a < 0.2f)
    {
        discard;
    }

    float3 n = normalize(input.normal);
    float ndotl = saturate(dot(n, normalize(-sunDirection)));
    float3 lit = albedo.rgb * (0.25f + 0.75f * ndotl);
    return float4(lit, 1.0f);
}
