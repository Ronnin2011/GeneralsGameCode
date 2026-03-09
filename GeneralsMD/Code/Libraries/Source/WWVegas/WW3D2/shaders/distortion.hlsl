// distortion.hlsl - Heat shimmer/smudge distortion pixel shader
// @feature Ronin 03/03/2026 Proper HLSL ps_3_0 distortion with procedural-friendly noise sampling.
// Replaces legacy ps_2_0 assembly version. Generates signed UV offsets from noise texture,
// modulated by per-vertex alpha for edge falloff.
//
// @bugfix Ronin 04/03/2026 Reduced distortion scale, use smoothed noise sampling, output
// scene color with alpha=1 to avoid FFP alpha blending darkening the result.
//
// Compile with: fxc /T ps_3_0 /Fo distortion.pso distortion.hlsl
//
// Registers (set by CPU):
//   c0 = (distortionStrength, 0, 0, 0)
//   c1 = (texClampX, texClampY, 0, 0) — valid scene UV bounds
//
// Samplers:
//   s0 = scene texture (render-to-texture backbuffer copy)
//   s1 = noise texture (procedurally generated, wrap + linear)
//
// Interpolators (from FFP vertex pipeline with VertexFormatXYZNDUV2):
//   TEXCOORD0 = screen-space UV for scene texture (uv1 in vertex buffer)
//   TEXCOORD1 = noise UV (uv2 in vertex buffer, scrolled on CPU each frame)
//   COLOR0.a  = per-vertex opacity (center vertex alpha, edges=0)

sampler2D sceneSampler : register(s0);
sampler2D noiseSampler : register(s1);

float4 g_Params : register(c0); // x = distortionStrength
float4 g_TexClamp : register(c1); // xy = max valid UV

struct PS_INPUT
{
    float2 sceneUV : TEXCOORD0;
    float2 noiseUV : TEXCOORD1;
    float4 color : COLOR0;
};

float4 main(PS_INPUT input) : COLOR0
{
    // Sample noise — R and G contain independent Perlin noise in [0,1], convert to signed [-1,1]
    float2 noise = tex2D(noiseSampler, input.noiseUV).rg * 2.0 - 1.0;

    // Scale by distortion strength and per-vertex alpha (edge fade).
    // The alpha ramp from center (opaque) to edges (transparent) gives smooth falloff.
    float2 offset = noise * g_Params.x * input.color.a;

    // Offset scene UV and clamp to valid region
    float2 distortedUV = input.sceneUV + offset;
    distortedUV = clamp(distortedUV, float2(0, 0), g_TexClamp.xy);

    // Sample the scene at distorted position.
    float4 scene = tex2D(sceneSampler, distortedUV);

    // @bugfix Ronin 04/03/2026 Output alpha = vertex alpha so the _PresetAlphaShader
    // (SrcAlpha, InvSrcAlpha) blends the distorted region over the original backbuffer.
    // Where alpha=0 (edges), the original scene shows through untouched.
    // Where alpha=1 (center), the distorted scene fully replaces the original.
    scene.a = input.color.a;
    return scene;
}
