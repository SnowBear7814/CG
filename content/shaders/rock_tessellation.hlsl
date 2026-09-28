// Lab 3: tessellation + displacement map + normal map + distance LOD
// Cracks: no Phong; UV-matched displacement; integer LOD
// Lab 8: RT3 ORM (ao / roughness / metallic)

cbuffer ObjectCB : register(b0)
{
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
    float4x4 gWorldViewProj;

    float3 gEyePosW;
    float _pad0;

    float gDispScale;
    float gMinTess;
    float gMaxTess;
    float gTessNear;

    float gTessFar;
    float gHasNormalTexture;
    float gHasDispTexture;
    float gNormalFlipY;

    float gRoughness;
    float gMetallic;
    float gAo;
    float _padPbr;
};

Texture2D gDiffuseMap : register(t0);
Texture2D gNormalMap : register(t1);
Texture2D gDispMap : register(t2);
SamplerState gSamLinearWrap : register(s0);

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
    float4 TangentL : TANGENT;
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
    float4 TangentW : TEXCOORD2;
    float2 Tex : TEXCOORD3;
};

struct PSOutput
{
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
    float4 position : SV_Target2;
    float4 material : SV_Target3;
};

float3 TangentFromNormal(float3 n)
{
    float3 up = (abs(n.y) > 0.999f) ? float3(1.f, 0.f, 0.f) : float3(0.f, 1.f, 0.f);
    float3 t = cross(up, n);
    return (dot(t, t) > 1e-8f) ? normalize(t) : float3(1.f, 0.f, 0.f);
}

float4 ResolveTangentL(float3 n, float4 storedT)
{
    float3 t = (dot(storedT.xyz, storedT.xyz) < 1e-8f)
        ? TangentFromNormal(n)
        : normalize(storedT.xyz - dot(storedT.xyz, n) * n);
    float signW = (abs(storedT.w) > 0.5f) ? sign(storedT.w) : 1.f;
    return float4(t, signW);
}

HsControlPoint VSMain(VSInput vin)
{
    HsControlPoint o;
    o.PosL = vin.PosL;
    o.NormalL = normalize(vin.NormalL);
    o.TangentL = ResolveTangentL(o.NormalL, vin.TangentL);
    o.Tex = vin.Tex;
    return o;
}

float TessFactorFromWorldPos(float3 posW)
{
    float d = distance(posW, gEyePosW);
    float t = saturate((gTessFar - d) / max(gTessFar - gTessNear, 0.001f));
    return lerp(gMinTess, gMaxTess, t);
}

// Same factor on all edges: avoids fractional mismatch; midpoints still used for LOD distance.
HS_CONSTANTS PatchConstantHS(InputPatch<HsControlPoint, 3> patch)
{
    HS_CONSTANTS hs;
    float3 c0 = mul(float4(patch[0].PosL, 1.f), gWorld).xyz;
    float3 c1 = mul(float4(patch[1].PosL, 1.f), gWorld).xyz;
    float3 c2 = mul(float4(patch[2].PosL, 1.f), gWorld).xyz;
    float3 center = (c0 + c1 + c2) * (1.0f / 3.0f);
    float tess = TessFactorFromWorldPos(center);
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

[domain("tri")]
DSOutput DSMain(
    HS_CONSTANTS hs,
    const OutputPatch<HsControlPoint, 3> patch,
    float3 bary : SV_DomainLocation)
{
    float u = bary.x;
    float v = bary.y;
    float w = bary.z;

    // Flat barycentric interpolation only — Phong caused cracks on shared edges.
    float3 posL = patch[0].PosL * u + patch[1].PosL * v + patch[2].PosL * w;
    float3 nL = normalize(patch[0].NormalL * u + patch[1].NormalL * v + patch[2].NormalL * w);
    float4 tL4 = patch[0].TangentL * u + patch[1].TangentL * v + patch[2].TangentL * w;
    float3 tL = normalize(tL4.xyz - dot(tL4.xyz, nL) * nL);
    float tangentW = (abs(patch[0].TangentL.w) > 0.5f) ? sign(patch[0].TangentL.w) : 1.f;
    float2 tex = patch[0].Tex * u + patch[1].Tex * v + patch[2].Tex * w;

    if (gHasDispTexture > 0.5f)
    {
        float h = gDispMap.SampleLevel(gSamLinearWrap, tex, 0).r;
        posL += nL * ((h - 0.5f) * gDispScale);
    }

    float4 posW4 = mul(float4(posL, 1.f), gWorld);
    float3 nW = normalize(mul(float4(nL, 0.f), gWorldInvTranspose).xyz);
    float3 tW = normalize(mul(float4(tL, 0.f), gWorld).xyz);
    tW = normalize(tW - dot(tW, nW) * nW);

    DSOutput o;
    o.PosW = posW4.xyz;
    o.NormalW = nW;
    o.TangentW = float4(tW, tangentW);
    o.PosH = mul(float4(posL, 1.f), gWorldViewProj);
    o.Tex = tex;
    return o;
}

float3 NormalFromMap(float3 Ngeom, float3 Tgeom, float tangentW, float3 nMapSample, float flipY)
{
    float3 nTex = nMapSample * 2.f - 1.f;
    if (flipY > 0.5f)
        nTex.y = -nTex.y;
    float3 T = normalize(Tgeom - dot(Tgeom, Ngeom) * Ngeom);
    float3 B = cross(Ngeom, T) * tangentW;
    return normalize(nTex.x * T + nTex.y * B + nTex.z * Ngeom);
}

PSOutput PSMain(DSOutput pin)
{
    float3 Ngeom = normalize(pin.NormalW);
    float3 N = Ngeom;
    if (gHasNormalTexture > 0.5f)
    {
        float3 nMap = gNormalMap.Sample(gSamLinearWrap, pin.Tex).rgb;
        N = NormalFromMap(Ngeom, pin.TangentW.xyz, pin.TangentW.w, nMap, gNormalFlipY);
    }

    float4 albedo = gDiffuseMap.Sample(gSamLinearWrap, pin.Tex);

    PSOutput o;
    o.albedo = float4(albedo.rgb, 1.f);
    o.normal = float4(N, 0.f);
    o.position = float4(pin.PosW, 1.f);
    o.material = float4(saturate(gAo), saturate(gRoughness), saturate(gMetallic), 1.f);
    return o;
}
