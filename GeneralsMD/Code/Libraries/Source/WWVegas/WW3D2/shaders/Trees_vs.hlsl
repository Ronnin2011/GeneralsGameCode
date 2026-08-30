// Ronin @feature 25/08/2026 DX9: §29i.4 stage 1 — port Trees.nvv (vs_1_1) to HLSL.
//
// Faithful port. Same constant registers, same maths, same outputs as the shipped
// Trees.vso, which disassembles byte-for-byte to Shaders/Trees.nvv:
//
//   mov  r2, v1.wwzw     ; (1, 1, baseZ, 1)
//   sub  r2, v0, r2      ; r2.z = height above the tree base, r2.w = 0
//   mov  a0.x, v1        ; sway index
//   mov  r0, c[a0.x+8]   ; c8 = noSway, c9..c18 = the MAX_SWAY_TYPES table
//   mad  r1, r2.zzzw, r0, v0
//   m4x4 oPos, r1, c4
//   mov  r2, v1.yyyw     ; (scale, scale, scale, 1)
//   mul  oD0, v2, r2
//   mov  oT0, v7
//   add  r1, v0, c32
//   mul  oT1, r1, c33
//
// WHY THE PORT EXISTS. The DX8 build bound inputs by REGISTER — D3DVSD_REG(1) put the
// packed data in v1 and D3DVSD_REG(2) the diffuse in v2. The DX9 port replaced that with
// a semantic D3DVERTEXELEMENT9[] that is created, stored in W3DShaderManager's map, and
// never bound; the draw runs on SetFVF(DX8_FVF_XYZNDUV1) instead. Under an FVF the SM1
// register mapping is fixed — NORMAL lands on v3 and COLOR0 on v5 — so v1 and v2 are
// unmapped. HLSL binds by SEMANTIC, which is what makes this correct again.
//
// NOT A NORMAL. The NORMAL slot carries packed per-tree data, written at
// W3DTreeBuffer.cpp:893-895 as nx = swayType, ny = 1 - darkening*pushAside, nz = base Z.
// Trees have no vertex normal at all.
//
// Compile with: fxc /T vs_3_0 /Fo Trees.vso Trees_vs.hlsl
//   Was vs_2_0 while this shipped with no pixel shader. Now paired with Trees_ps.hlsl
//   (ps_3_0), and D3D9 requires vs_3_0 and ps_3_0 to be used together.
//   NOTE: vs_1_1 is REJECTED by CreateVertexShader on this hardware (D3DERR_INVALIDCALL),
//   which is why the original Trees.vso never loaded and trees never swayed.

float4 g_WVP[4]      : register(c4);   // c4..c7, TRANSPOSED on upload (m4x4 semantics)
float4 g_Sway[11]    : register(c8);   // c8 = noSway, c9..c18 = MAX_SWAY_TYPES (10) entries
float4 g_ShroudOfs   : register(c32);  // (xoffset, yoffset, 0, 0)
float4 g_ShroudScale : register(c33);  // (1/(cellW*texW), 1/(cellH*texH), 1, 1)
float4 g_CloudParams : register(c34);  // y = scale, zw = offset. Same layout as rigid's c14.

struct VS_INPUT
{
    float4 pos     : POSITION;   // was v0
    float3 packed  : NORMAL;     // was v1: x = sway index, y = colour scale, z = base Z
    float4 diffuse : COLOR0;     // was v2
    float2 uv0     : TEXCOORD0;  // was v7
};

struct VS_OUTPUT
{
    float4 pos      : POSITION;
    float4 diffuse  : COLOR0;
    float2 uv0      : TEXCOORD0;  // tree texture
    float2 uv1      : TEXCOORD1;  // shroud, sampled from the UNSWAYED position
    float3 worldPos : TEXCOORD2;  // SWAYED world position, for the shadow receiver
    float2 uv2      : TEXCOORD3;  // terrain cloud projection
};

VS_OUTPUT main(VS_INPUT input)
{
    VS_OUTPUT o;

    // r2.z, then the mad. r2.w is 0, so the swayed w stays v0.w == 1.
    float  height = input.pos.z - input.packed.z;
    int    idx    = (int)(input.packed.x + 0.5f);   // mova rounds to nearest
    float4 sway   = g_Sway[idx];

    float4 swayed;
    swayed.xyz = input.pos.xyz + height * sway.xyz;
    swayed.w   = 1.0f;

    // m4x4 oPos, r1, c4 — four dp4s against consecutive registers. Written out rather
    // than declared float4x4 so no assumption about HLSL matrix packing is involved.
    o.pos.x = dot(swayed, g_WVP[0]);
    o.pos.y = dot(swayed, g_WVP[1]);
    o.pos.z = dot(swayed, g_WVP[2]);
    o.pos.w = dot(swayed, g_WVP[3]);

    // mul oD0, v2, v1.yyyw — rgb scaled by the darkening term, alpha passed through.
    // The alpha matters: the FFP modulates it against the texture and the result feeds
    // the alpha test at ref 0x60 GREATEREQUAL.
    o.diffuse = float4(input.diffuse.rgb * input.packed.y, input.diffuse.a);

    o.uv0 = input.uv0;
    o.uv1 = (input.pos.xy + g_ShroudOfs.xy) * g_ShroudScale.xy;

    // Tree vertices are already WORLD space — proven by the shroud UV above, which is built
    // from world-space offsets (c32/c33) and lands correctly.
    o.worldPos = swayed.xyz;

    // Same formula as RigidInstance.hlsl:132, so trees and rigid palms agree. UNSWAYED on
    // purpose — the cloud is a large soft field and sway would only add shimmer to it.
    o.uv2 = input.pos.xy * g_CloudParams.y + g_CloudParams.zw;

    return o;
}
