// Lab 4 доп: camera-facing billboards в G-buffer вместо далёких инстансов (камни).

cbuffer BillboardCB : register(b0)
{
	float4x4 gViewProj;
	float3 gCameraRight;
	float gPad0;
	float3 gCameraUp;
	float gPad1;
	float3 gEyePosW;
	float gPad2;
	float3 gMatKd;
	float gHasDiffuseTexture;
	float gMatRoughness;
	float gMatMetallic;
	float gHasRmTexture;
	float gMatNsFallback;
};

struct BillboardInst
{
	float4 Data0;
	float4 Data1;
};

StructuredBuffer<BillboardInst> gBillboards : register(t0);
Texture2D gDiffuseMap : register(t1);
Texture2D gRmMap : register(t2);
SamplerState gSamLinearWrap : register(s0);

struct VSOut
{
	float3 Center : CENTER;
	float HalfWidth : HW;
	float HalfHeight : HH;
};

struct GSOut
{
	float4 PosH : SV_POSITION;
	float3 PosW : TEXCOORD0;
	float3 NormalW : TEXCOORD1;
	float2 Tex : TEXCOORD2;
};

struct GBufferPack
{
	float4 AlbedoA : SV_Target0;
	float4 NormalW : SV_Target1;
	float4 PosWorld : SV_Target2;
	float4 PbrExtra : SV_Target3;
};

VSOut VS_Billboard(uint id : SV_VertexID)
{
	BillboardInst b = gBillboards[id];
	VSOut o;
	o.Center = b.Data0.xyz;
	o.HalfWidth = b.Data0.w;
	o.HalfHeight = b.Data1.x;
	return o;
}

void AppendBillboardCorner(
	float3 posW,
	float3 center,
	float2 uv,
	inout TriangleStream<GSOut> triStream)
{
	GSOut o;
	o.PosW = posW;
	o.PosH = mul(float4(posW, 1.0f), gViewProj);
	o.NormalW = normalize(gEyePosW - center);
	o.Tex = uv;
	triStream.Append(o);
}

// Y-up спрайт, повёрнутый к камере; два треугольника (без кривого triangle strip).
[maxvertexcount(6)]
void GS_Billboard(point VSOut input[1], inout TriangleStream<GSOut> triStream)
{
	const float3 c = input[0].Center;
	const float halfW = input[0].HalfWidth;
	const float halfH = input[0].HalfHeight;

	float3 toEye = gEyePosW - c;
	toEye.y = 0.0f;
	const float xzLen = length(toEye);
	const float3 faceXZ = (xzLen > 1e-4f) ? (toEye / xzLen) : float3(0.0f, 0.0f, 1.0f);
	const float3 right = float3(faceXZ.z, 0.0f, -faceXZ.x);
	const float3 up = float3(0.0f, 1.0f, 0.0f);

	const float3 bl = c - right * halfW - up * halfH;
	const float3 br = c + right * halfW - up * halfH;
	const float3 tl = c - right * halfW + up * halfH;
	const float3 tr = c + right * halfW + up * halfH;

	AppendBillboardCorner(bl, c, float2(0.0f, 1.0f), triStream);
	AppendBillboardCorner(br, c, float2(1.0f, 1.0f), triStream);
	AppendBillboardCorner(tl, c, float2(0.0f, 0.0f), triStream);
	triStream.RestartStrip();

	AppendBillboardCorner(br, c, float2(1.0f, 1.0f), triStream);
	AppendBillboardCorner(tr, c, float2(1.0f, 0.0f), triStream);
	AppendBillboardCorner(tl, c, float2(0.0f, 0.0f), triStream);
	triStream.RestartStrip();
}

float3 SrgbToLinear(float3 srgb)
{
	return pow(max(srgb, 0.0), 2.2);
}

float RoughnessFromNs(float ns)
{
	return saturate(sqrt(2.0 / (ns + 2.0)));
}

GBufferPack PS_Billboard(GSOut pin)
{
	GBufferPack o;

	float3 N = normalize(pin.NormalW);
	float3 texRgb = SrgbToLinear(gDiffuseMap.Sample(gSamLinearWrap, pin.Tex).rgb);
	float3 alb = gMatKd * lerp(float3(1, 1, 1), texRgb, gHasDiffuseTexture);

	float roughness = RoughnessFromNs(gMatNsFallback) * gMatRoughness;
	float metallic = gMatMetallic;
	float ao = 1.0;
	if (gHasRmTexture > 0.5f)
	{
		float4 arm = gRmMap.Sample(gSamLinearWrap, pin.Tex);
		if (arm.g < 0.05 && arm.r > 0.05)
		{
			roughness = arm.r * max(gMatRoughness, 0.001);
			metallic = 0.0;
		}
		else
		{
			ao = arm.r;
			roughness = arm.g * max(gMatRoughness, 0.001);
			metallic = arm.b * ((gMatMetallic > 0.001) ? gMatMetallic : 1.0);
		}
	}

	o.AlbedoA = float4(alb, 1.0);
	o.NormalW = float4(N, 0.0);
	o.PosWorld = float4(pin.PosW, 1.0);
	o.PbrExtra = float4(saturate(roughness), saturate(metallic), saturate(ao), 1.0);
	return o;
}
