// GpuFilterCaps.h - Ask the GPU what it can actually filter. ONCE.
// ─────────────────────────────────────────────────────────────────
// Written by: Eriumsss
//
// Story: viewport ships, looks gorgeous on Eriumsss's NVIDIA box,
// gets handed to a tester running Intel UHD Graphics, every texture
// in Helm's Deep turns into wet cardboard. Same exe, same shader
// bin, same level. Different GPU vendor, different result.
//
// Root cause: the renderer was happily slamming D3DTEXF_ANISOTROPIC
// onto every sampler with D3DSAMP_MAXANISOTROPY=8 without ever
// asking the device whether it could actually DO either of those
// things. NVIDIA drivers ship with global panel defaults set to
// "Quality" so unspecified or unsupported filter modes silently
// upgrade to whatever the driver thinks looks nicest. Intel drivers
// don't. They give you exactly what the D3D spec says to give you,
// which for an unsupported filter mode is D3DERR_INVALIDCALL and a
// sampler that stays at POINT. Hence: wet cardboard.
//
// This header asks IDirect3DDevice9::GetDeviceCaps once, looks at
// D3DPTFILTERCAPS_MINFANISOTROPIC + MAGFANISOTROPIC, and reports
// back the SAFEST filter mode the device actually supports plus the
// clamped MaxAnisotropy value. Function-local static caches the
// result so the cap query runs exactly once per process lifetime.
// Both Scene3DRenderer (the renderer setup pass) and LevelScene
// (the main mesh draw) call this in their texture state blocks.
//
// If the device's caps query fails (it shouldn't, but if Havok
// hands us a stub device or some such horseshit), we fall back to
// the safest possible state: LINEAR filter, MaxAnisotropy=1. That
// at least won't be POINT-sampled. Slightly soft beats jagged hell.

#ifndef GPU_FILTER_CAPS_H
#define GPU_FILTER_CAPS_H

#include <d3d9.h>
#include <string.h>

struct GpuFilterCaps {
    DWORD anisoFilter;     // D3DTEXF_ANISOTROPIC if the device actually supports
                           // BOTH min and mag aniso, else D3DTEXF_LINEAR.
    DWORD maxAnisotropy;   // Clamped to [1, min(8, caps.MaxAnisotropy)]. Eight is
                           // the cap because anything above that is invisible to
                           // a human and just costs fill rate.
    bool  queried;         // True after a successful GetDeviceCaps call.
};

inline const GpuFilterCaps& GetGpuFilterCaps(IDirect3DDevice9* dev) {
    // Default before the first query lands: safe, dumb, slightly soft, but
    // not POINT. Beats getting eaten alive by sampler nasal demons.
    static GpuFilterCaps s_caps = { D3DTEXF_LINEAR, 1, false };
    if (s_caps.queried || !dev) return s_caps;

    D3DCAPS9 caps;
    memset(&caps, 0, sizeof(caps));
    if (FAILED(dev->GetDeviceCaps(&caps))) {
        // Device is wedged or fake. Leave the safe fallback in place.
        return s_caps;
    }

    DWORD tf = caps.TextureFilterCaps;
    bool minAniso = (tf & D3DPTFILTERCAPS_MINFANISOTROPIC) != 0;
    bool magAniso = (tf & D3DPTFILTERCAPS_MAGFANISOTROPIC) != 0;
    s_caps.anisoFilter = (minAniso && magAniso) ? D3DTEXF_ANISOTROPIC
                                                : D3DTEXF_LINEAR;

    DWORD maxA = caps.MaxAnisotropy;
    if (maxA < 1) maxA = 1;     // GPU reported zero, treat as 1.
    if (maxA > 8) maxA = 8;     // Anything more is invisible to humans.
    s_caps.maxAnisotropy = maxA;

    s_caps.queried = true;
    return s_caps;
}

#endif // GPU_FILTER_CAPS_H
