// Ronin @feature 13/09/2026 DX9: SSAO. Screen-space quad for the SSAO passes and their debug views. The C++ side
// supplies clip-space XY (half-pixel offset already applied) and the target UV.
//
// Compile with: fxc /T vs_3_0 /Fo ScreenQuad.vso ScreenQuad_vs.hlsl

struct VS_INPUT
{
    float3 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

struct VS_OUTPUT
{
    float4 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

VS_OUTPUT main(VS_INPUT input)
{
    VS_OUTPUT o;
    o.pos = float4(input.pos.xy, 0.0f, 1.0f);
    o.uv  = input.uv;
    return o;
}
