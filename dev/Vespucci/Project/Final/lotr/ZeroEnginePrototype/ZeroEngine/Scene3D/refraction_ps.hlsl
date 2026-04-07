struct PS_IN
{
    float2 t0 : TEXCOORD0; // normal/noise UV
    float2 t1 : TEXCOORD1; // screen UV
    float2 t2 : TEXCOORD2; // mask UV
    float4 c  : COLOR0;
};

sampler2D s0 : register(s0);
sampler2D s1 : register(s1);
sampler2D s2 : register(s2);

float4 c0 : register(c0);

float4 main(PS_IN i) : COLOR0
{
    float2 n = tex2D(s0, i.t0).xy;
    n = n * 2.0f - 1.0f;
    float2 uv = i.t1 + n * float2(c0.x, c0.y);
    float4 scene = tex2D(s1, uv);
    float4 mask = tex2D(s2, i.t2);
    scene.a = i.c.a * mask.a;
    return scene;
}
