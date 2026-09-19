// =============================================================================
// deferred_tessellation.hlsl — геометрический проход Lab 3 (deferred G-buffer)
// =============================================================================
//
// Пайплайн DirectX 12 (один draw = один треугольник OBJ как патч из 3 точек):
//
//   IA: PATCHLIST (3 control points)
//        ↓
//   VS  — подготовка контрольных точек для hull
//        ↓
//   HS  — PatchConstantHS: факторы тесселяции (LOD по камере)
//       — HullHS: передаёт вершины патча без изменений
//        ↓  [фиксированный tessellator GPU режет рёбра по факторам]
//   DS  — интерполяция внутри патча + displacement (Lab 3 доп: Perlin noise)
//        ↓
//   PS  — albedo, normal map → запись в G-buffer (4 RT)
//        ↓
//   deferred_lighting.hlsl читает G-buffer и рисует освещение на экран
//
// Текстуры материала (root signature, таблица SRV):
//   t0 — diffuse (map_Kd)
//   t1 — normal map (map_Bump / norm)
//   t2 — displacement map (не используется в DS после Lab 3 доп; SRV остаётся в root)
//   t3 — ARM: AO / roughness / metallic (map_ARM), Lab 8 PBR
//
// DebugMode (клавиша T в приложении):
//   0 — нормальный рендер
//   1 — heatmap плотности tess
//   2 — wireframe (переключается PSO на CPU, здесь только цвет)
//   3 — tess без displacement (сравнение формы)
// =============================================================================

// --- Constant buffer: те же поля, что struct ObjectConstants в ObjTexturesDemoApp.h ---
cbuffer ObjectCB : register(b0)
{
	float4x4 gWorld;              // объект → мир
	float4x4 gWorldInvTranspose;  // для корректного преобразования нормалей
	float4x4 gWorldViewProj;      // объект → clip (после displacement в DS)

	float3   gEyePosW;          // позиция камеры (мир) — для LOD tess
	float    _pad0;

	float3   gMatKd;
	float    gHasDiffuseTexture;

	float    gMatRoughness;
	float    gMatMetallic;
	float    gHasRmTexture;
	float    gMatNsFallback;

	float2   gUvScale;            // масштаб UV (сейчас 1,1 с CPU)
	float2   _pad1;

	// Lab 3: тесселяция и displacement
	float    gDispScale;          // амплитуда сдвига по height map (метры в лок. пространстве)
	float    gMinTess;            // мин. фактор tess (далеко от камеры)
	float    gMaxTess;            // макс. фактор tess (близко)
	float    gTessNear;           // дистанция, на которой tess = gMaxTess

	float    gTessFar;            // дистанция, на которой tess = gMinTess
	float    gHasNormalTexture;   // 1 — perturb normal из gNormalMap
	float    gDebugMode;          // см. константы ниже
	float    gNormalFlipY;        // 1 — OpenGL normal map (Sponza ddn)

	// Lab 1: вертексная анимация squash/stretch ваз с цветами на Sponza
	float    gVertexAnimEnable;   // 1 — применить анимацию к submesh
	float    gVertexAnimPivotX;   // опора по XZ (центр основания вазы)
	float    gVertexAnimPivotY;   // опора по Y (дно вазы)
	float    gVertexAnimPivotZ;

	float    gVertexAnimPhase;    // сдвиг фазы (разные вазы не синхронно)
	float    gVertexAnimTime;     // глобальное время, сек
	float    gVertexAnimAmp;      // амплитуда сжатия (0.15 ≈ ±15% по Y)
	float    gVertexAnimSpeed;    // скорость sin-волны

	// Lab 3 доп: procedural displacement (Perlin) вместо height map
	float    gPerlinSeed;         // сид — меняется F6 на CPU
	float    gPerlinFrequency;    // масштаб шума в локальных координатах
	// Lab 6 доп: alpha test (забор, cutout)
	float    gAlphaTestEnable;
	float    gAlphaTestCutoff;
};

static const float kDbgTessHeatmap = 1.f;  // режим визуализации LOD
static const float kDbgWireframe = 2.f;    // цвет под wireframe PSO
static const float kDbgTessNoDisp = 3.f;   // tess есть, displacement выключен

Texture2D    gDiffuseMap : register(t0);
Texture2D    gNormalMap   : register(t1);
Texture2D    gDispMap     : register(t2);
Texture2D    gRmMap       : register(t3);
SamplerState gSamLinearWrap : register(s0);

// Вход из vertex buffer (layout совпадает с struct Vertex в C++)
struct VSInput
{
	float3 PosL     : POSITION;
	float3 NormalL  : NORMAL;
	float2 Tex      : TEXCOORD0;
	float4 TangentL : TANGENT;
};

// Выход VS = одна контрольная точка патча для hull/domain
struct HsControlPoint
{
	float3 PosL     : POSITION;
	float3 NormalL  : NORMAL;
	float4 TangentL : TANGENT;   // xyz=tangent, w=bitangent sign
	float2 Tex      : TEXCOORD0;
};

// Факторы тесселяции: 3 рёбра треугольника + 1 inside (для tri domain)
struct HS_CONSTANTS
{
	float Edge[3] : SV_TessFactor;
	float Inside  : SV_InsideTessFactor;
};

	float2 TransformUv(float2 rawTex)
{
	float2 uv = float2(rawTex.x * abs(gUvScale.x), rawTex.y * abs(gUvScale.y));
	if (gUvScale.x < 0.f)
		uv.x = 1.f - uv.x;
	if (gUvScale.y < 0.f)
		uv.y = 1.f - uv.y;
	return uv;
}

// Строит касательную по нормали (в OBJ часто нет tangent — нужен для normal map)
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
	return float4(t, storedT.w);
}

// Lab 1: сжатие/растяжение по Y от дна вазы + лёгкое «дыхание» по XZ
float3 ApplyVaseVertexAnim(float3 posL)
{
	if (gVertexAnimEnable < 0.5f)
		return posL;

	const float wave = sin(gVertexAnimTime * gVertexAnimSpeed + gVertexAnimPhase);
	const float sy = 1.0 + gVertexAnimAmp * wave;
	const float sxz = rsqrt(max(sy, 0.2));

	float3 animated = posL;
	animated.y = gVertexAnimPivotY + (animated.y - gVertexAnimPivotY) * sy;
	animated.x = gVertexAnimPivotX + (animated.x - gVertexAnimPivotX) * sxz;
	animated.z = gVertexAnimPivotZ + (animated.z - gVertexAnimPivotZ) * sxz;
	return animated;
}

// --- Lab 3 доп: 3D gradient noise (Perlin-style) для displacement ---
float3 PerlinHash3(float3 p, float seed)
{
	p = frac(p * float3(0.1031, 0.1030, 0.0973) + seed * 0.137);
	p += dot(p, p.yxz + 33.33);
	return frac((p.xxy + p.yxx) * p.zyx) * 2.0 - 1.0;
}

float3 PerlinFade(float3 t)
{
	return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

float PerlinNoise3(float3 p, float seed)
{
	float3 i = floor(p);
	float3 f = frac(p);
	float3 u = PerlinFade(f);

	float n000 = dot(PerlinHash3(i + float3(0, 0, 0), seed), f - float3(0, 0, 0));
	float n100 = dot(PerlinHash3(i + float3(1, 0, 0), seed), f - float3(1, 0, 0));
	float n010 = dot(PerlinHash3(i + float3(0, 1, 0), seed), f - float3(0, 1, 0));
	float n110 = dot(PerlinHash3(i + float3(1, 1, 0), seed), f - float3(1, 1, 0));
	float n001 = dot(PerlinHash3(i + float3(0, 0, 1), seed), f - float3(0, 0, 1));
	float n101 = dot(PerlinHash3(i + float3(1, 0, 1), seed), f - float3(1, 0, 1));
	float n011 = dot(PerlinHash3(i + float3(0, 1, 1), seed), f - float3(0, 1, 1));
	float n111 = dot(PerlinHash3(i + float3(1, 1, 1), seed), f - float3(1, 1, 1));

	float nx00 = lerp(n000, n100, u.x);
	float nx10 = lerp(n010, n110, u.x);
	float nx01 = lerp(n001, n101, u.x);
	float nx11 = lerp(n011, n111, u.x);
	float nxy0 = lerp(nx00, nx10, u.y);
	float nxy1 = lerp(nx01, nx11, u.y);
	return lerp(nxy0, nxy1, u.z);
}

float PerlinFbm3(float3 p, float seed)
{
	float value = 0.0;
	float amplitude = 0.5;
	float frequency = 1.0;
	[unroll]
	for (int octave = 0; octave < 4; ++octave)
	{
		value += amplitude * PerlinNoise3(p * frequency, seed + octave * 19.17);
		frequency *= 2.03;
		amplitude *= 0.5;
	}
	return value;
}

float SamplePerlinDisplacement(float3 posL)
{
	const float3 samplePos = posL * gPerlinFrequency + float3(gPerlinSeed * 1.73, gPerlinSeed * 2.41, gPerlinSeed * 0.97);
	return PerlinFbm3(samplePos, gPerlinSeed);
}

// -----------------------------------------------------------------------------
// VERTEX SHADER (VS)
// Вызывается 1 раз на каждую вершину исходного меша (контрольные точки патча).
// Не двигает геометрию — только упаковывает атрибуты для hull.
// -----------------------------------------------------------------------------
HsControlPoint VS(VSInput vin)
{
	HsControlPoint o;
	o.PosL = vin.PosL;
	o.NormalL = normalize(vin.NormalL);
	o.TangentL = ResolveTangentL(o.NormalL, vin.TangentL);
	o.Tex = TransformUv(vin.Tex);
	return o;
}

// LOD: линейная интерполяция tess factor между gMinTess и gMaxTess по расстоянию до gEyePosW
float TessFactorFromWorldPos(float3 posW)
{
	float d = distance(posW, gEyePosW);
	float t = saturate((gTessFar - d) / max(gTessFar - gTessNear, 0.001));
	return lerp(gMinTess, gMaxTess, t);
}

// -----------------------------------------------------------------------------
// HULL SHADER — constant function (PatchConstantHS)
// 1 раз на патч (треугольник). Задаёт, сколько сегментов на ребро/внутри.
// Центр патча в мировых координатах → одинаковый tess на все 4 фактора
// (меньше полос/швов на плоских сетках, чем при разных edge factors).
// -----------------------------------------------------------------------------
HS_CONSTANTS PatchConstantHS(InputPatch<HsControlPoint, 3> patch)
{
	HS_CONSTANTS hs;                                     // выход: 3 edge + 1 inside factor
	float3 c0 = mul(float4(patch[0].PosL, 1.f), gWorld).xyz; // угол 0 патча в мире
	float3 c1 = mul(float4(patch[1].PosL, 1.f), gWorld).xyz; // угол 1
	float3 c2 = mul(float4(patch[2].PosL, 1.f), gWorld).xyz; // угол 2
	float3 center = (c0 + c1 + c2) / 3.f;                // центр треугольника в мире

	float tess = TessFactorFromWorldPos(center);           // LOD: ближе камера → больше число
	hs.Edge[0] = tess;                                   // сколько сегментов на ребро 0-1
	hs.Edge[1] = tess;                                   // ребро 1-2
	hs.Edge[2] = tess;                                   // ребро 2-0
	hs.Inside = tess;                                    // плотность внутри (tri domain)
	return hs;                                           // дальше — фикс. tessellator GPU (не HLSL)
}

// -----------------------------------------------------------------------------
// HULL SHADER — control points (HullHS)
// Pass-through: контрольные точки не смещаются на hull (смещение только в DS).
// Атрибуты domain: tri, integer partitioning, CW, 3 CP, max tess 64.
// -----------------------------------------------------------------------------
[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PatchConstantHS")]
[maxtessfactor(64.0)]
HsControlPoint HullHS(InputPatch<HsControlPoint, 3> patch, uint i : SV_OutputControlPointID)
{
	return patch[i];                                     // hull НЕ сдвигает точки; i = 0,1,2
}

// Выход domain shader → вход pixel shader
struct DSOutput
{
	float4 PosH     : SV_POSITION;
	float3 PosW     : TEXCOORD0;
	float3 NormalW  : TEXCOORD1;
	float4 TangentW : TEXCOORD2; // xyz=tangent, w=handedness
	float2 Tex      : TEXCOORD3;
	float  TessLevel : TEXCOORD4;  // для debug heatmap
};

// -----------------------------------------------------------------------------
// DOMAIN SHADER (DS)
// Вызывается для каждой точки, которую tessellator создал внутри патча.
// bary = барицентрические координаты (u,v,w), u+v+w=1.
//
// 1) Линейная интерполяция pos/normal/tangent/UV по контрольным точкам
// 2) Displacement: Lab 3 доп — Perlin FBM вдоль nL (вместо gDispMap)
// 3) Преобразование в мир и clip
// -----------------------------------------------------------------------------
[domain("tri")]
DSOutput DomainDS(
	HS_CONSTANTS hs,
	const OutputPatch<HsControlPoint, 3> patch,
	float3 bary : SV_DomainLocation)
{
	float u = bary.x;                                    // барицентрические веса (сумма = 1)
	float v = bary.y;                                    // от tessellator: где точка внутри патча
	float w = bary.z;

	// Шаг 1 domain: интерполяция атрибутов на ПЛОСКОМ (или сглаженном) треугольнике
	float3 posL = patch[0].PosL * u + patch[1].PosL * v + patch[2].PosL * w;
	float3 nL = normalize(patch[0].NormalL * u + patch[1].NormalL * v + patch[2].NormalL * w);
	float4 tL4 = patch[0].TangentL * u + patch[1].TangentL * v + patch[2].TangentL * w;
	float3 tL = normalize(tL4.xyz - dot(tL4.xyz, nL) * nL);
	float tangentW = (abs(patch[0].TangentL.w) > 0.5f) ? sign(patch[0].TangentL.w) : 1.f;
	float2 tex = patch[0].Tex * u + patch[1].Tex * v + patch[2].Tex * w;

	// Шаг 2 domain: высота из 3D Perlin noise (сид gPerlinSeed с CPU, F6)
	float h = SamplePerlinDisplacement(posL);
	float dispOff = h * gDispScale;
	if (abs(gDebugMode - kDbgTessNoDisp) > 0.5f)
		posL += nL * dispOff;

	// Средний tess factor патча (для отладочной раскраски в PS)
	float tessLevel = (hs.Edge[0] + hs.Edge[1] + hs.Edge[2] + hs.Inside) * 0.25f;

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
	o.TessLevel = tessLevel;
	return o;
}

// -----------------------------------------------------------------------------
// VS_GBuffer — камни без tess/displacement; выход = DSOutput → тот же PS
// -----------------------------------------------------------------------------
DSOutput VS_GBuffer(VSInput vin)
{
	float3 posL = ApplyVaseVertexAnim(vin.PosL);
	float3 nL = normalize(vin.NormalL);
	float4 tL4 = ResolveTangentL(nL, vin.TangentL);
	float3 tL = tL4.xyz;
	float2 tex = TransformUv(vin.Tex);

	float4 posW4 = mul(float4(posL, 1.f), gWorld);
	float3 nW = normalize(mul(float4(nL, 0.f), gWorldInvTranspose).xyz);
	float3 tW = normalize(mul(float4(tL, 0.f), gWorld).xyz);
	tW = normalize(tW - dot(tW, nW) * nW);

	DSOutput o;
	o.PosW = posW4.xyz;
	o.NormalW = nW;
	o.TangentW = float4(tW, tL4.w);
	o.PosH = mul(float4(posL, 1.f), gWorldViewProj);
	o.Tex = tex;
	o.TessLevel = 1.f;
	return o;
}

// Псевдоцвет для режима debug 1: синий → зелёный → красный = рост tess factor
float3 TessHeatmap(float t)
{
	t = saturate(t);
	float3 c = lerp(float3(0.1, 0.2, 0.9), float3(0.1, 0.95, 0.35), t);
	return lerp(c, float3(0.95, 0.25, 0.1), smoothstep(0.55, 1.f, t));
}

float3 SrgbToLinear(float3 srgb)
{
	float3 c = max(srgb, 0.0);
	return pow(c, 2.2);
}

// Phong Ns → perceptual roughness (Disney-style approximation)
float RoughnessFromNs(float ns)
{
	return saturate(sqrt(2.0 / (ns + 2.0)));
}

// Формат G-buffer (совпадает с deferred_lighting.hlsl)
struct GBufferPack
{
	float4 AlbedoA    : SV_Target0;
	float4 NormalW    : SV_Target1;
	float4 PosWorld   : SV_Target2;
	float4 PbrExtra   : SV_Target3;  // r=roughness, g=metallic, b=AO
};

// Perturbation: tangent space normal map → мировая нормаль (T, B, N)
float3 NormalFromMap(float3 Ngeom, float3 Tgeom, float tangentW, float3 nMapSample, float flipY)
{
	float3 nTex = nMapSample * 2.f - 1.f;       // [0,1] → [-1,1]
	if (flipY > 0.5f)
		nTex.y = -nTex.y;
	float3 T = normalize(Tgeom - dot(Tgeom, Ngeom) * Ngeom);
	float3 B = cross(Ngeom, T) * tangentW;
	return normalize(nTex.x * T + nTex.y * B + nTex.z * Ngeom);
}

// -----------------------------------------------------------------------------
// PIXEL SHADER (PS)
// На каждый пиксель треугольника после tess + displacement:
//   - normal map (если есть) → RT1
//   - diffuse → RT0
//   - позиция и материал для deferred lighting
// Normal map не двигает вершины — только направление света в следующем проходе.
// -----------------------------------------------------------------------------
GBufferPack PS(DSOutput pin)
{
	GBufferPack o;

	// Шаг PS: normal map ПОСЛЕ displacement — меняет только освещение, не posL
	float3 Ngeom = normalize(pin.NormalW);               // нормаль от смещённой поверхности (мир)
	float3 Tgeom = pin.TangentW.xyz;                       // для базиса TBN
	float3 nMap = gNormalMap.Sample(gSamLinearWrap, pin.Tex).rgb; // tangent-space normal
	float3 N = (gHasNormalTexture > 0.5f)
		? NormalFromMap(Ngeom, Tgeom, pin.TangentW.w, nMap, gNormalFlipY)
		: Ngeom;

	float3 alb;
	if (gDebugMode > kDbgTessHeatmap - 0.5f && gDebugMode < kDbgTessHeatmap + 0.5f)
		alb = TessHeatmap(pin.TessLevel / max(gMaxTess, 1.f));
	else if (gDebugMode > kDbgWireframe - 0.5f && gDebugMode < kDbgWireframe + 0.5f)
		alb = float3(0.15, 0.85, 0.35);
	else if (gDebugMode > kDbgTessNoDisp - 0.5f && gDebugMode < kDbgTessNoDisp + 0.5f)
	{
		float3 texRgb = SrgbToLinear(gDiffuseMap.Sample(gSamLinearWrap, pin.Tex).rgb);
		alb = gMatKd * lerp(float3(1, 1, 1), texRgb, gHasDiffuseTexture) * 0.65f;
	}
	else
	{
		float4 texSample = gDiffuseMap.Sample(gSamLinearWrap, pin.Tex);
		float3 texRgb = SrgbToLinear(texSample.rgb);
		if (gAlphaTestEnable > 0.5f)
			clip(texSample.a - gAlphaTestCutoff);
		alb = gMatKd * lerp(float3(1, 1, 1), texRgb, gHasDiffuseTexture);
	}

	o.AlbedoA = float4(alb, 1.f);
	o.NormalW = float4(N, 0.f);
	o.PosWorld = float4(pin.PosW, 1.f);

	float roughness = RoughnessFromNs(gMatNsFallback) * gMatRoughness;
	float metallic = gMatMetallic;
	float ao = 1.f;
	if (gHasRmTexture > 0.5f)
	{
		float4 arm = gRmMap.Sample(gSamLinearWrap, pin.Tex);
		const float metalMul = (gMatMetallic > 0.001) ? gMatMetallic : 1.0;

		// Fallback: rough-only map (R=roughness) вместо packed ARM
		if (arm.g < 0.05 && arm.r > 0.05)
		{
			ao = 1.0;
			roughness = arm.r * max(gMatRoughness, 0.001);
			metallic = 0.0;
		}
		else
		{
			ao = arm.r;
			roughness = arm.g * max(gMatRoughness, 0.001);
			metallic = arm.b * metalMul;
		}
	}
	o.PbrExtra = float4(saturate(roughness), saturate(metallic), saturate(ao), 1.f);
	return o;
}
