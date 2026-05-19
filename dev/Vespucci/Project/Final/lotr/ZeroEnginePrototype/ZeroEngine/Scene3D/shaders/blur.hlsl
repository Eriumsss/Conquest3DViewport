// Gaussian blur (separable, H or V pass)
// Hot-reloadable: press F12 in viewport to recompile
// c0 = (dx, dy, 0, 0) — direction: (texelW, 0) for H pass, (0, texelH) for V pass

sampler2D g_Tex : register(s0);
float4 g_Dir : register(c0);

float4 main(float2 uv : TEXCOORD0) : COLOR {
    float2 d = g_Dir.xy;
    float3 c = tex2D(g_Tex, uv).rgb * 0.2270270;
    c += tex2D(g_Tex, uv + d * 1.3846153).rgb * 0.3162162;
    c += tex2D(g_Tex, uv - d * 1.3846153).rgb * 0.3162162;
    c += tex2D(g_Tex, uv + d * 3.2307692).rgb * 0.0702702;
    c += tex2D(g_Tex, uv - d * 3.2307692).rgb * 0.0702702;
    return float4(c, 1.0);
}
