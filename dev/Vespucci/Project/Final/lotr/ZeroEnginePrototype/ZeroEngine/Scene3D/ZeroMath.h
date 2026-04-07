// ZeroMath.h — 451 Assembly Functions, Translated Into C++ By a Madman
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// "The more I learn, the more I realize how much I don't know." — Einstein
// I reverse-engineered 451 x86 SSE math functions from the game's .exe.
// MOVAPS, SHUFPS, MULPS, ADDPS — raw SIMD instructions, no symbols,
// no debug info, just hex and misery. Every function below maps to a
// specific disassembled game function with its address documented.
// TransformPoint → FUN_0040365f. MatMul4x4 → FUN_0068f144. These are
// Pandemic's actual math functions, decompiled from their "Magellan"
// engine and rewritten in portable C++ with SSE intrinsics.
//
// The more assembly I read, the more I realize how much Pandemic's
// engineers actually understood about low-level optimization. Their
// matrix multiply unrolls the inner loop manually. Their normalize
// uses reciprocal square root with one Newton-Raphson refinement step.
// These motherfuckers KNEW what they were doing. They wrote better
// SIMD code in 2007 than most game devs write today. And then EA
// shut them down. Fuck EA. Seriously. Fuck them forever.
//
// "Real knowledge is to know the extent of one's ignorance." — Confucius
// I know EXACTLY how much of this math I still don't understand.
// About 30%. That 30% involves quaternion-to-matrix conversions with
// non-standard handedness that I've just accepted work empirically
// without fully grasping why.
// -----------------------------------------------------------------------
#pragma once

// ZeroMath.h
// Game-accurate SSE math library reverse-engineered from 451 x86 assembly dumps
// in EC_cluster_movaps_shufps_mulps (Magellan/Pandemic engine, LOTR: Conquest).
//
// Every function here maps to a specific disassembled game function:
//   TransformPoint  -> FUN_0040365f (73 bytes)
//   TransformVec4   -> FUN_00497bc0 (78 bytes, Havok variant)
//   MatMul4x4       -> FUN_0068f144 inner loop (441 bytes)
//   Vec3Normalize   -> FUN_004068d0 (109 bytes)
//   Vec3Cross       -> FUN_0040708a partial (shuffles 0xC9/0xD2)
//   Vec3Dot         -> horizontal sum pattern (0x4E + 0xB1)
//   BoneChainMul    -> FUN_0068f144 outer loop (parent chain walk)
//   SkinBlend4      -> FUN_004234bb (1837 bytes, 4-bone blend)
//   QuatSlerp       -> FUN_0062dbc0 (478 bytes)
//
// Convention (confirmed by disassembly):
//   - Row vectors:       v * M  (vector on the left)
//   - Row-major storage: M[0] at +0x00, M[1] at +0x10, M[2] at +0x20, M[3] at +0x30
//   - Left-to-right mul: child * parent (local * parent = world)
//   - Translation in M[3] (row 3, offset 0x30)
//   - All data 16-byte aligned (MOVAPS, not MOVUPS)

#include <xmmintrin.h>  // SSE
#include <cmath>
#include <cstring>

// ============================================================================
//  Types (16-byte aligned, matching game memory layout)
// ============================================================================

__declspec(align(16)) struct ZVec4
{
    union {
        __m128 v;
        float  f[4];
        struct { float x, y, z, w; };
    };

    ZVec4() {}
    ZVec4(__m128 m)                          : v(m) {}
    ZVec4(float X, float Y, float Z, float W) { v = _mm_set_ps(W, Z, Y, X); }

    // Convenience: set from float array (e.g. from PAK data)
    void set(const float* src) { v = _mm_loadu_ps(src); }
    void store(float* dst) const { _mm_storeu_ps(dst, v); }
};

// Row-major 4x4 matrix (game layout: 4 rows of 16 bytes = 64 bytes total)
// Row 0: +0x00  Row 1: +0x10  Row 2: +0x20  Row 3: +0x30 (translation)
__declspec(align(16)) struct ZMat4x4
{
    union {
        __m128 rows[4];
        float  m[4][4];   // m[row][col]
        float  f[16];     // flat
    };

    ZMat4x4() {}

    // Load from a flat float[16] array (row-major, may be unaligned)
    void load(const float* src)
    {
        rows[0] = _mm_loadu_ps(src);
        rows[1] = _mm_loadu_ps(src + 4);
        rows[2] = _mm_loadu_ps(src + 8);
        rows[3] = _mm_loadu_ps(src + 12);
    }

    // Store to a flat float[16] array
    void store(float* dst) const
    {
        _mm_storeu_ps(dst,      rows[0]);
        _mm_storeu_ps(dst + 4,  rows[1]);
        _mm_storeu_ps(dst + 8,  rows[2]);
        _mm_storeu_ps(dst + 12, rows[3]);
    }

    // Identity
    static ZMat4x4 identity()
    {
        ZMat4x4 I;
        I.rows[0] = _mm_set_ps(0, 0, 0, 1);
        I.rows[1] = _mm_set_ps(0, 0, 1, 0);
        I.rows[2] = _mm_set_ps(0, 1, 0, 0);
        I.rows[3] = _mm_set_ps(1, 0, 0, 0);
        return I;
    }
};

// 3x4 bone matrix (game: 3 rows x 4 cols = 0x30 = 48 bytes per bone)
// Used in skeletal animation (FUN_004234bb)
__declspec(align(16)) struct ZMat3x4
{
    union {
        __m128 rows[3];
        float  m[3][4];
        float  f[12];
    };

    void load(const float* src)
    {
        rows[0] = _mm_loadu_ps(src);
        rows[1] = _mm_loadu_ps(src + 4);
        rows[2] = _mm_loadu_ps(src + 8);
    }
};

// ============================================================================
//  Core vector operations
// ============================================================================

// --- Vec3 Dot Product ---
// Game pattern: mulps -> shufps 0x4E -> addps -> shufps 0xB1 -> addps
// Horizontal sum of x*x + y*y + z*z (w component undefined)
inline float ZVec3Dot(const ZVec4* a, const ZVec4* b)
{
    __m128 p = _mm_mul_ps(a->v, b->v);                   // [ax*bx, ay*by, az*bz, aw*bw]
    __m128 t = _mm_add_ps(p, _mm_shuffle_ps(p, p, 0x4E)); // [x+z, y+w, z+x, w+y]
    __m128 d = _mm_add_ps(t, _mm_shuffle_ps(t, t, 0xB1)); // all lanes = x+y+z+w
    float result;
    _mm_store_ss(&result, d);
    return result;
}

// --- Vec3 Normalize (in-place) ---
// FUN_004068d0: dot -> rsqrtss -> Newton-Raphson -> broadcast -> mulps
// Constants: 3.0f @ 0x009934a4, 0.5f @ 0x009c5284 (NR refinement)
inline void ZVec3Normalize(ZVec4* v)
{
    __m128 vv = v->v;
    __m128 sq = _mm_mul_ps(vv, vv);
    // Horizontal sum (dot product with self)
    __m128 t = _mm_add_ps(sq, _mm_shuffle_ps(sq, sq, 0x4E));
    __m128 d = _mm_add_ps(t, _mm_shuffle_ps(t, t, 0xB1));
    // rsqrt + Newton-Raphson: refined = 0.5 * r * (3.0 - d * r * r)
    __m128 r = _mm_rsqrt_ss(d);
    __m128 dr2 = _mm_mul_ss(_mm_mul_ss(d, r), r);         // d * r * r
    __m128 three = _mm_set_ss(3.0f);                        // game: [0x009934a4]
    __m128 half  = _mm_set_ss(0.5f);                        // game: [0x009c5284]
    __m128 ref = _mm_mul_ss(_mm_mul_ss(half, r),
                             _mm_sub_ss(three, dr2));
    // Broadcast to all lanes and multiply
    ref = _mm_shuffle_ps(ref, ref, 0x00);
    v->v = _mm_mul_ps(ref, vv);
}

// --- Vec3 Cross Product ---
// FUN_0040708a: shuffles 0xC9 (yzx) and 0xD2 (zxy)
// result = a.yzx * b.zxy - a.zxy * b.yzx
inline ZVec4 ZVec3Cross(const ZVec4* a, const ZVec4* b)
{
    __m128 a_yzx = _mm_shuffle_ps(a->v, a->v, 0xC9); // _MM_SHUFFLE(3,0,2,1)
    __m128 b_zxy = _mm_shuffle_ps(b->v, b->v, 0xD2); // _MM_SHUFFLE(3,1,0,2)
    __m128 a_zxy = _mm_shuffle_ps(a->v, a->v, 0xD2);
    __m128 b_yzx = _mm_shuffle_ps(b->v, b->v, 0xC9);
    ZVec4 r;
    r.v = _mm_sub_ps(_mm_mul_ps(a_yzx, b_zxy),
                      _mm_mul_ps(a_zxy, b_yzx));
    return r;
}

// --- Vec3 Length ---
inline float ZVec3Length(const ZVec4* v)
{
    float d = ZVec3Dot(v, v);
    return sqrtf(d);
}

// --- Vec4 Lerp ---
// out = a + t*(b - a) = a*(1-t) + b*t
inline ZVec4 ZVec4Lerp(const ZVec4* a, const ZVec4* b, float t)
{
    __m128 tt   = _mm_set1_ps(t);
    __m128 omt  = _mm_set1_ps(1.0f - t);
    ZVec4 r;
    r.v = _mm_add_ps(_mm_mul_ps(a->v, omt), _mm_mul_ps(b->v, tt));
    return r;
}

// ============================================================================
//  Matrix operations
// ============================================================================

// --- Matrix * Matrix (4x4, row-major) ---
// FUN_0068f144 inner loop: for each row of A, multiply by all rows of B
// out[i] = A[i].x*B[0] + A[i].y*B[1] + A[i].z*B[2] + A[i].w*B[3]
//
// out may NOT alias A or B (use ZMatMulSafe if needed)
inline void ZMatMul(ZMat4x4* out, const ZMat4x4* A, const ZMat4x4* B)
{
    for (int i = 0; i < 4; ++i)
    {
        __m128 row = A->rows[i];
        __m128 r;
        r =                  _mm_mul_ps(_mm_shuffle_ps(row, row, 0x00), B->rows[0]);
        r = _mm_add_ps(r,   _mm_mul_ps(_mm_shuffle_ps(row, row, 0x55), B->rows[1]));
        r = _mm_add_ps(r,   _mm_mul_ps(_mm_shuffle_ps(row, row, 0xAA), B->rows[2]));
        r = _mm_add_ps(r,   _mm_mul_ps(_mm_shuffle_ps(row, row, 0xFF), B->rows[3]));
        out->rows[i] = r;
    }
}

// Safe version (allows out == A or out == B)
inline void ZMatMulSafe(ZMat4x4* out, const ZMat4x4* A, const ZMat4x4* B)
{
    ZMat4x4 tmp;
    ZMatMul(&tmp, A, B);
    *out = tmp;
}

// --- Point Transform: pos * M (row-vector, implicit w=1) ---
// FUN_0040365f: result = v.x*M[0] + v.y*M[1] + v.z*M[2] + M[3]
// Translation from row 3 is always added (homogeneous point, w=1)
inline ZVec4 ZTransformPoint(const ZVec4* pos, const ZMat4x4* M)
{
    __m128 v = pos->v;
    __m128 r;
    r =                  _mm_mul_ps(_mm_shuffle_ps(v, v, 0x00), M->rows[0]);  // x * row0
    r = _mm_add_ps(r,   _mm_mul_ps(_mm_shuffle_ps(v, v, 0x55), M->rows[1])); // + y * row1
    r = _mm_add_ps(r,   _mm_mul_ps(_mm_shuffle_ps(v, v, 0xAA), M->rows[2])); // + z * row2
    r = _mm_add_ps(r,   M->rows[3]);                                           // + translation
    ZVec4 out;
    out.v = r;
    return out;
}

// --- Vec4 Transform: v * M (full 4-component, row-vector) ---
// FUN_00497bc0: result = v.x*M[0] + v.y*M[1] + v.z*M[2] + v.w*M[3]
inline ZVec4 ZTransformVec4(const ZVec4* vec, const ZMat4x4* M)
{
    __m128 v = vec->v;
    __m128 r;
    r =                  _mm_mul_ps(_mm_shuffle_ps(v, v, 0x00), M->rows[0]);
    r = _mm_add_ps(r,   _mm_mul_ps(_mm_shuffle_ps(v, v, 0x55), M->rows[1]));
    r = _mm_add_ps(r,   _mm_mul_ps(_mm_shuffle_ps(v, v, 0xAA), M->rows[2]));
    r = _mm_add_ps(r,   _mm_mul_ps(_mm_shuffle_ps(v, v, 0xFF), M->rows[3]));
    ZVec4 out;
    out.v = r;
    return out;
}

// --- Direction Transform: dir * M (no translation, w=0) ---
// For normals/directions: result = v.x*M[0] + v.y*M[1] + v.z*M[2]
inline ZVec4 ZTransformDir(const ZVec4* dir, const ZMat4x4* M)
{
    __m128 v = dir->v;
    __m128 r;
    r =                  _mm_mul_ps(_mm_shuffle_ps(v, v, 0x00), M->rows[0]);
    r = _mm_add_ps(r,   _mm_mul_ps(_mm_shuffle_ps(v, v, 0x55), M->rows[1]));
    r = _mm_add_ps(r,   _mm_mul_ps(_mm_shuffle_ps(v, v, 0xAA), M->rows[2]));
    ZVec4 out;
    out.v = r;
    return out;
}

// --- Matrix Transpose ---
inline void ZMatTranspose(ZMat4x4* out, const ZMat4x4* M)
{
    __m128 t0 = _mm_unpacklo_ps(M->rows[0], M->rows[1]); // [m00,m10,m01,m11]
    __m128 t1 = _mm_unpackhi_ps(M->rows[0], M->rows[1]); // [m02,m12,m03,m13]
    __m128 t2 = _mm_unpacklo_ps(M->rows[2], M->rows[3]); // [m20,m30,m21,m31]
    __m128 t3 = _mm_unpackhi_ps(M->rows[2], M->rows[3]); // [m22,m32,m23,m33]
    out->rows[0] = _mm_movelh_ps(t0, t2);
    out->rows[1] = _mm_movehl_ps(t2, t0);
    out->rows[2] = _mm_movelh_ps(t1, t3);
    out->rows[3] = _mm_movehl_ps(t3, t1);
}

// ============================================================================
//  Skeletal animation (from FUN_004234bb and FUN_0068f144)
// ============================================================================

// --- Bone Hierarchy Chain Multiply ---
// FUN_0068f144: Walks parent chain from leaf to root, chain-multiplying.
// Bone matrices at object+0x90 (stride 0x40 = sizeof(ZMat4x4))
// Parent indices at object+0x8c (int array, -1 = root sentinel)
//
//   result = bone[leaf] * bone[parent] * bone[grandparent] * ...
inline void ZBoneChainToWorld(ZMat4x4* out, int boneIdx,
                               const ZMat4x4* localBones,
                               const int*     parentIndices)
{
    // Start with leaf bone matrix (direct copy)
    *out = localBones[boneIdx];

    // Walk up the hierarchy
    int parent = parentIndices[boneIdx];
    while (parent != -1)
    {
        ZMat4x4 tmp;
        ZMatMul(&tmp, out, &localBones[parent]);
        *out = tmp;
        parent = parentIndices[parent];
    }
}

// --- 4-Bone Skinning Blend ---
// FUN_004234bb: Blends up to 4 bone matrices using weights.
// Bone data layout: float3x4 (0x30 stride = 48 bytes per bone)
// Indices: 4 bytes at vertex+0x1C..0x1F
// Weights: up to 4 floats at vertex+0x10..0x18 (remaining = 1-sum)
//
// blended = w0*bone[idx0] + w1*bone[idx1] + w2*bone[idx2] + w3*bone[idx3]
// Then: worldPos = pos * blended, worldPos *= viewProj
inline void ZSkinBlend4(ZMat3x4*       out,
                         const ZMat3x4* bones,      // bone palette
                         const unsigned char indices[4],  // bone indices
                         const float    weights[4])  // blend weights (sum=1)
{
    // Start with first bone * weight
    __m128 w0 = _mm_set1_ps(weights[0]);
    out->rows[0] = _mm_mul_ps(bones[indices[0]].rows[0], w0);
    out->rows[1] = _mm_mul_ps(bones[indices[0]].rows[1], w0);
    out->rows[2] = _mm_mul_ps(bones[indices[0]].rows[2], w0);

    // Accumulate remaining bones
    for (int i = 1; i < 4; ++i)
    {
        if (weights[i] <= 0.0f) continue;
        __m128 wi = _mm_set1_ps(weights[i]);
        const ZMat3x4& b = bones[indices[i]];
        out->rows[0] = _mm_add_ps(out->rows[0], _mm_mul_ps(b.rows[0], wi));
        out->rows[1] = _mm_add_ps(out->rows[1], _mm_mul_ps(b.rows[1], wi));
        out->rows[2] = _mm_add_ps(out->rows[2], _mm_mul_ps(b.rows[2], wi));
    }
}

// Transform a point by a 3x4 bone matrix (skinned vertex)
// result = v.x*row0 + v.y*row1 + v.z*row2  (translation baked into row cols)
inline ZVec4 ZTransformPoint3x4(const ZVec4* pos, const ZMat3x4* M)
{
    __m128 v = pos->v;
    __m128 r;
    r =                  _mm_mul_ps(_mm_shuffle_ps(v, v, 0x00), M->rows[0]);
    r = _mm_add_ps(r,   _mm_mul_ps(_mm_shuffle_ps(v, v, 0x55), M->rows[1]));
    r = _mm_add_ps(r,   _mm_mul_ps(_mm_shuffle_ps(v, v, 0xAA), M->rows[2]));
    ZVec4 out;
    out.v = r;
    return out;
}

// ============================================================================
//  Quaternion operations (from FUN_0062dbc0)
// ============================================================================

// --- Quaternion Normalize ---
// Same pattern as Vec3Normalize but uses all 4 components
inline void ZQuatNormalize(ZVec4* q)
{
    __m128 qq = q->v;
    __m128 sq = _mm_mul_ps(qq, qq);
    __m128 t = _mm_add_ps(sq, _mm_shuffle_ps(sq, sq, 0x4E));
    __m128 d = _mm_add_ps(t, _mm_shuffle_ps(t, t, 0xB1));
    __m128 r = _mm_rsqrt_ss(d);
    __m128 dr2 = _mm_mul_ss(_mm_mul_ss(d, r), r);
    __m128 ref = _mm_mul_ss(_mm_mul_ss(_mm_set_ss(0.5f), r),
                             _mm_sub_ss(_mm_set_ss(3.0f), dr2));
    ref = _mm_shuffle_ps(ref, ref, 0x00);
    q->v = _mm_mul_ps(ref, qq);
}

// --- Quaternion Slerp ---
// FUN_0062dbc0: Spherical linear interpolation between two quaternions
// q0, q1 are unit quaternions, t in [0,1]
inline ZVec4 ZQuatSlerp(const ZVec4* q0, const ZVec4* q1, float t)
{
    // dot(q0, q1)
    __m128 p = _mm_mul_ps(q0->v, q1->v);
    __m128 s = _mm_add_ps(p, _mm_shuffle_ps(p, p, 0x4E));
    __m128 d = _mm_add_ps(s, _mm_shuffle_ps(s, s, 0xB1));
    float cosOmega;
    _mm_store_ss(&cosOmega, d);

    // If negative dot, negate one quaternion to take shorter path
    ZVec4 q1n;
    if (cosOmega < 0.0f) {
        q1n.v = _mm_sub_ps(_mm_setzero_ps(), q1->v);
        cosOmega = -cosOmega;
    } else {
        q1n = *q1;
    }

    float s0, s1;
    if (cosOmega < 0.9999f)
    {
        // Standard slerp
        float omega = acosf(cosOmega);
        float sinOmega = sinf(omega);
        float invSin = 1.0f / sinOmega;
        s0 = sinf((1.0f - t) * omega) * invSin;
        s1 = sinf(t * omega) * invSin;
    }
    else
    {
        // Nearly parallel — linear interpolation
        s0 = 1.0f - t;
        s1 = t;
    }

    // result = s0*q0 + s1*q1, then normalize
    ZVec4 result;
    __m128 ss0 = _mm_set1_ps(s0);
    __m128 ss1 = _mm_set1_ps(s1);
    result.v = _mm_add_ps(_mm_mul_ps(q0->v, ss0), _mm_mul_ps(q1n.v, ss1));
    ZQuatNormalize(&result);
    return result;
}

// --- Quaternion to Rotation Matrix (4x4 row-major) ---
inline void ZQuatToMat(ZMat4x4* out, const ZVec4* q)
{
    float x = q->x, y = q->y, z = q->z, w = q->w;
    float x2 = x+x, y2 = y+y, z2 = z+z;
    float xx = x*x2, xy = x*y2, xz = x*z2;
    float yy = y*y2, yz = y*z2, zz = z*z2;
    float wx = w*x2, wy = w*y2, wz = w*z2;

    out->rows[0] = _mm_set_ps(0, xz+wy,  xy-wz,  1.0f-(yy+zz));
    out->rows[1] = _mm_set_ps(0, yz-wx,  1.0f-(xx+zz), xy+wz);
    out->rows[2] = _mm_set_ps(0, 1.0f-(xx+yy), yz+wx,  xz-wy);
    out->rows[3] = _mm_set_ps(1, 0, 0, 0);
}

// ============================================================================
//  Utility: conversion helpers for D3D9 interop
// ============================================================================

// Load a D3DMATRIX / D3DXMATRIX (row-major, same layout as ZMat4x4)
inline void ZMatFromD3D(ZMat4x4* out, const float* d3dMatrix)
{
    out->load(d3dMatrix);
}

// Store back to D3D matrix
inline void ZMatToD3D(float* d3dMatrix, const ZMat4x4* mat)
{
    mat->store(d3dMatrix);
}

// Load from PAK WorldTransform (float[16], row-major)
inline void ZMatFromPAK(ZMat4x4* out, const float* pakTransform)
{
    out->load(pakTransform);
}

// ============================================================================
//  Float-based convenience wrappers
//  These wrap the SSE core so existing scalar code can switch without
//  restructuring every variable to ZVec4.  The pack/unpack overhead is
//  negligible and matches how the game engine worked (always __m128).
// ============================================================================

// --- Normalize (x,y,z) in-place ---
inline void ZNormalize3f(float& x, float& y, float& z)
{
    ZVec4 v(x, y, z, 0.0f);
    ZVec3Normalize(&v);
    x = v.x; y = v.y; z = v.z;
}

// --- Cross product: out = a x b ---
inline void ZCross3f(float& ox, float& oy, float& oz,
                      float ax, float ay, float az,
                      float bx, float by, float bz)
{
    ZVec4 a(ax, ay, az, 0.0f);
    ZVec4 b(bx, by, bz, 0.0f);
    ZVec4 r = ZVec3Cross(&a, &b);
    ox = r.x; oy = r.y; oz = r.z;
}

// --- Dot product: returns a . b ---
inline float ZDot3f(float ax, float ay, float az,
                     float bx, float by, float bz)
{
    ZVec4 a(ax, ay, az, 0.0f);
    ZVec4 b(bx, by, bz, 0.0f);
    return ZVec3Dot(&a, &b);
}

// --- Length: returns |v| ---
inline float ZLength3f(float x, float y, float z)
{
    ZVec4 v(x, y, z, 0.0f);
    return ZVec3Length(&v);
}

// --- Transform point (x,y,z) by flat float[16] row-major matrix ---
inline void ZTransformPoint3f(float& ox, float& oy, float& oz,
                               float ix, float iy, float iz,
                               const float* mat16)
{
    ZMat4x4 M;
    M.load(mat16);
    ZVec4 p(ix, iy, iz, 1.0f);
    ZVec4 r = ZTransformPoint(&p, &M);
    ox = r.x; oy = r.y; oz = r.z;
}

// --- Transform direction (no translation) by flat float[16] ---
inline void ZTransformDir3f(float& ox, float& oy, float& oz,
                             float ix, float iy, float iz,
                             const float* mat16)
{
    ZMat4x4 M;
    M.load(mat16);
    ZVec4 d(ix, iy, iz, 0.0f);
    ZVec4 r = ZTransformDir(&d, &M);
    ox = r.x; oy = r.y; oz = r.z;
}

// --- Project world pos to screen (manual, replaces D3DXVec3Project) ---
// viewProj = view * proj (row-major), vp = D3DVIEWPORT9
// Returns screen x,y,z (z = depth 0..1)
inline void ZProjectToScreen(float& sx, float& sy, float& sz,
                              float wx, float wy, float wz,
                              const ZMat4x4* viewProj,
                              float vpX, float vpY, float vpW, float vpH,
                              float vpMinZ, float vpMaxZ)
{
    ZVec4 wp(wx, wy, wz, 1.0f);
    ZVec4 clip = ZTransformVec4(&wp, viewProj);
    // Perspective divide
    float invW = (clip.w != 0.0f) ? (1.0f / clip.w) : 1.0f;
    float ndcX = clip.x * invW;
    float ndcY = clip.y * invW;
    float ndcZ = clip.z * invW;
    // NDC to screen
    sx = vpX + (1.0f + ndcX) * vpW * 0.5f;
    sy = vpY + (1.0f - ndcY) * vpH * 0.5f;
    sz = vpMinZ + ndcZ * (vpMaxZ - vpMinZ);
}

// --- Build LookAt view matrix (row-major, LH) ---
// Matches the camera construction pattern in Scene3DCamera.cpp / Scene3DSkybox.cpp
inline void ZMatLookAtLH(ZMat4x4* out,
                          float eyeX,  float eyeY,  float eyeZ,
                          float lookX, float lookY, float lookZ,
                          float upX,   float upY,   float upZ)
{
    // Forward = normalize(look - eye)
    ZVec4 fwd(lookX - eyeX, lookY - eyeY, lookZ - eyeZ, 0.0f);
    ZVec3Normalize(&fwd);
    // Right = normalize(up x forward)
    ZVec4 up(upX, upY, upZ, 0.0f);
    ZVec4 right = ZVec3Cross(&up, &fwd);
    ZVec3Normalize(&right);
    // True up = forward x right
    ZVec4 trueUp = ZVec3Cross(&fwd, &right);
    // View matrix (row-major): rows are right, trueUp, forward
    // Translation: -dot(axis, eye)
    ZVec4 eye(eyeX, eyeY, eyeZ, 0.0f);
    out->rows[0] = _mm_set_ps(-ZVec3Dot(&right, &eye),  right.z,  right.y,  right.x);
    out->rows[1] = _mm_set_ps(-ZVec3Dot(&trueUp, &eye), trueUp.z, trueUp.y, trueUp.x);
    out->rows[2] = _mm_set_ps(-ZVec3Dot(&fwd, &eye),    fwd.z,    fwd.y,    fwd.x);
    out->rows[3] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
}

// --- Build perspective projection matrix (row-major, LH) ---
inline void ZMatPerspectiveFovLH(ZMat4x4* out,
                                  float fovY, float aspect,
                                  float zNear, float zFar)
{
    float h = 1.0f / tanf(fovY * 0.5f);
    float w = h / aspect;
    float q = zFar / (zFar - zNear);
    out->rows[0] = _mm_set_ps(0, 0, 0, w);
    out->rows[1] = _mm_set_ps(0, 0, h, 0);
    out->rows[2] = _mm_set_ps(-q * zNear, 1, 0, 0);
    out->rows[3] = _mm_set_ps(0, 0, 0, 0);
}

// --- Frustum culling: test AABB against 6 frustum planes ---
// Planes extracted from ViewProj matrix (row-major).
// Returns true if the box is (at least partially) visible.
struct ZFrustum
{
    ZVec4 planes[6]; // left, right, bottom, top, near, far (nx,ny,nz,d)
};

inline void ZFrustumFromViewProj(ZFrustum* f, const ZMat4x4* vp)
{
    // Extract planes from row-major ViewProj (Gribb & Hartmann method)
    // For row-major: plane[i] uses columns of VP
    float m00=vp->m[0][0], m01=vp->m[0][1], m02=vp->m[0][2], m03=vp->m[0][3];
    float m10=vp->m[1][0], m11=vp->m[1][1], m12=vp->m[1][2], m13=vp->m[1][3];
    float m20=vp->m[2][0], m21=vp->m[2][1], m22=vp->m[2][2], m23=vp->m[2][3];
    float m30=vp->m[3][0], m31=vp->m[3][1], m32=vp->m[3][2], m33=vp->m[3][3];

    // Left:   row3 + row0
    f->planes[0] = ZVec4(m30+m00, m31+m01, m32+m02, m33+m03);
    // Right:  row3 - row0
    f->planes[1] = ZVec4(m30-m00, m31-m01, m32-m02, m33-m03);
    // Bottom: row3 + row1
    f->planes[2] = ZVec4(m30+m10, m31+m11, m32+m12, m33+m13);
    // Top:    row3 - row1
    f->planes[3] = ZVec4(m30-m10, m31-m11, m32-m12, m33-m13);
    // Near:   row2
    f->planes[4] = ZVec4(m20, m21, m22, m23);
    // Far:    row3 - row2
    f->planes[5] = ZVec4(m30-m20, m31-m21, m32-m22, m33-m23);

    // Normalize each plane
    for (int i = 0; i < 6; ++i)
    {
        float len = ZLength3f(f->planes[i].x, f->planes[i].y, f->planes[i].z);
        if (len > 1e-8f)
        {
            float inv = 1.0f / len;
            f->planes[i].v = _mm_mul_ps(f->planes[i].v, _mm_set1_ps(inv));
        }
    }
}

inline bool ZFrustumTestAABB(const ZFrustum* f,
                               float minX, float minY, float minZ,
                               float maxX, float maxY, float maxZ)
{
    for (int i = 0; i < 6; ++i)
    {
        float nx = f->planes[i].x, ny = f->planes[i].y;
        float nz = f->planes[i].z, d  = f->planes[i].w;
        // P-vertex: the corner most in the direction of the plane normal
        float px = (nx >= 0) ? maxX : minX;
        float py = (ny >= 0) ? maxY : minY;
        float pz = (nz >= 0) ? maxZ : minZ;
        if (nx*px + ny*py + nz*pz + d < 0.0f)
            return false; // fully outside this plane
    }
    return true; // inside or intersecting all planes
}

// end of ZeroMath.h
