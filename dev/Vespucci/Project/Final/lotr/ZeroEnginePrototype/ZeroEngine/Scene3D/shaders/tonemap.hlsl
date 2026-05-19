// Tone mapping. Reinhard with key-as-target middle grey.
// Hot-reloadable: press F12 to recompile.
//
// HISTORICAL HORSESHIT WE INHERITED:
//
// Whoever wrote the original version of this file did NOT understand
// what Reinhard is. They wrote `c *= g_ToneParams.x * 1.15`, where
// g_ToneParams.x is the AtmosphereSetting "key" field. The "key" in
// Reinhard auto-exposure is the TARGET MIDDLE-GREY LUMINANCE (~0.18).
// You DIVIDE the average scene luminance by it to PRODUCE an exposure
// multiplier. You do NOT multiply the scene by the goddamn target.
// That is a textbook rookie fuckup, the kind of thing that gets you
// laughed out of a graphics interview, and somehow it shipped.
//
// Result: any level with key < 0.18 (Helm's Deep ~0.15, Mordor moody
// ~0.10, basically every cool-atmosphere level the game has) got the
// scene multiplied by 0.17x and crushed straight into black. Whatever
// color survived the crushing was the inscatter sky tint, which on
// these levels happens to be blue. That is the FAMOUS "haunted
// aquarium" frame the LevelScene.cpp pipeline-off log block has been
// screaming about for weeks. Hours fucking lost staring at a dark
// blue swamp before the math was actually traced.
//
// THE FIX (Option B from the brief, the cheap-and-correct one):
//
// Translate key into exposure by treating 0.18 as canonical middle
// grey. So key=0.18 -> 1.0x exposure. key=0.15 -> 0.83x. key=0.30 ->
// 1.67x. Same shape as a real auto-exposure pass minus the extra
// downsample-to-1x1 step. No luminance bias toward blue inscatter.
// Levels look like the artists meant. Eat it.

sampler2D g_Scene : register(s0);
sampler2D g_Bloom : register(s1);
float4 g_ToneParams  : register(c0); // x=key, y=bloomStr, z=whitepoint
float4 g_Brightness  : register(c1); // unused for now
float4 g_Gamma       : register(c2); // per-channel gamma
float4 g_ContrastExp : register(c3); // unused

float4 main(float2 uv : TEXCOORD0) : COLOR {
    float3 scene = tex2D(g_Scene, uv).rgb;
    float3 bloom = tex2D(g_Bloom, uv).rgb;
    float bloomStr = (g_ToneParams.y > 0.0) ? g_ToneParams.y : 0.2;
    float3 c = scene + bloom * bloomStr;

    // Key-as-target exposure. Clamp the floor so an absent/zero key
    // doesn't black-frame us; ceiling matches game scene-key range.
    float key = clamp(g_ToneParams.x, 0.04, 1.20);
    float exposure = key / 0.18;
    c *= exposure;

    // Extended Reinhard with whitepoint
    float wp2 = max(g_ToneParams.z * g_ToneParams.z, 0.0001);
    c = c * (1.0 + c / wp2) / (1.0 + c);

    // Gamma
    c = pow(max(c, 0.001), 1.0 / 1.05);

    return float4(c, 1.0);
}
