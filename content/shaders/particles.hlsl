// Lab 6: opaque particles — Append/Consume StructuredBuffers, CS update, GS billboards

cbuffer ParticleSimCB : register(b0)
{
    float gDeltaTime;
    float gGravity;
    uint gMaxParticles;
    uint gPadSim0;
    float3 gEmitterPos;
    float gPadSim1;
};

cbuffer ParticleDrawCB : register(b0)
{
    float4x4 gViewProj;
    float3 gCameraRight;
    float gPad0;
    float3 gCameraUp;
    float gPad1;
};

struct Particle
{
    float3 Pos;
    float Life;
    float3 Vel;
    float Size;
    float4 Color;
};

ConsumeStructuredBuffer<Particle> gConsumeIn : register(u0);
AppendStructuredBuffer<Particle> gAppendOut : register(u1);
StructuredBuffer<Particle> gRenderParticles : register(t0);

struct VSOut
{
    float3 Pos : POSITION;
    float Size : PSIZE;
    float4 Color : COLOR0;
};

struct GSOut
{
    float4 PosH : SV_POSITION;
    float2 Uv : TEXCOORD0;
    float4 Color : COLOR0;
};

[numthreads(64, 1, 1)]
void CS_Update(uint3 dtid : SV_DispatchThreadID)
{
    // Dispatch is exactly gMaxParticles/64 groups — every thread Consumes once.
    Particle p = gConsumeIn.Consume();

    const bool badLife = (p.Life <= 0.0f || p.Life > 10.0f || !isfinite(p.Life));
    const bool badPos =
        !all(isfinite(p.Pos)) ||
        any(abs(p.Pos - gEmitterPos) > 400.0f);

    if (!badLife && !badPos)
    {
        p.Vel.y += gGravity * gDeltaTime;
        p.Pos += p.Vel * gDeltaTime;
        p.Life -= gDeltaTime;
    }

    if (badLife || badPos || p.Life <= 0.0f)
    {
        const float u0 = frac(dtid.x * 0.6180339 + gDeltaTime * 0.07);
        const float u1 = frac(dtid.x * 0.37 + gDeltaTime * 0.11);
        const float u2 = frac(dtid.x * 0.19);
        const float a = u0 * 6.2831853;
        const float speed = 10.0 + u1 * 16.0;
        p.Pos = gEmitterPos + float3(0.0, 0.5, 0.0);
        p.Vel = float3(cos(a) * (2.0 + u2 * 5.0), speed, sin(a) * (2.0 + u2 * 5.0));
        p.Size = 1.1 + frac(dtid.x * 0.21) * 1.8;
        p.Color = float4(0.95, 0.55 + u1 * 0.35, 0.12 + u2 * 0.25, 1.0);
        p.Life = 2.0 + frac(dtid.x * 0.11) * 2.5;
    }

    gAppendOut.Append(p);
}

VSOut VS_Particle(uint id : SV_VertexID)
{
    Particle p = gRenderParticles[id];
    VSOut o;
    o.Pos = p.Pos;
    o.Size = p.Size;
    o.Color = p.Color;
    return o;
}

[maxvertexcount(4)]
void GS_Billboard(point VSOut input[1], inout TriangleStream<GSOut> triStream)
{
    const VSOut pin = input[0];
    const float halfSize = pin.Size * 0.5f;

    const float3 right = gCameraRight * halfSize;
    const float3 up = gCameraUp * halfSize;

    const float3 corners[4] = {
        pin.Pos - right - up,
        pin.Pos + right - up,
        pin.Pos - right + up,
        pin.Pos + right + up,
    };
    const float2 uvs[4] = {
        float2(0.0f, 1.0f),
        float2(1.0f, 1.0f),
        float2(0.0f, 0.0f),
        float2(1.0f, 0.0f),
    };

    // Triangle strip: BL, BR, TL, TR
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        GSOut o;
        o.PosH = mul(float4(corners[i], 1.0f), gViewProj);
        o.Uv = uvs[i];
        o.Color = pin.Color;
        triStream.Append(o);
    }
}

float4 PS_Particle(GSOut pin) : SV_TARGET
{
    // Opaque hard circle (no alpha blending).
    const float2 p = pin.Uv * 2.0f - 1.0f;
    if (dot(p, p) > 1.0f)
        discard;

    return float4(pin.Color.rgb, 1.0f);
}
