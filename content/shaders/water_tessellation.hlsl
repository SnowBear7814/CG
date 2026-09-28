// Lab 3 bonus: tessellated water plane — single sine displacement

cbuffer WaterCB : register(b0)
{
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
    float4x4 gWorldViewProj;

    float3 gEyePosW;
    float gTime;

    float gMinTess;
    float gMaxTess;
    float gTessNear;
    float gTessFar;

    float gWaveAmp;
    float gWaveFreq;
    float gWaveSpeed;
    float _pad;
};

struct VSInput
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 Tex : TEXCOORD0;
    float4 TangentL : TANGENT;
};

struct HsControlPoint
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 Tex : TEXCOORD0;
};

struct HS_CONSTANTS
{
    float Edge[3] : SV_TessFactor;
    float Inside : SV_InsideTessFactor;
};

struct DSOutput
{
    float4 PosH : SV_POSITION;
    float3 PosW : TEXCOORD0;
    float3 NormalW : TEXCOORD1;
    float2 Tex : TEXCOORD2;
};

HsControlPoint VSMain(VSInput vin)
{
    HsControlPoint o;
    o.PosL = vin.PosL;
    o.NormalL = vin.NormalL;
    o.Tex = vin.Tex;
    return o;
}

float TessFactorFromWorldPos(float3 posW)
{
    float d = distance(posW, gEyePosW);
    float t = saturate((gTessFar - d) / max(gTessFar - gTessNear, 0.001f));
    return lerp(gMinTess, gMaxTess, t);
}

HS_CONSTANTS PatchConstantHS(InputPatch<HsControlPoint, 3> patch)
{
    HS_CONSTANTS hs;
    float3 c0 = mul(float4(patch[0].PosL, 1.f), gWorld).xyz;
    float3 c1 = mul(float4(patch[1].PosL, 1.f), gWorld).xyz;
    float3 c2 = mul(float4(patch[2].PosL, 1.f), gWorld).xyz;
    float tess = TessFactorFromWorldPos((c0 + c1 + c2) * (1.0f / 3.0f));
    hs.Edge[0] = tess;
    hs.Edge[1] = tess;
    hs.Edge[2] = tess;
    hs.Inside = tess;
    return hs;
}

[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PatchConstantHS")]
[maxtessfactor(64.0)]
HsControlPoint HSMain(InputPatch<HsControlPoint, 3> patch, uint i : SV_OutputControlPointID)
{
    return patch[i];
}

void SampleWaves(float2 xz, out float height, out float dHdx, out float dHdz)
{
    float2 d = float2(1.0f, 0.0f);
    float k = gWaveFreq;
    float a = gWaveAmp;
    float phase = k * dot(d, xz) + gTime * gWaveSpeed;
    float s = sin(phase);
    float c = cos(phase);
    height = a * s;
    dHdx = a * k * d.x * c;
    dHdz = a * k * d.y * c;
}

[domain("tri")]
DSOutput DSMain(
    HS_CONSTANTS hs,
    const OutputPatch<HsControlPoint, 3> patch,
    float3 bary : SV_DomainLocation)
{
    float u = bary.x;
    float v = bary.y;
    float w = bary.z;

    float3 posL = patch[0].PosL * u + patch[1].PosL * v + patch[2].PosL * w;
    float2 tex = patch[0].Tex * u + patch[1].Tex * v + patch[2].Tex * w;

    float h, dHdx, dHdz;
    SampleWaves(posL.xz, h, dHdx, dHdz);
    posL.y += h;

    // Heightfield normal in local space, then to world.
    float3 nL = normalize(float3(-dHdx, 1.0f, -dHdz));

    DSOutput o;
    float4 posW4 = mul(float4(posL, 1.f), gWorld);
    o.PosW = posW4.xyz;
    o.NormalW = normalize(mul(float4(nL, 0.f), gWorldInvTranspose).xyz);
    o.PosH = mul(float4(posL, 1.f), gWorldViewProj);
    o.Tex = tex;
    return o;
}

float4 PSMain(DSOutput pin) : SV_Target
{
    float3 n = normalize(pin.NormalW);
    float3 l = normalize(float3(0.40f, 0.85f, 0.20f));
    float ndotl = saturate(dot(n, l));
    float3 color = float3(0.08f, 0.42f, 0.62f) * (0.40f + 0.60f * ndotl);
    return float4(color, 0.38f);
}
