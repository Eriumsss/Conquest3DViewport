// ============================================================================
// neural_gl_shaders.h -- GLSL 330 Shaders for Biological Neural Rendering
// ============================================================================
//
// Four shaders stored as raw C string literals because this is 2008-era
// C++ with a 2024 OpenGL renderer duct-taped on. Pandemic's shaders were
// HLSL SM2.0 for D3D9. These are GLSL 330 core profile running on a
// completely separate GPU pipeline in a GLFW window. Two rendering APIs,
// one process. Absolute fucking madness.
//
// Soma VS: Instanced sphere rendering with procedural membrane wobble.
// 4 octaves of sin() displacement modulated by voltage and phase.
// Higher voltage = more wobble = neuron looks alive and angry.
//
// Soma FS: Voltage-to-color ramp (deep blue -> cyan -> green -> yellow
// -> orange -> white), sub-surface scattering approximation via wrap
// lighting, Fresnel rim glow that intensifies with activity, and
// bioluminescence boost from Hebbian learning. The neurons GLOW.
//
// Edge VS: Takes a unit tube mesh (Z=0 to Z=1) and orients it along
// the source-to-destination vector using a constructed basis matrix.
// Tubes taper from source to destination. Weight controls thickness.
//
// Edge FS: Purple-to-lavender based on activity, same wrap lighting,
// alpha transparency so you can see through the synapse forest.
// Pulse glow placeholder for traveling action potentials.
//
// None of this has anything to do with Lord of the Rings. Sauron
// doesn't have neurons. But the engine does now.
// ============================================================================
#pragma once

// ============================================================
// SOMA VERTEX SHADER -- instanced spheres with membrane wobble
// ============================================================
static const char* g_somaInstVS = R"(
#version 330 core
// Per-vertex (unit sphere mesh)
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNorm;
layout(location=2) in vec2 aUV;

// Per-instance (one per neuron, divisor=1)
layout(location=3) in vec4 iPos;       // xyz=world pos, w=radius
layout(location=4) in vec4 iState;     // x=voltage, y=phase, z=hebbianGlow, w=externalLight
layout(location=5) in vec4 iExtra;     // x=metabolicLoad, y=flags(as float), z=pad, w=pad

uniform mat4 uVP;       // view * projection
uniform vec3 uCamPos;

out vec3 vNorm;
out vec3 vWorldPos;
out float vVoltage;
out float vGlow;

void main() {
    float radius = iPos.w;
    float voltage = iState.x;
    float phase = iState.y;

    // Membrane wobble displacement
    float seed = aPos.x*3.7 + aPos.y*5.3 + aPos.z*2.1;
    float wobbleAmp = 0.03 + voltage * 0.08;
    float disp = sin(seed + phase)*0.50
               + sin(seed*2.7 + phase*1.6)*0.30
               + sin(seed*5.1 + phase*2.3)*0.15
               + sin(seed*9.8 + phase*3.1)*0.05;
    vec3 displaced = aPos + aNorm * disp * wobbleAmp;

    // Scale by radius and translate to world position
    vec3 worldPos = displaced * radius + iPos.xyz;

    gl_Position = uVP * vec4(worldPos, 1.0);
    vNorm = aNorm; // unit sphere normals are already correct
    vWorldPos = worldPos;
    vVoltage = voltage;
    vGlow = max(iState.z, iState.w); // max of hebbian and bioluminescence
}
)";

// ============================================================
// SOMA FRAGMENT SHADER -- voltage-to-color + wrap lighting + rim
// ============================================================
static const char* g_somaInstFS = R"(
#version 330 core
uniform vec3 uLightDir;
uniform vec3 uCamPos;

in vec3 vNorm;
in vec3 vWorldPos;
in float vVoltage;
in float vGlow;
out vec4 FragColor;

vec3 VoltageToColor(float v) {
    vec3 c0=vec3(0.04,0.04,0.45), c1=vec3(0.00,0.55,0.75);
    vec3 c2=vec3(0.10,0.75,0.20), c3=vec3(1.00,0.90,0.05);
    vec3 c4=vec3(1.00,0.45,0.00), c5=vec3(1.00,1.00,1.00);
    if (v<0.20) return mix(c0,c1,v/0.20);
    if (v<0.40) return mix(c1,c2,(v-0.20)/0.20);
    if (v<0.50) return mix(c2,c3,(v-0.40)/0.10);
    if (v<0.70) return mix(c3,c4,(v-0.50)/0.20);
    return mix(c4,c5,(v-0.70)/0.30);
}

void main() {
    vec3 N = normalize(vNorm);
    vec3 L = normalize(-uLightDir);
    vec3 V = normalize(uCamPos - vWorldPos);

    vec3 base = VoltageToColor(clamp(vVoltage, 0.0, 1.0));

    // Wrap lighting (SSS approximation)
    float wrap = 0.35;
    float NdotL = clamp((dot(N,L) + wrap) / (1.0 + wrap), 0.0, 1.0);
    vec3 scatter = mix(vec3(0.75,0.25,0.15), vec3(1), clamp(dot(N,L)+0.3, 0.0, 1.0));

    // Fresnel rim
    float NdotV = clamp(dot(N,V), 0.0, 1.0);
    float fresnel = pow(1.0 - NdotV, 2.5);
    float rimInt = mix(0.25, 2.0, clamp(vVoltage, 0.0, 1.0));
    vec3 rimCol = mix(base, vec3(1), vVoltage * 0.6);
    vec3 rim = rimCol * fresnel * rimInt;

    // Bioluminescence boost
    vec3 glowAdd = base * vGlow * 0.3;

    vec3 ambient = base * vec3(0.12, 0.10, 0.18);
    vec3 result = ambient + base * NdotL * scatter + rim + glowAdd;
    FragColor = vec4(clamp(result, 0.0, 1.0), 1.0);
}
)";

// ============================================================
// EDGE VERTEX SHADER -- instanced tubes oriented along src->dst
// ============================================================
static const char* g_edgeInstVS = R"(
#version 330 core
// Per-vertex (unit tube mesh along Z axis, length 1)
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNorm;
layout(location=2) in vec2 aUV;

// Per-instance
layout(location=3) in vec3 iSrc;       // source position
layout(location=4) in vec3 iDst;       // destination position
layout(location=5) in vec4 iEdgeData;  // x=activity, y=weight, z=flags(float), w=pulse0

uniform mat4 uVP;

out vec3 vNorm;
out vec3 vWorldPos;
out float vActivity;
out float vPulseProgress;

void main() {
    vec3 dir = iDst - iSrc;
    float len = length(dir);
    if (len < 0.001) { gl_Position = vec4(0); return; }
    vec3 fwd = dir / len;

    // Build orientation basis
    vec3 up = abs(fwd.y) < 0.99 ? vec3(0,1,0) : vec3(1,0,0);
    vec3 right = normalize(cross(up, fwd));
    vec3 realUp = cross(fwd, right);

    // Scale tube: radius from weight, length from distance
    float baseRadius = 0.04 + iEdgeData.y * 0.06;
    float t = aPos.z; // 0=src end, 1=dst end
    float taperRadius = baseRadius * mix(1.0, 0.6, t); // taper toward dst

    // Transform unit tube to world
    vec3 localPos = right * aPos.x * taperRadius + realUp * aPos.y * taperRadius + fwd * t * len;
    vec3 worldPos = iSrc + localPos;

    // Transform normal
    vec3 worldNorm = right * aNorm.x + realUp * aNorm.y + fwd * aNorm.z;

    gl_Position = uVP * vec4(worldPos, 1.0);
    vNorm = worldNorm;
    vWorldPos = worldPos;
    vActivity = iEdgeData.x;
    vPulseProgress = iEdgeData.w;
}
)";

// ============================================================
// EDGE FRAGMENT SHADER -- activity-driven color with pulse glow
// ============================================================
static const char* g_edgeInstFS = R"(
#version 330 core
uniform vec3 uLightDir;
uniform vec3 uCamPos;

in vec3 vNorm;
in vec3 vWorldPos;
in float vActivity;
in float vPulseProgress;
out vec4 FragColor;

void main() {
    vec3 N = normalize(vNorm);
    vec3 L = normalize(-uLightDir);
    vec3 V = normalize(uCamPos - vWorldPos);

    // Excitatory color: purple -> bright lavender with activity
    vec3 base = mix(vec3(0.28,0.08,0.50), vec3(0.80,0.70,1.00), clamp(vActivity,0.0,1.0));

    // Wrap lighting
    float wrap = 0.25;
    float NdotL = clamp((dot(N,L) + wrap) / (1.0 + wrap), 0.0, 1.0);
    float NdotV = clamp(dot(N,V), 0.0, 1.0);
    float fresnel = pow(1.0 - NdotV, 2.0);
    vec3 rim = base * fresnel * (0.4 + vActivity * 1.2);
    vec3 result = base * 0.07 + base * NdotL * 0.85 + rim;

    // Pulse glow -- bright spot traveling along the tube
    if (vPulseProgress >= 0.0 && vPulseProgress <= 1.0) {
        // vWorldPos projected onto src->dst gives position along tube (approx via gl_Position)
        // For now: simple activity-based glow
        result += vec3(0.3, 0.2, 0.5) * vActivity;
    }

    float alpha = 0.35 + vActivity * 0.55;
    FragColor = vec4(clamp(result, 0.0, 1.0), alpha);
}
)";
