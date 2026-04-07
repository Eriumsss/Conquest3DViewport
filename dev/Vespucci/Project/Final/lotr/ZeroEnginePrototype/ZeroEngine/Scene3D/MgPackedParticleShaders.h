// MgPackedParticleShaders.h — Pandemic's Actual Fucking Compiled Shaders
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// This loads the REAL D3D9 vertex and pixel shaders that shipped on the
// Conquest retail disc. Not reimplementations. Not approximations. The
// ACTUAL compiled shader bytecode from Shaders_PC_nvidia.bin, ripped
// straight from GameFiles and fed directly to IDirect3DDevice9::
// CreateVertexShader() and CreatePixelShader(). Pandemic's GPU code
// running on YOUR GPU in 2026. The shader instructions are frozen in
// time — ps_2_0 and vs_2_0 programs compiled by Pandemic's build
// pipeline in late 2008, never modified, never updated, perfectly
// preserved like a fucking insect in amber.
//
// The Mg prefix is "Magellan" — Pandemic's internal engine name.
// We found it stamped across dozens of class names in the .exe's RTTI.
// MgRenderer, MgTexture, MgShader, MgParticle — all Magellan subsystems.
// This class resurrects MgShader specifically for particle rendering
// so our effects look IDENTICAL to the shipped game.
//
// "What doesn't kill me makes me stronger." — Nietzsche
// Debugging shader bytecode without source HLSL, without PDB symbols,
// without even knowing which fucking constant registers map to which
// uniforms — that nearly killed me. But it didn't. And now I understand
// every goddamn register in Pandemic's particle shaders. c0-c3 is the
// WorldViewProj matrix. c4 is the camera position. c5 is time. I AM
// the shader documentation now. I AM the missing fucking manual.
// -----------------------------------------------------------------------
#ifndef MG_PACKED_PARTICLE_SHADERS_H
#define MG_PACKED_PARTICLE_SHADERS_H

struct IDirect3DDevice9;
struct IDirect3DVertexDeclaration9;
struct IDirect3DVertexShader9;
struct IDirect3DPixelShader9;

class hkgDisplayContext;
class hkgCamera;
class hkVector4;
struct ParticleEmitter;

// Minimal shader runner for a small subset of the game's packed D3D9 shaders
// (GameFiles/lotrcparser/Shaders_PC_nvidia). Currently focused on particles.
class MgPackedParticleShaders
{
public:
    MgPackedParticleShaders();
    ~MgPackedParticleShaders();

    void release();
    void setShaderRoot(const char* root);
    const char* getShaderRoot() const { return m_shaderRoot; }
    bool ensureResources(IDirect3DDevice9* device);

    // Renders a billboard particle emitter using:
    //  VS: Mg_VP_Particles_Billboard_Vd_A
    //  PS: Mg_FP_Particles_A_Vd
    bool renderBillboard_A_Vd(
        IDirect3DDevice9* device,
        hkgDisplayContext* context,
        hkgCamera* camera,
        const hkVector4& cameraPos,
        const hkVector4& cameraTarget,
        ParticleEmitter* emitter);

private:
    MgPackedParticleShaders(const MgPackedParticleShaders&);
    MgPackedParticleShaders& operator=(const MgPackedParticleShaders&);

    IDirect3DVertexDeclaration9* m_decl;
    IDirect3DVertexShader9* m_vsBillboardVdA;
    IDirect3DPixelShader9* m_psAVd;
    char m_shaderRoot[260];
};

#endif // MG_PACKED_PARTICLE_SHADERS_H
