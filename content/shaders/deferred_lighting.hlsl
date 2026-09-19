// deferred_lighting.hlsl — Lab 8: deferred PBR (Cook-Torrance GGX / Beckmann + IBL)

Texture2D    gBufAlbedo   : register(t0);
Texture2D    gBufNormal   : register(t1);
Texture2D    gBufPosition : register(t2);
Texture2D    gBufMatExtra : register(t3);

static const uint kLightTypeDirectional = 0;
static const uint kLightTypePoint = 1;
static const uint kLightTypeSpot = 2;
static const float kPi = 3.14159265;

struct GpuLight
{
	float3 Position;
	uint   Type;
	float3 Direction;
	float  Range;
	float3 Color;
	float  Intensity;
	float  SpotInnerCos;
	float  SpotOuterCos;
	float2 Padding;
};

StructuredBuffer<GpuLight> gLights : register(t4);
Texture2DArray<float> gShadowMap : register(t5);
TextureCube gIrradianceMap : register(t6);
TextureCube gPrefilteredEnvMap : register(t7);
Texture2D   gIntegrationMap : register(t8);

cbuffer LightingCB : register(b0)
{
	float4 gEyeWorld;
	float4 gDirLightDirection;
	float4 gDirColorIntensity;
	uint   gNumLights;
	float  gMaxEnvMipLevel;
	float  gHasIblEnv;
	float  gUseBeckmann;
};

cbuffer ShadowLightingCB : register(b1)
{
	float4x4 gLightViewProj[3];
	float4x4 gViewMatrix;
	float4   gCascadeSplits;
	float    gCameraNear;
	float    gShadowBias;
	float2   gShadowMapInvSize;
};

SamplerState gPointClamp : register(s0);
SamplerComparisonState gShadowSampler : register(s1);
SamplerState gIblSampler : register(s2);

struct VSQuadOut
{
	float4 PosH : SV_POSITION;
	float2 TexC : TEXCOORD0;
};

VSQuadOut VS_Light(uint vid : SV_VertexID)
{
	VSQuadOut o;
	float2 full = float2((vid << 1u) & 2u, vid & 2u);
	o.PosH = float4(full.x * 2.f - 1.f, -full.y * 2.f + 1.f, 0.f, 1.f);
	o.TexC = float2(full.x * 0.5f, full.y * 0.5f);
	return o;
}

float3 ReinhardToneMap(float3 x)
{
	return x / (float3(1.f, 1.f, 1.f) + x);
}

float GetViewDepth(float3 worldPos)
{
	float4 v = mul(float4(worldPos, 1.0f), gViewMatrix);
	return max(v.z, 0.0f);
}

uint SelectCascade(float viewDepth)
{
	if (viewDepth < gCascadeSplits.x)
		return 0u;
	if (viewDepth < gCascadeSplits.y)
		return 1u;
	if (viewDepth < gCascadeSplits.z)
		return 2u;
	return 2u;
}

float SampleShadowPcf(uint cascade, float3 worldPos)
{
	float4 posL = mul(float4(worldPos, 1.0f), gLightViewProj[cascade]);
	float3 proj = posL.xyz / max(abs(posL.w), 1e-5f);
	float2 uv = proj.xy * 0.5f + 0.5f;
	uv.y = 1.0f - uv.y;

	float depth = saturate(proj.z);

	if (any(uv < 0.0f) || any(uv > 1.0f))
		return 1.0f;

	float shadow = 0.0f;
	[unroll]
	for (int y = -1; y <= 1; ++y)
	{
		[unroll]
		for (int x = -1; x <= 1; ++x)
		{
			float2 offset = float2(x, y) * gShadowMapInvSize;
			shadow += gShadowMap.SampleCmpLevelZero(
				gShadowSampler,
				float3(uv + offset, (float)cascade),
				depth - gShadowBias);
		}
	}
	return shadow / 9.0f;
}

float CalcDirectionalShadow(float3 worldPos)
{
	if (dot(worldPos, worldPos) < 1e-4f)
		return 1.0f;

	const float viewDepth = GetViewDepth(worldPos);
	uint cascade = SelectCascade(viewDepth);
	float s = SampleShadowPcf(cascade, worldPos);

	if (cascade > 0u)
	{
		float blendEnd;
		float blendRange;
		if (cascade == 1u)
		{
			blendEnd = gCascadeSplits.x;
			blendRange = max(blendEnd - gCameraNear, 1e-3f);
		}
		else
		{
			blendEnd = gCascadeSplits.y;
			blendRange = max(blendEnd - gCascadeSplits.x, 1e-3f);
		}
		const float t = saturate((viewDepth - (blendEnd - blendRange * 0.15f)) / (blendRange * 0.15f));
		const float sPrev = SampleShadowPcf(cascade - 1u, worldPos);
		s = lerp(sPrev, s, t);
	}

	return s;
}

float DistributionGGX(float3 N, float3 H, float roughness)
{
	float a = roughness * roughness;
	float a2 = a * a;
	float NdotH = saturate(dot(N, H));
	float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
	return a2 / max(kPi * denom * denom, 1e-4);
}

float GeometrySchlickGGX(float NdotX, float roughness)
{
	float r = roughness + 1.0;
	float k = (r * r) / 8.0;
	return NdotX / max(NdotX * (1.0 - k) + k, 1e-4);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
	float gv = GeometrySchlickGGX(saturate(dot(N, V)), roughness);
	float gl = GeometrySchlickGGX(saturate(dot(N, L)), roughness);
	return gv * gl;
}

float DistributionBeckmann(float3 N, float3 H, float roughness)
{
	float a = roughness * roughness;
	float a2 = a * a;
	float NdotH = saturate(dot(N, H));
	float NdotH2 = NdotH * NdotH;
	if (NdotH2 < 1e-4)
		return 0.0;
	float tan2 = (1.0 - NdotH2) / NdotH2;
	float expTerm = exp(-tan2 / max(a2, 1e-4));
	return expTerm / max(kPi * a2 * NdotH2 * NdotH2, 1e-4);
}

float GeometrySchlickBeckmann(float NdotX, float roughness)
{
	float k = roughness * sqrt(2.0 / kPi);
	return NdotX / max(NdotX * (1.0 - k) + k, 1e-4);
}

float GeometrySmithBeckmann(float3 N, float3 V, float3 L, float roughness)
{
	float gv = GeometrySchlickBeckmann(saturate(dot(N, V)), roughness);
	float gl = GeometrySchlickBeckmann(saturate(dot(N, L)), roughness);
	return gv * gl;
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
	return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
	float3 oneMinusRoughness = float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness);
	return F0 + (max(oneMinusRoughness, F0) - F0) * pow(1.0 - cosTheta, 5.0);
}

// PBR attenuation: inverse-square (+1 avoids singularity) with smooth range cutoff
float PbrLightAttenuation(float dist, float range)
{
	float distAtt = 1.0 / (dist * dist + 1.0);
	float rangeAtt = saturate(1.0 - pow(dist / max(range, 1e-4), 4.0));
	rangeAtt *= rangeAtt;
	return distAtt * rangeAtt;
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
	if (NdotL <= 0.0)
		return float3(0, 0, 0);

	float3 H = normalize(V + L);
	float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

	const bool useBeckmann = gUseBeckmann > 0.5;
	float NDF = useBeckmann
		? DistributionBeckmann(N, H, roughness)
		: DistributionGGX(N, H, roughness);
	float G = useBeckmann
		? GeometrySmithBeckmann(N, V, L, roughness)
		: GeometrySmith(N, V, L, roughness);
	float3 F = FresnelSchlick(saturate(dot(H, V)), F0);

	float3 numerator = NDF * G * F;
	float denom = max(4.0 * saturate(dot(N, V)) * NdotL, 1e-4);
	float3 specular = numerator / denom;

	float3 kS = F;
	float3 kD = (1.0 - kS) * (1.0 - metallic);
	float3 diffuse = kD * albedo / kPi;

	return (diffuse + specular) * radiance * NdotL * atten;
}

// Split-sum IBL (irradiance + prefiltered env + BRDF LUT from Stuff/)
float3 EvaluateIBL(
	float3 N,
	float3 V,
	float3 albedo,
	float roughness,
	float metallic,
	float ao)
{
	float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
	float NdotV = saturate(dot(N, V));
	float3 kS = FresnelSchlickRoughness(NdotV, F0, roughness);
	float3 kD = (1.0 - kS) * (1.0 - metallic);

	float3 irradiance = gIrradianceMap.Sample(gIblSampler, N).rgb;
	float3 diffuse = irradiance * albedo;

	float3 R = reflect(-V, N);
	float lod = roughness * gMaxEnvMipLevel;
	float3 prefiltered = gPrefilteredEnvMap.SampleLevel(gIblSampler, R, lod).rgb;
	float2 brdf = gIntegrationMap.Sample(gIblSampler, float2(NdotV, roughness)).rg;
	float3 specular = prefiltered * (F0 * brdf.x + brdf.y);

	return (kD * diffuse + specular) * ao;
}

float3 SampleEnvironmentBackground(float2 uv)
{
	const float3 flatSky = float3(0.04, 0.05, 0.08);
	if (gHasIblEnv < 0.5f)
		return flatSky;

	float2 ndc = float2(uv.x * 2.f - 1.f, 1.f - uv.y * 2.f);
	float3 rightW = normalize(float3(gViewMatrix._11, gViewMatrix._21, gViewMatrix._31));
	float3 upW = normalize(float3(gViewMatrix._12, gViewMatrix._22, gViewMatrix._32));
	float3 fwdW = normalize(-float3(gViewMatrix._13, gViewMatrix._23, gViewMatrix._33));
	float3 dirW = normalize(fwdW + rightW * ndc.x * 0.75 + upW * ndc.y * 0.75);
	return gPrefilteredEnvMap.SampleLevel(gIblSampler, dirW, 0).rgb;
}

float4 PS_Light(VSQuadOut pin, float4 posSs : SV_Position) : SV_Target
{
	uint w, h, levels;
	gBufAlbedo.GetDimensions(0, w, h, levels);
	float2 dims = float2(max(w, 1u), max(h, 1u));
	float2 uv = posSs.xy / dims;

	float4 albedoS = gBufAlbedo.Sample(gPointClamp, uv);
	float3 albedo = albedoS.rgb;

	float4 nS = gBufNormal.Sample(gPointClamp, uv);
	float nLen2 = dot(nS.xyz, nS.xyz);

	float4 pS = gBufPosition.Sample(gPointClamp, uv);
	float3 pw = pS.xyz;

	if (nLen2 < 1e-6f || dot(pw, pw) < 1e-4f)
		return float4(ReinhardToneMap(SampleEnvironmentBackground(uv)), 1.f);

	float3 N = nS.xyz * rsqrt(nLen2);

	float4 mx = gBufMatExtra.Sample(gPointClamp, uv);
	float roughness = max(mx.r, 0.04);
	float metallic = saturate(mx.g);
	float ao = saturate(mx.b);

	float3 eye = gEyeWorld.xyz;
	float3 vw = eye - pw;
	float vLen2 = dot(vw, vw);
	float3 V = vLen2 > 1e-10f ? vw * rsqrt(vLen2) : float3(0.f, 0.f, -1.f);

	float3 Lo = float3(0, 0, 0);

	float3 dd = gDirLightDirection.xyz;
	float dDirs = dot(dd, dd);
	if (dDirs > 1e-8f)
	{
		float3 L = normalize(-dd);
		float3 sunCol = gDirColorIntensity.xyz * gDirColorIntensity.w;
		float shadow = CalcDirectionalShadow(pw);
		Lo += EvaluatePBR(N, V, L, albedo, roughness, metallic, sunCol, shadow);
	}

	const uint MAX_L = 48u;
	uint nLights = min(gNumLights, MAX_L);

	for (uint i = 0u; i < nLights; ++i)
	{
		GpuLight Ld = gLights[i];
		if (Ld.Type == kLightTypeDirectional)
			continue;

		float3 col = Ld.Color * Ld.Intensity;

		if (Ld.Type == kLightTypePoint)
		{
			float3 toL = Ld.Position - pw;
			float dist = length(toL);
			if (dist < 1e-5f || dist > Ld.Range)
				continue;

			float3 L = toL / dist;
			float att = PbrLightAttenuation(dist, Ld.Range);
			Lo += EvaluatePBR(N, V, L, albedo, roughness, metallic, col, att);
		}
		else if (Ld.Type == kLightTypeSpot)
		{
			float3 toL = Ld.Position - pw;
			float dist = length(toL);
			if (dist < 1e-5f || dist > Ld.Range)
				continue;

			float3 L = toL / dist;
			float3 dirL = Ld.Direction;
			float dLen2 = dot(dirL, dirL);
			if (dLen2 < 1e-8f)
				continue;
			dirL = normalize(dirL);
			float cosAngle = dot(-L, dirL);
			float outer = max(Ld.SpotOuterCos, 1e-4f);

			float spot = saturate((cosAngle - outer) / max(Ld.SpotInnerCos - outer, 1e-4f));
			if (spot <= 0.f)
				continue;

			float att = PbrLightAttenuation(dist, Ld.Range) * spot;
			Lo += EvaluatePBR(N, V, L, albedo, roughness, metallic, col, att);
		}
	}

	float3 hdrLinear = Lo + EvaluateIBL(N, V, albedo, roughness, metallic, ao);

	return float4(ReinhardToneMap(hdrLinear), 1.f);
}
