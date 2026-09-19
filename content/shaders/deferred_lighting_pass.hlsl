// Lab 5 deferred lighting + CSM + PCF
// Lab 8: Cook-Torrance GGX metallic workflow (toggle via pbrEnabled)

static const uint kLightDirectional = 0;
static const uint kLightPoint = 1;
static const uint kLightSpot = 2;
static const uint kMaxLights = 16;
static const uint kCascadeCount = 4;
static const float kPi = 3.14159265359f;

struct GpuLight
{
    float3 position;
    uint type;
    float3 direction;
    float range;
    float3 color;
    float intensity;
    float spotInnerCos;
    float spotOuterCos;
    float padding0;
    float padding1;
};

cbuffer LightingCB : register(b0)
{
    float3 cameraPos;
    uint lightCount;
    float3 ambient;
    float shadowBias;

    GpuLight lights[kMaxLights];

    float4x4 gLightViewProj[kCascadeCount];
    float4 gCascadeSplits; // far view-Z of cascades 0..3
    float4x4 gView;
    float shadowEnabled;
    float pbrEnabled;
    float skyboxEnabled;
    float _padShadow;
    float4x4 invViewProj;
};

Texture2D gAlbedo : register(t0);
Texture2D gNormal : register(t1);
Texture2D gPosition : register(t2);
Texture2D gMaterial : register(t3); // R=AO, G=roughness, B=metallic
Texture2DArray gShadowMap : register(t4);
TextureCube gSkybox : register(t5);
SamplerState gPointClamp : register(s0);
SamplerComparisonState gShadowSampler : register(s1);
SamplerState gLinearClamp : register(s2);

struct VSOut
{
    float4 pos : SV_POSITION;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    VSOut o;
    float2 full = float2((vid << 1u) & 2u, vid & 2u);
    o.pos = float4(full.x * 2.0f - 1.0f, -full.y * 2.0f + 1.0f, 0.0f, 1.0f);
    return o;
}

float Attenuation(float distance, float range)
{
    float x = saturate(1.0f - (distance / max(range, 1e-3f)));
    return x * x;
}

float PbrLightAttenuation(float dist, float range)
{
    float distAtt = 1.0f / (dist * dist + 1.0f);
    float rangeAtt = saturate(1.0f - pow(dist / max(range, 1e-4f), 4.0f));
    rangeAtt *= rangeAtt;
    return distAtt * rangeAtt;
}

uint SelectCascade(float viewDepth)
{
    if (viewDepth < gCascadeSplits.x)
        return 0u;
    if (viewDepth < gCascadeSplits.y)
        return 1u;
    if (viewDepth < gCascadeSplits.z)
        return 2u;
    return 3u;
}

float CascadeFar(uint cascade)
{
    if (cascade == 0u) return gCascadeSplits.x;
    if (cascade == 1u) return gCascadeSplits.y;
    if (cascade == 2u) return gCascadeSplits.z;
    return gCascadeSplits.w;
}

float CascadeNear(uint cascade)
{
    if (cascade == 0u) return 0.0f;
    return CascadeFar(cascade - 1u);
}

// 5x5 PCF; radius grows with cascade. Taps outside map are skipped (not forced lit).
float SampleShadowPcf(uint cascade, float3 worldPos)
{
    float4 posH = mul(float4(worldPos, 1.0f), gLightViewProj[cascade]);
    float3 ndc = posH.xyz / max(posH.w, 1e-5f);

    float2 uv = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
    float depth = ndc.z;

    if (uv.x < -0.01f || uv.x > 1.01f || uv.y < -0.01f || uv.y > 1.01f || depth < 0.0f || depth > 1.0f)
        return 1.0f;

    uint w, h, elements, levels;
    gShadowMap.GetDimensions(0, w, h, elements, levels);
    float2 texel = 1.0f / float2(max(w, 1u), max(h, 1u));

    float radius = 1.5f + (float)cascade * 1.25f;
    float bias = shadowBias * (1.0f + (float)cascade * 0.75f);

    float sum = 0.0f;
    float weight = 0.0f;
    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            float2 sampleUv = uv + float2(x, y) * texel * radius;
            if (sampleUv.x < 0.0f || sampleUv.x > 1.0f || sampleUv.y < 0.0f || sampleUv.y > 1.0f)
                continue;

            sum += gShadowMap.SampleCmpLevelZero(
                gShadowSampler,
                float3(sampleUv, (float)cascade),
                depth - bias);
            weight += 1.0f;
        }
    }
    return (weight > 0.0f) ? (sum / weight) : 1.0f;
}

float DirectionalShadowFactor(float3 worldPos)
{
    if (shadowEnabled < 0.5f)
        return 1.0f;

    float viewDepth = mul(float4(worldPos, 1.0f), gView).z;
    uint cascade = SelectCascade(viewDepth);
    float s = SampleShadowPcf(cascade, worldPos);

    if (cascade > 0u)
    {
        float nearZ = CascadeNear(cascade);
        float farZ = CascadeFar(cascade);
        float blendRange = max((farZ - nearZ) * 0.12f, 1.0f);
        float blendStart = nearZ;
        float t = saturate((viewDepth - blendStart) / blendRange);
        if (t < 1.0f)
        {
            float sPrev = SampleShadowPcf(cascade - 1u, worldPos);
            s = lerp(sPrev, s, t);
        }
    }

    return s;
}

float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = saturate(dot(N, H));
    float denom = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / max(kPi * denom * denom, 1e-4f);
}

float GeometrySchlickGGX(float NdotX, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return NdotX / max(NdotX * (1.0f - k) + k, 1e-4f);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    float gv = GeometrySchlickGGX(saturate(dot(N, V)), roughness);
    float gl = GeometrySchlickGGX(saturate(dot(N, L)), roughness);
    return gv * gl;
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    float3 oneMinusRoughness = (1.0f - roughness).xxx;
    return F0 + (max(oneMinusRoughness, F0) - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

// Cook-Torrance BRDF (metallic workflow)
float3 EvaluatePBR(
    float3 N,
    float3 V,
    float3 L,
    float3 albedo,
    float roughness,
    float metallic,
    float3 radiance,
    float atten)
{
    float NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0f)
        return 0.0f.xxx;

    float3 H = normalize(V + L);
    float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);

    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    float3 F = FresnelSchlick(saturate(dot(H, V)), F0);

    float3 numerator = NDF * G * F;
    float denom = max(4.0f * saturate(dot(N, V)) * NdotL, 1e-4f);
    float3 specular = numerator / denom;

    float3 kS = F;
    float3 kD = (1.0f - kS) * (1.0f - metallic);
    float3 diffuse = kD * albedo / kPi;

    return (diffuse + specular) * radiance * NdotL * atten;
}

float3 ShadeLightLambert(GpuLight light, float3 worldPos, float3 n, float3 albedo)
{
    float3 contrib = 0.0f.xxx;

    if (light.type == kLightDirectional)
    {
        float3 l = normalize(-light.direction);
        float ndotl = saturate(dot(n, l));
        float shadow = DirectionalShadowFactor(worldPos);
        contrib = albedo * light.color * light.intensity * ndotl * shadow;
    }
    else if (light.type == kLightPoint)
    {
        float3 toLight = light.position - worldPos;
        float dist = length(toLight);
        if (dist < 1e-5f || dist > light.range)
            return contrib;
        float3 l = toLight / dist;
        float ndotl = saturate(dot(n, l));
        float att = Attenuation(dist, light.range);
        contrib = albedo * light.color * light.intensity * ndotl * att;
    }
    else
    {
        float3 toLight = light.position - worldPos;
        float dist = length(toLight);
        if (dist < 1e-5f || dist > light.range)
            return contrib;
        float3 l = toLight / dist;
        float ndotl = saturate(dot(n, l));
        float att = Attenuation(dist, light.range);
        float3 spotDir = normalize(light.direction);
        float cosAngle = dot(-l, spotDir);
        float spot = saturate((cosAngle - light.spotOuterCos) / max(light.spotInnerCos - light.spotOuterCos, 1e-4f));
        spot *= spot;
        contrib = albedo * light.color * light.intensity * ndotl * att * spot;
    }

    return contrib;
}

float3 ShadeLightPbr(
    GpuLight light,
    float3 worldPos,
    float3 n,
    float3 v,
    float3 albedo,
    float roughness,
    float metallic)
{
    float3 contrib = 0.0f.xxx;
    float3 radiance = light.color * light.intensity;

    if (light.type == kLightDirectional)
    {
        float3 l = normalize(-light.direction);
        float shadow = DirectionalShadowFactor(worldPos);
        contrib = EvaluatePBR(n, v, l, albedo, roughness, metallic, radiance, shadow);
    }
    else if (light.type == kLightPoint)
    {
        float3 toLight = light.position - worldPos;
        float dist = length(toLight);
        if (dist < 1e-5f || dist > light.range)
            return contrib;
        float3 l = toLight / dist;
        float att = PbrLightAttenuation(dist, light.range);
        contrib = EvaluatePBR(n, v, l, albedo, roughness, metallic, radiance, att);
    }
    else
    {
        float3 toLight = light.position - worldPos;
        float dist = length(toLight);
        if (dist < 1e-5f || dist > light.range)
            return contrib;
        float3 l = toLight / dist;
        float att = PbrLightAttenuation(dist, light.range);
        float3 spotDir = normalize(light.direction);
        float cosAngle = dot(-l, spotDir);
        float spot = saturate((cosAngle - light.spotOuterCos) / max(light.spotInnerCos - light.spotOuterCos, 1e-4f));
        spot *= spot;
        contrib = EvaluatePBR(n, v, l, albedo, roughness, metallic, radiance, att * spot);
    }

    return contrib;
}

float4 PSMain(VSOut input) : SV_TARGET
{
    uint w, h, mips;
    gAlbedo.GetDimensions(0, w, h, mips);
    float2 uv = input.pos.xy / float2(max(w, 1u), max(h, 1u));

    float4 albedoS = gAlbedo.Sample(gPointClamp, uv);
    float4 nS = gNormal.Sample(gPointClamp, uv);
    float4 pS = gPosition.Sample(gPointClamp, uv);
    float4 mS = gMaterial.Sample(gPointClamp, uv);

    float nLen2 = dot(nS.xyz, nS.xyz);
    float pLen2 = dot(pS.xyz, pS.xyz);
    if (nLen2 < 1e-6f || pLen2 < 1e-4f)
    {
        if (skyboxEnabled > 0.5f)
        {
            float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
            float4 worldH = mul(float4(ndc, 1.0f, 1.0f), invViewProj);
            float3 dir = normalize(worldH.xyz / max(worldH.w, 1e-5f) - cameraPos);
            float3 sky = gSkybox.Sample(gLinearClamp, dir).rgb;
            sky = sky / (1.0f + sky);
            return float4(sky, 1.0f);
        }
        return float4(0.45f, 0.55f, 0.70f, 1.0f);
    }

    float3 albedo = albedoS.rgb;
    float3 n = nS.xyz * rsqrt(nLen2);
    float3 worldPos = pS.xyz;
    float ao = saturate(mS.r);
    float roughness = max(mS.g, 0.04f);
    float metallic = saturate(mS.b);

    float3 color = 0.0f.xxx;
    uint count = min(lightCount, kMaxLights);

    if (pbrEnabled > 0.5f)
    {
        float3 v = normalize(cameraPos - worldPos);
        float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
        float3 kS = FresnelSchlickRoughness(saturate(dot(n, v)), F0, roughness);
        float3 kD = (1.0f - kS) * (1.0f - metallic);
        color = kD * albedo * ambient * ao;

        for (uint i = 0; i < count; ++i)
            color += ShadeLightPbr(lights[i], worldPos, n, v, albedo, roughness, metallic);
    }
    else
    {
        color = albedo * ambient;
        for (uint i = 0; i < count; ++i)
            color += ShadeLightLambert(lights[i], worldPos, n, albedo);
    }

    color = color / (1.0f + color);
    return float4(color, 1.0f);
}
