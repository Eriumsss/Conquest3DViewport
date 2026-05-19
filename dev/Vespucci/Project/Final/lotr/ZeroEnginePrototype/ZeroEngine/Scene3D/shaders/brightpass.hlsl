// Bright pass filter for bloom extraction
// Hot-reloadable: press F12 in viewport to recompile
// c0 = (threshold, 0, 0, 0)

sampler2D g_Scene : register(s0);
float4 g_Params : register(c0);

float4 main(float2 uv : TEXCOORD0) : COLOR {
    float4 c = tex2D(g_Scene, uv);
    float lum = dot(c.rgb, float3(0.2126, 0.7152, 0.0722));
    float bloom = max(0, lum - g_Params.x);
    return float4(c.rgb * (bloom / (lum + 0.001)), 1.0);
}
