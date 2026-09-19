// Lab 2 доп: видимые кружки дождя из источников света (billboard в полёте, диск на полу).

cbuffer RainCircleDrawCB : register(b0)
{
	float4x4 gViewProj;
	float3 gCameraRight;
	float gPad0;
	float3 gCameraUp;
	float gPad1;
};

struct RainCircle
{
	float3 Pos;
	float Size;
	float4 Color;
	uint Landed;
	uint3 Pad;
};

StructuredBuffer<RainCircle> gRainCircles : register(t0);

struct VSOut
{
	float3 Pos : POSITION;
	float Size : PSIZE;
	float4 Color : COLOR0;
	uint Landed : LANDED;
};

struct GSOut
{
	float4 PosH : SV_POSITION;
	float2 Uv : TEXCOORD0;
	float4 Color : COLOR0;
};

VSOut VS_RainCircle(uint id : SV_VertexID)
{
	RainCircle c = gRainCircles[id];
	VSOut o;
	o.Pos = c.Pos;
	o.Size = c.Size;
	o.Color = c.Color;
	o.Landed = c.Landed;
	return o;
}

[maxvertexcount(4)]
void GS_RainCircle(point VSOut input[1], inout TriangleStream<GSOut> triStream)
{
	VSOut pin = input[0];
	const float halfSize = pin.Size * 0.5f;

	float3 corners[4];
	float2 uvs[4] = {
		float2(0.0f, 1.0f),
		float2(1.0f, 1.0f),
		float2(1.0f, 0.0f),
		float2(0.0f, 0.0f)
	};

	if (pin.Landed != 0u)
	{
		float3 p = pin.Pos;
		corners[0] = p + float3(-halfSize, 0.0f, -halfSize);
		corners[1] = p + float3( halfSize, 0.0f, -halfSize);
		corners[2] = p + float3( halfSize, 0.0f,  halfSize);
		corners[3] = p + float3(-halfSize, 0.0f,  halfSize);
	}
	else
	{
		float3 right = gCameraRight * halfSize;
		float3 up = gCameraUp * halfSize;
		corners[0] = pin.Pos - right - up;
		corners[1] = pin.Pos + right - up;
		corners[2] = pin.Pos + right + up;
		corners[3] = pin.Pos - right + up;
	}

	[unroll]
	for (uint i = 0u; i < 4u; ++i)
	{
		GSOut o;
		o.PosH = mul(float4(corners[i], 1.0f), gViewProj);
		o.Uv = uvs[i];
		o.Color = pin.Color;
		triStream.Append(o);
	}

	triStream.RestartStrip();
}

float4 PS_RainCircle(GSOut pin) : SV_TARGET
{
	float2 p = pin.Uv * 2.0f - 1.0f;
	float r2 = dot(p, p);
	if (r2 > 1.0f)
		discard;

	float core = saturate(1.0f - r2);
	float glow = pow(core, 1.6f);
	float4 col = pin.Color;
	col.rgb *= 1.15f;
	col.a *= glow;
	return col;
}
