// Scene3DRendererInternal.h — The Forbidden Scrolls
// -----------------------------------------------------------------------
// Written by: Eriumsss
// Status: one eye open, the other one is twitching on its own schedule
//
// This is the INTERNAL header — the shit behind the curtain that nobody
// outside Scene3D is allowed to see. Matrix math, quaternion utilities,
// animation helpers, easing functions, bone lookup caches, SSE intrinsics
// for when the math gets too heavy for scalar... all the guts that make
// the renderer actually function.
//
// Included ONLY by the .cpp files that implement Scene3DRenderer across
// multiple translation units. Scene3DRenderer is so fucking massive that
// it's split across like 7 .cpp files (Renderer, Animation, BoneEditor,
// Camera, Material, Effects, Loader). Each one includes this header to
// share inline utilities. If you include this from outside Scene3D, the
// Havok 5.5 header chain will pull in 400+ transitive includes and your
// compile time will go from 8 seconds to 2 minutes. Don't do it. I did
// it once accidentally and the compiler used MANY GB's of RAM and my laptop
// started making sounds like a poor dying animal.
//
// NOT part of the public API. This is the private diary of a codebase
// that has seen too much. Pandemic's engineers had their own version of
// this — we found fragments in the .exe's RTTI data. They called their
// internal math "MgMath" (Manager? MiniGame? Math).
// Ours is less elegant but it
// compiles with the stolen SDK and that's all that matters anymore.
// -----------------------------------------------------------------------

#ifndef SCENE3D_RENDERER_INTERNAL_H
#define SCENE3D_RENDERER_INTERNAL_H

#include "Scene3DRenderer.h"
#include "AnimationCurve.h"
#include <vector>
#if defined(_MSC_VER)
#include <xmmintrin.h>
#endif
#include <string>
#include <math.h>
#include <Animation/Internal/Compression/hkaCompression.h>
#include <Common/Base/Math/Quaternion/hkQuaternion.h>
#include <Common/Base/Math/Matrix/hkRotation.h>
#include <Animation/Animation/Rig/hkaSkeleton.h>
#include <Animation/Animation/Rig/hkaPose.h>

// ---------------------------------------------------------------------------
// Shared free functions (defined once in Scene3DRenderer.cpp)
// ---------------------------------------------------------------------------

// Append a single line to renderer.log (opened on first call).
void RendererLog(const char* msg);

// Scan HKX directories for rigs matching a target skeleton (defined in Scene3DAnimation.cpp).
class hkaSkeleton;
void ScanForMatchingRigs(const hkaSkeleton* target);

// Initialize a JsonAnimInfo struct with defaults (defined in Scene3DAnimation.cpp).
struct JsonAnimInfo;
void InitJsonAnimInfo(JsonAnimInfo& info);

// Column-major 4×4 matrix multiply: out = a * b
// Pandemic stored ALL their transforms column-major because D3D9 and Havok
// both use column-major internally. Row-major people can fuck right off.
// This function does 64 multiply-adds. Unoptimized. I could use SSE but
// this isn't in any hot loop — it's for setup transforms. If you call
// this per-frame per-bone you deserve what happens to your framerate.
inline void MultiplyMatrix4(const float* a, const float* b, float* out)
{
    for (int col = 0; col < 4; col++)
    {
        for (int row = 0; row < 4; row++)
        {
            out[row + col * 4] =
                a[row + 0 * 4] * b[0 + col * 4] +
                a[row + 1 * 4] * b[1 + col * 4] +
                a[row + 2 * 4] * b[2 + col * 4] +
                a[row + 3 * 4] * b[3 + col * 4];
        }
    }
}

// ---------------------------------------------------------------------------
// Shared inline utilities — the math that keeps the skeleton from imploding.
// Used across all Scene3D translation units. Quaternion math, euler conversion,
// bone lookup. Every function here was written at 3 PM with the wrong sign
// convention at least twice before it worked. Euler→Quat alone has 6 possible
// rotation orders (XYZ, XZY, ZYX...) and Pandemic used XYZ.
// Or whatever bullshit. I even managed to contact with an Ex-Pandemic guy
// and he told me that EVEN they were not sure of which fucking way was up
// anyways... so I tried ZYX first.
// The character's head came out from its ass and then rotated 180 degrees
// and stared at me. Backwards.
// ---------------------------------------------------------------------------

inline bool IsIdentityQuatApprox(const hkQuaternion& q)
{
    hkVector4 i = q.getImag();
    return fabsf(i(0)) < 1e-5f && fabsf(i(1)) < 1e-5f && fabsf(i(2)) < 1e-5f && fabsf(q.getReal() - 1.0f) < 1e-5f;
}

inline bool HasTransOverride(const Scene3DRenderer::EditorTransOverride& t)
{
    return (fabsf(t.x) > 1e-6f || fabsf(t.y) > 1e-6f || fabsf(t.z) > 1e-6f);
}

inline hkQuaternion EulerDegreesToQuatXYZ(float rxDeg, float ryDeg, float rzDeg)
{
    const float d2r = 0.0174532925f;
    hkVector4 axisX; axisX.set(1.0f, 0.0f, 0.0f);
    hkVector4 axisY; axisY.set(0.0f, 1.0f, 0.0f);
    hkVector4 axisZ; axisZ.set(0.0f, 0.0f, 1.0f);
    hkQuaternion qx(axisX, rxDeg * d2r);
    hkQuaternion qy(axisY, ryDeg * d2r);
    hkQuaternion qz(axisZ, rzDeg * d2r);
    hkQuaternion q;
    hkQuaternion t;
    t.setMul(qy, qx);
    q.setMul(qz, t);
    q.normalize();
    return q;
}

inline void QuatToEulerDegreesXYZ(const hkQuaternion& q, float& rxDeg, float& ryDeg, float& rzDeg)
{
    hkRotation r;
    r.set(q);
    hkVector4 c0 = r.getColumn(0);
    hkVector4 c1 = r.getColumn(1);
    hkVector4 c2 = r.getColumn(2);
    float m00 = c0(0), m10 = c0(1), m20 = c0(2);
    float m11 = c1(1), m21 = c1(2);
    float m12 = c2(1), m22 = c2(2);

    float x = 0.0f, y = 0.0f, z = 0.0f;
    if (m20 > -1.0f)
    {
        if (m20 < 1.0f)
        {
            y = asinf(-m20);
            x = atan2f(m21, m22);
            z = atan2f(m10, m00);
        }
        else
        {
            y = -1.5707963f;
            x = -atan2f(-m12, m11);
            z = 0.0f;
        }
    }
    else
    {
        y = 1.5707963f;
        x = atan2f(-m12, m11);
        z = 0.0f;
    }
    const float r2d = 57.2957795f;
    rxDeg = x * r2d;
    ryDeg = y * r2d;
    rzDeg = z * r2d;
}

inline int FindSkeletonBoneIndex(const hkaSkeleton* skeleton, const char* name)
{
    if (!skeleton || !name || name[0] == '\0') return -1;
    for (int i = 0; i < skeleton->m_numBones; i++)
    {
        const hkaBone* bone = skeleton->m_bones[i];
        if (bone && bone->m_name && strcmp(bone->m_name, name) == 0)
        {
            return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Animation structs — the binary format Pandemic used to pack bone rotations.
//
// ThreeComp40 is a 40-bit compressed quaternion format. FORTY BITS for a
// quaternion that normally takes 128 bits. Pandemic's compression was
// AGGRESSIVE. They packed 3 components into 40 bits using variable-width
// encoding and reconstructed the 4th component from the unit constraint
// (x²+y²+z²+w²=1). I spent days in Ghidra staring at the decode
// function at 0x006B2A10 before I understood the bit layout. The packing
// changes depending on which component is largest. There's no header byte.
// You just have to KNOW from context which scheme a given track uses.
// Pandemic's tools knew. We had to reverse-engineer the knowledge.
//
// JsonTrack / JsonAnimClip are our decoded representation — human-readable
// frame data that we can actually debug. The original binary is opaque
// horseshit. These structs are the light at the end of that tunnel.
// ---------------------------------------------------------------------------

struct ThreeComp40
{
    int a;
    int b;
    int c;
    int d;
    int e;
};

struct JsonTrack
{
    int boneIndex;
    std::vector<int> frames;
    struct Quat4
    {
        float x;
        float y;
        float z;
        float w;
    };
    std::vector<Quat4> rotations;  // decoded using the active mode at load time
    struct PackedQuat16
    {
        short x;
        short y;
        short z;
        short w;
    };
    std::vector<PackedQuat16> rotationsPacked;
    bool rotationsPackedValid;

    JsonTrack()
        : boneIndex(-1),
          rotationsPackedValid(false)
    {}
};

struct JsonVec3
{
    float x;
    float y;
    float z;
};

struct JsonTranslationTrack
{
    int boneIndex;
    std::vector<int> frames;
    std::vector<JsonVec3> translations;
};

struct JsonType2Block
{
    int nbytes;
    int flags;
    int s1;
    int s2;
    std::vector<int> data;
    std::vector<float> valsA;
    std::vector<std::string> valsAStr;
    std::vector<int> valsType2;
    bool valid;

    JsonType2Block()
        : nbytes(0), flags(0), s1(0), s2(0), valid(false)
    {}
};

struct JsonRotBlock
{
    int nbytes;
    int flags;
    int s1;
    int s2;
    std::vector<int> data;
    std::vector<ThreeComp40> vals;
    bool valid;

    JsonRotBlock()
        : nbytes(0), flags(0), s1(0), s2(0), valid(false)
    {}
};

struct JsonAnimInfo
{
    char key[128];
    int gamemodemask;
    int offset;
    int size;
    int kind;
    float unk_5;
    int vals_num;
    int vals2_num;
    int unk_8;
    int vala;
    int unk_10;
    int unk_11;
    int data_offset;
    float unk_13;
    float unk_14;
    float t_scale;
    int block_starts_offset;
    int block_starts_num;
    int block_starts2_offset;
    int block_starts2_num;
    int obj_c3_offset;
    int obj_c3_num;
    int obj_c4_offset;
    int obj_c4_num;
    int block_offset;
    int block_size;
    int obj3_num;
    int obj3_offset;
    int bones_num1;
    int unk_29;
    int obj1_num;
    int bones_offset;
    int unk_32;
    int obj1_offset;
    int obj2_offset;
    int obj2_num;
    int obj5_offset;
};

struct JsonScaleTrack
{
    int boneIndex;
    std::vector<int> frames;
    std::vector<JsonVec3> scales;
};

// ---------------------------------------------------------------------------
// Animation Event Types — EVERY SINGLE EVENT Pandemic embedded in their
// 11,726 animation files, reverse-engineered from binary CRC32 hashes.
//
// Each of these was a string in Pandemic's source code. "ApplyDamage",
// "TrailOnRight", "FireProjectile", "RumblePlay"... We found them by
// brute-forcing CRC32 against a dictionary of game development terms
// at 3 AM until the hashes matched. Some took DAYS to crack.
// Why didnt you used the haighcam's parser you dumbass?
// I was trying to make my own parser in c++ without needing to decompile
// but at the end i realized i was just doing some random bullshit.
// The string "FF_ONLY_DamageOBBArea" took me some time
// because who the FUCK names
// a function "FF_ONLY"? (It means "friendly fire only." Thanks, Pandemic.
// Very clear. Very intuitive. Definitely worth the hours of my life.)
//
// 64 unique event types across 15,166 event instances. Damage events,
// sound triggers (routed through the stolen Wwise SDK), camera shakes,
// weapon trails, projectile spawning, throw physics, bow string animation,
// controller rumble... Pandemic's animators had control over EVERYTHING
// from within their animation tool. These events made the game feel alive.
// Now they make our reverse-engineered viewer feel slightly less dead.
// ---------------------------------------------------------------------------
enum AnimEventType
{
    ANIM_EVT_UNKNOWN = 0,

    // Damage / Combat
    ANIM_EVT_APPLY_DAMAGE,
    ANIM_EVT_APPLY_DAMAGE_TO_TARGET,
    ANIM_EVT_DAMAGE_OBB_AREA,
    ANIM_EVT_DAMAGE_OBB_AREA_LEFT_HAND,
    ANIM_EVT_DAMAGE_OBB_AREA_RIGHT_HAND,
    ANIM_EVT_DAMAGE_CYLINDER_AREA,
    ANIM_EVT_DAMAGE_CYLINDER_AREA_RIGHT_HAND,
    ANIM_EVT_FF_ONLY_DAMAGE_OBB_AREA,
    ANIM_EVT_FF_DAMAGE_OBB_AREA_RIGHT_HAND,
    ANIM_EVT_CLEAR_HIT_LIST,
    ANIM_EVT_FLYER_CLEAR_HIT_LIST,
    ANIM_EVT_FLYER_DAMAGE_OBB_AREA,
    ANIM_EVT_GRAB_OBB_AREA_LEFT_HAND,
    ANIM_EVT_GRAB_OBB_AREA_RIGHT_HAND,
    ANIM_EVT_NEW_HIT2,
    ANIM_EVT_NEW_HIT3,
    ANIM_EVT_NEW_HIT4,
    ANIM_EVT_NEW_HIT5,

    // Trails / Visual FX
    ANIM_EVT_TRAIL_ON_LEFT,
    ANIM_EVT_TRAIL_ON_RIGHT,
    ANIM_EVT_TRAIL_ON_RIGHT_SPLIT,
    ANIM_EVT_TRAIL_OFF,
    ANIM_EVT_TRAIL_OFF_LEFT,
    ANIM_EVT_TRAIL_OFF_RIGHT,
    ANIM_EVT_ACTIVATE_ATTACHED_PARTICLE_EFFECT,
    ANIM_EVT_ACTIVATE_PARTICLE_EFFECT,

    // Sound
    ANIM_EVT_SOUND_CUE,
    ANIM_EVT_SOUND_EVENT,
    ANIM_EVT_SET_SOUND_OVERRIDE,

    // Camera
    ANIM_EVT_CAMERA,
    ANIM_EVT_CAMERA_EFFECT,
    ANIM_EVT_CAMERA_EFFECT_ALWAYS,

    // State / Logic
    ANIM_EVT_STATE_CHANGE1,
    ANIM_EVT_STATE_CHANGE2,
    ANIM_EVT_BEGIN_FACE_TARGET,
    ANIM_EVT_END_FACE_TARGET,
    ANIM_EVT_POST_GENERIC_EVENT,
    ANIM_EVT_JUMP,
    ANIM_EVT_DESTROY,
    ANIM_EVT_SH_INVINCIBLE,
    ANIM_EVT_SELECT_INVENTORY_LOADOUT,
    ANIM_EVT_ENABLE_DRONE,
    ANIM_EVT_ABILITY_ACTIVATION_EVENT,
    ANIM_EVT_SET_ROTATION_RATE_SCALE,
    ANIM_EVT_START_CHARGE,
    ANIM_EVT_STOP_CHARGE,

    // Projectile
    ANIM_EVT_FIRE_PROJECTILE,
    ANIM_EVT_FIRE_PROJECTILE_LEFT_HAND,
    ANIM_EVT_FIRE_PROJECTILE_RIGHT_HAND,
    ANIM_EVT_FIRE_PROJECTILE_VISUAL,
    ANIM_EVT_FIRE_PROJECTILE_VISUAL_LEFT_HAND,
    ANIM_EVT_READY_PROJECTILE,
    ANIM_EVT_READY_PROJECTILE_LEFT_HAND,
    ANIM_EVT_READY_PROJECTILE_RIGHT_HAND,
    ANIM_EVT_UNREADY_PROJECTILE_LEFT_HAND,
    ANIM_EVT_UNREADY_PROJECTILE_RIGHT_HAND,
    ANIM_EVT_EMBED_LAUNCH_POINT,

    // Throw
    ANIM_EVT_THROW_LEFT_HAND,
    ANIM_EVT_THROW_LEFT_TORQUE,
    ANIM_EVT_THROW_RIGHT_HAND,
    ANIM_EVT_THROW_RIGHT_TORQUE,

    // Bow
    ANIM_EVT_GRAB_STRING_L,
    ANIM_EVT_RELEASE_STRING_L,

    // Controller
    ANIM_EVT_RUMBLE_PLAY,

    ANIM_EVT_COUNT
};

struct JsonAnimEventVal
{
    enum Type { EVT_INT, EVT_FLOAT, EVT_CRC };
    Type type;
    int intVal;
    float floatVal;
    char crcVal[128];

    JsonAnimEventVal()
        : type(EVT_INT), intVal(0), floatVal(0.0f)
    {
        crcVal[0] = '\0';
    }
};

struct JsonAnimEvent
{
    char event[128];
    float t;
    AnimEventType eventType;
    std::vector<JsonAnimEventVal> vals;

    JsonAnimEvent()
        : t(0.0f), eventType(ANIM_EVT_UNKNOWN)
    {
        event[0] = '\0';
    }
};

// ---------------------------------------------------------------------------
// SoundEvent category identifiers (17 unique categories from 11,726 animation files)
//
// These are the CRC[0] values found in ANIM_EVT_SOUND_EVENT entries.
// The original game posts these as Wwise event names; Wwise resolves the
// actual sound variation via creature/weapon/material switch groups already
// set on the game object.  CRC[1], when present, is a bone name hint for
// 3D spatial positioning (e.g. "LeftHand", "FootLeftFront").
//
// Frequency from full dataset (15,166 SoundEvent instances):
//   footstep_walk 10967 | swing 1654 | footstep_run 1260 | attack_vocal 607
//   land 142 | Death 126 | taunt 96 | release 82 | footstep_trot 76
//   charge 64 | punch 50 | Jump 18 | exhale 8 | inhale 8
//   Throw 6 | cheer 1 | pickup 1
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// SoundCue modifier switch groups
//
// ANIM_EVT_SOUND_CUE CRC layout (50+ unique primary cues, 7,378 instances):
//   CRC[0] = primary Wwise event name (e.g. "Foley_Jump", "Hand_Grab")
//   CRC[1] = optional modifier   → set as "anim_modifier" switch before posting
//   CRC[2] = optional sub-qualifier → set as "anim_sub" switch before posting
//
// Known CRC[1] values: wing_flap (90), Warg (28), skid (16)
// Known CRC[2] values: downstroke (69), upstroke (21), turn_180 (6),
//                       skid_turn (3), skid (3)
//
// All 6 integer parameters are always zero across the entire dataset.
// ---------------------------------------------------------------------------

// Obj5 entries — 7 floats of COMPLETELY UNKNOWN PURPOSE.
// Found in the animation binary at the offset pointed to by obj5_offset.
// Every single field is named unk_ because after months of analysis I
// still have NO FUCKING IDEA what these do. They're always present.
// They vary per animation. They're not bone data, not timing data,
// not blend weights. My best guess is they're bounding box or
// collision capsule dimensions for the character during this animation,
// but I can't prove it. Pandemic took this secret to their grave.
// If you're a former Pandemic animator reading this: WHAT ARE THESE.
// PLEASE. I'M BEGGING YOU. EMAIL ME. I WILL PAY YOU IN BEER.
struct JsonObj5Entry
{
    float unk_0;
    float unk_1;
    float unk_2;
    float unk_3;
    float unk_4;
    float unk_5;
    float unk_6;

    JsonObj5Entry()
        : unk_0(0), unk_1(0), unk_2(0), unk_3(0), unk_4(0), unk_5(0), unk_6(0)
    {}
};

// JsonAnimClip — THE decoded animation. This is what a Conquest .ANM file
// looks like after we've unpacked the compressed binary into something
// a human can read without losing their mind. Rotation tracks, translation
// tracks, scale tracks, root motion, events, the mysterious obj5 data,
// and all the raw block data for formats we haven't fully decoded yet.
// This struct is the beating heart of the animation system. It gets
// passed around like a cursed artifact between 15+ functions. If you
// corrupt one field, the entire animation pipeline produces output that
// looks like the skeleton is having a seizure in 4-dimensional space.
struct JsonAnimClip
{
    float frameTime;
    float duration;
    int frameCount;
    int maxFramesPerBlock;
    std::vector<JsonTrack> tracks;
    std::vector<JsonTranslationTrack> translationTracks;
    std::vector<JsonScaleTrack> scaleTracks;
    int decodeCount;
    int altDecodeCount;
    int rootBoneIndex;
    std::vector<int> rootFrames;
    std::vector<JsonVec3> rootTranslations;
    std::vector<std::string> boneNames;
    std::vector< std::vector<JsonType2Block> > type2Obj0;
    std::vector< std::vector<JsonType2Block> > type2Obj2;
    std::vector< std::vector<JsonRotBlock> > rotObj1;
    std::vector<JsonAnimEvent> events;
    std::vector<JsonObj5Entry> obj5a;
    std::vector<JsonObj5Entry> obj5b;
    JsonAnimInfo info;
};

// ---------------------------------------------------------------------------
// Quaternion decode/encode — the THREE different decompression schemes
// Pandemic used for rotation data, plus our own encode for round-tripping.
//
// DecodeThreeComp40Havok: Uses Havok's own unpackSignedQuaternion40().
//   This is the "correct" one that matches what the game actually does
//   at runtime. Found the call in the .exe's animation update path.
//   The function is inside the stolen Havok Animation SDK.
//
// DecodeThreeComp40A: Our manual decode — 3×13-bit signed integers packed
//   into 40 bits, divided by 4096 to get [-1,1], 4th component from
//   unit quaternion constraint. 1-bit sign for W. This matches MOST files.
//
// DecodeThreeComp40B: Same layout but DIFFERENT BIT WIDTHS — because
//   Pandemic apparently had two versions of their export tool and each
//   one packed the bits differently. I discovered this at 5 AM when
//   half my test animations played correctly and the other half made
//   the skeleton turn inside out. The difference? 13-bit vs 14-bit
//   component widths. ONE FUCKING BIT. Cost me 3 days.
// ---------------------------------------------------------------------------

inline int SignExtend13(int v)
{
    if (v & 0x1000) v = v - 0x2000;
    return v;
}

inline JsonTrack::Quat4 DecodeThreeComp40Havok(const ThreeComp40& v)
{
    hkUint8 bytes[5];
    bytes[0] = (hkUint8)(v.a & 0xFF);
    bytes[1] = (hkUint8)(v.b & 0xFF);
    bytes[2] = (hkUint8)(v.c & 0xFF);
    bytes[3] = (hkUint8)(v.d & 0xFF);
    bytes[4] = (hkUint8)(v.e & 0xFF);
    hkQuaternion q;
    unpackSignedQuaternion40(bytes, &q);
    q.normalize();
    JsonTrack::Quat4 out;
    out.x = q(0); out.y = q(1); out.z = q(2); out.w = q(3);
    return out;
}

inline JsonTrack::Quat4 DecodeThreeComp40A(const ThreeComp40& v)
{
    unsigned long long raw = 0;
    raw |= (unsigned long long)(v.a & 0xFF);
    raw |= (unsigned long long)(v.b & 0xFF) << 8;
    raw |= (unsigned long long)(v.c & 0xFF) << 16;
    raw |= (unsigned long long)(v.d & 0xFF) << 24;
    raw |= (unsigned long long)(v.e & 0xFF) << 32;
    int xi = SignExtend13((int)(raw & 0x1FFF));
    int yi = SignExtend13((int)((raw >> 13) & 0x1FFF));
    int zi = SignExtend13((int)((raw >> 26) & 0x1FFF));
    int sign = (int)((raw >> 39) & 0x1);
    const float scale = 4096.0f;
    float x = (float)xi / scale, y = (float)yi / scale, z = (float)zi / scale;
    float w2 = 1.0f - x*x - y*y - z*z;
    if (w2 < 0.0f) w2 = 0.0f;
    float w = (float)sqrt(w2);
    if (sign) w = -w;
    JsonTrack::Quat4 q; q.x = x; q.y = y; q.z = z; q.w = w;
    return q;
}

inline JsonTrack::Quat4 DecodeThreeComp40B(const ThreeComp40& v)
{
    unsigned long long raw = 0;
    raw |= (unsigned long long)(v.a & 0xFF);
    raw |= (unsigned long long)(v.b & 0xFF) << 8;
    raw |= (unsigned long long)(v.c & 0xFF) << 16;
    raw |= (unsigned long long)(v.d & 0xFF) << 24;
    raw |= (unsigned long long)(v.e & 0xFF) << 32;
    int xi = SignExtend13((int)(raw & 0x1FFF));
    int zi = SignExtend13((int)((raw >> 13) & 0x1FFF));
    int yi = SignExtend13((int)((raw >> 26) & 0x1FFF));
    int sign = (int)((raw >> 39) & 0x1);
    const float scale = 4096.0f;
    float x = (float)xi / scale, y = (float)yi / scale, z = (float)zi / scale;
    float w2 = 1.0f - x*x - y*y - z*z;
    if (w2 < 0.0f) w2 = 0.0f;
    float w = (float)sqrt(w2);
    if (sign) w = -w;
    JsonTrack::Quat4 q; q.x = x; q.y = y; q.z = z; q.w = w;
    return q;
}

inline JsonTrack::Quat4 DecodeThreeComp40RotVec(const ThreeComp40& v)
{
    unsigned long long raw = 0;
    raw |= (unsigned long long)(v.a & 0xFF);
    raw |= (unsigned long long)(v.b & 0xFF) << 8;
    raw |= (unsigned long long)(v.c & 0xFF) << 16;
    raw |= (unsigned long long)(v.d & 0xFF) << 24;
    raw |= (unsigned long long)(v.e & 0xFF) << 32;
    int xi = SignExtend13((int)(raw & 0x1FFF));
    int yi = SignExtend13((int)((raw >> 13) & 0x1FFF));
    int zi = SignExtend13((int)((raw >> 26) & 0x1FFF));
    const float scale = 16383.0f;
    float x = (float)xi / scale, y = (float)yi / scale, z = (float)zi / scale;
    float angle = (float)sqrt(x*x + y*y + z*z);
    JsonTrack::Quat4 q;
    if (angle < 1e-6f) { q.x = 0.0f; q.y = 0.0f; q.z = 0.0f; q.w = 1.0f; return q; }
    float half = angle * 0.5f;
    float s = (float)sin(half) / angle;
    q.x = x * s; q.y = y * s; q.z = z * s; q.w = (float)cos(half);
    return q;
}

inline hkQuaternion MakeQuaternion(const JsonTrack::Quat4& q)
{
    hkQuaternion out;
    out.set(q.x, q.y, q.z, q.w);
    out.normalize();
    return out;
}

inline ThreeComp40 EncodeThreeComp40Havok(const hkQuaternion& qIn)
{
    hkQuaternion q = qIn;
    q.normalize();
    hkUint8 bytes[5];
    packSignedQuaternion40(&q, bytes);
    ThreeComp40 v;
    v.a = bytes[0]; v.b = bytes[1]; v.c = bytes[2]; v.d = bytes[3]; v.e = bytes[4];
    return v;
}

inline JsonTrack::Quat4 QuatToQuat4(const hkQuaternion& q)
{
    JsonTrack::Quat4 out;
    out.x = q.m_vec(0); out.y = q.m_vec(1); out.z = q.m_vec(2); out.w = q.m_vec(3);
    return out;
}

inline short PackQuat16Component(float v)
{
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    const float scale = 32767.0f;
    float f = v * scale;
    int iv = (int)((f >= 0.0f) ? (f + 0.5f) : (f - 0.5f));
    if (iv > 32767) iv = 32767;
    if (iv < -32767) iv = -32767;
    return (short)iv;
}

inline JsonTrack::PackedQuat16 PackQuat16(const hkQuaternion& qIn)
{
    hkQuaternion q = qIn;
    q.normalize();
    if (q(3) < 0.0f)
    {
        q.set(-q(0), -q(1), -q(2), -q(3));
    }
    JsonTrack::PackedQuat16 p;
    p.x = PackQuat16Component(q(0));
    p.y = PackQuat16Component(q(1));
    p.z = PackQuat16Component(q(2));
    p.w = PackQuat16Component(q(3));
    return p;
}

inline hkQuaternion UnpackQuat16(const JsonTrack::PackedQuat16& p)
{
    const float scale = 1.0f / 32767.0f;
    hkQuaternion q;
    q.set(p.x * scale, p.y * scale, p.z * scale, p.w * scale);
    q.normalize();
    return q;
}

inline float Quat4Error(const JsonTrack::Quat4& a, const JsonTrack::Quat4& b)
{
    float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (dot < 0.0f) dot = -dot;
    return 1.0f - dot;
}

inline ThreeComp40 EncodeThreeComp40Strict(const hkQuaternion& qIn, const ThreeComp40* original)
{
    hkQuaternion q = qIn;
    q.normalize();
    JsonTrack::Quat4 q4 = QuatToQuat4(q);
    ThreeComp40 v1 = EncodeThreeComp40Havok(q);
    JsonTrack::Quat4 d1 = DecodeThreeComp40Havok(v1);
    float e1 = Quat4Error(q4, d1);
    JsonTrack::Quat4 q4neg = q4;
    q4neg.x = -q4neg.x; q4neg.y = -q4neg.y; q4neg.z = -q4neg.z; q4neg.w = -q4neg.w;
    hkQuaternion qNeg = MakeQuaternion(q4neg);
    ThreeComp40 v2 = EncodeThreeComp40Havok(qNeg);
    JsonTrack::Quat4 d2 = DecodeThreeComp40Havok(v2);
    float e2 = Quat4Error(q4, d2);
    ThreeComp40 best = (e2 < e1) ? v2 : v1;
    float bestErr = (e2 < e1) ? e2 : e1;
    if (original)
    {
        JsonTrack::Quat4 d0 = DecodeThreeComp40Havok(*original);
        float e0 = Quat4Error(q4, d0);
        if (e0 <= bestErr + 1e-6f) return *original;
    }
    return best;
}

inline JsonTrack::Quat4 PickBestQuat(const JsonTrack::Quat4& a, const JsonTrack::Quat4& b, bool& usedAlt)
{
    float ma = a.x * a.x + a.y * a.y + a.z * a.z;
    float mb = b.x * b.x + b.y * b.y + b.z * b.z;
    if (mb < ma) { usedAlt = true; return b; }
    usedAlt = false;
    return a;
}

inline JsonTrack::Quat4 SelectQuatForMode(const JsonTrack& track, int index, int /*mode*/)
{
    if (track.rotations.empty() &&
        track.rotationsPackedValid &&
        index >= 0 &&
        index < (int)track.rotationsPacked.size())
    {
        hkQuaternion q = UnpackQuat16(track.rotationsPacked[index]);
        return QuatToQuat4(q);
    }
    if (index < 0 || index >= (int)track.rotations.size())
    {
        JsonTrack::Quat4 q; q.x = q.y = q.z = 0.0f; q.w = 1.0f;
        return q;
    }
    return track.rotations[index];
}

inline JsonTrack::Quat4 ApplyQuatAxisMap(const JsonTrack::Quat4& q, int axisMode, int signMask)
{
    float x = q.x, y = q.y, z = q.z;
    float nx = x, ny = y, nz = z;
    switch (axisMode)
    {
    case 1: nx = x; ny = z; nz = y; break;
    case 2: nx = z; ny = y; nz = x; break;
    case 3: nx = y; ny = x; nz = z; break;
    case 4: nx = y; ny = z; nz = x; break;
    case 5: nx = z; ny = x; nz = y; break;
    default: nx = x; ny = y; nz = z; break;
    }
    if (signMask & 1) nx = -nx;
    if (signMask & 2) ny = -ny;
    if (signMask & 4) nz = -nz;
    JsonTrack::Quat4 out = q;
    out.x = nx; out.y = ny; out.z = nz;
    return out;
}

inline JsonTrack::Quat4 ApplyQuatAxisMapInverse(const JsonTrack::Quat4& q, int axisMode, int signMask)
{
    float nx = q.x, ny = q.y, nz = q.z;
    if (signMask & 1) nx = -nx;
    if (signMask & 2) ny = -ny;
    if (signMask & 4) nz = -nz;
    float x = nx, y = ny, z = nz;
    switch (axisMode)
    {
    case 1: x = nx; y = nz; z = ny; break;
    case 2: x = nz; y = ny; z = nx; break;
    case 3: x = ny; y = nx; z = nz; break;
    case 4: x = nz; y = nx; z = ny; break;
    case 5: x = ny; y = nz; z = nx; break;
    default: x = nx; y = ny; z = nz; break;
    }
    JsonTrack::Quat4 out = q;
    out.x = x; out.y = y; out.z = z;
    return out;
}

// ---------------------------------------------------------------------------
// Editor key helpers — used by both Animation (pose building) and BoneEditor
// ---------------------------------------------------------------------------

inline hkQuaternion EditorKeyToQuat(const Scene3DRenderer::EditorKey& k)
{
    hkQuaternion q;
    q.set(k.rot[0], k.rot[1], k.rot[2], k.rot[3]);
    q.normalize();
    return q;
}

inline JsonTrack::Quat4 EditorKeyToQuat4(const Scene3DRenderer::EditorKey& k)
{
    JsonTrack::Quat4 q;
    q.x = k.rot[0]; q.y = k.rot[1]; q.z = k.rot[2]; q.w = k.rot[3];
    return q;
}

inline void StoreEditorQuat(Scene3DRenderer::EditorKey& k, const hkQuaternion& rot)
{
    hkVector4 imag = rot.getImag();
    k.rot[0] = imag(0); k.rot[1] = imag(1); k.rot[2] = imag(2);
    k.rot[3] = rot.getReal();
}

// ---------------------------------------------------------------------------
// Core quaternion helpers — must be before SampleEditorKey for VS2005
// ---------------------------------------------------------------------------

inline float Clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

inline float QuatDot4(const hkQuaternion& a, const hkQuaternion& b)
{
    return a.m_vec(0) * b.m_vec(0) +
           a.m_vec(1) * b.m_vec(1) +
           a.m_vec(2) * b.m_vec(2) +
           a.m_vec(3) * b.m_vec(3);
}

inline hkQuaternion QuatNegated(const hkQuaternion& q)
{
    hkQuaternion out;
    out.set(-q.m_vec(0), -q.m_vec(1), -q.m_vec(2), -q.m_vec(3));
    return out;
}

inline hkQuaternion QuatSlerpShortest(const hkQuaternion& a, const hkQuaternion& b, float t)
{
    hkQuaternion bb = b;
    if (QuatDot4(a, bb) < 0.0f)
    {
        bb = QuatNegated(bb);
    }
    hkQuaternion out;
    out.setSlerp(a, bb, t);
    out.normalize();
    return out;
}

inline hkQuaternion QuatNlerpShortest(const hkQuaternion& a, const hkQuaternion& b, float t)
{
    hkQuaternion bb = b;
    if (QuatDot4(a, bb) < 0.0f)
    {
        bb = QuatNegated(bb);
    }
    float it = 1.0f - t;
    hkQuaternion out;
    out.set(a.m_vec(0) * it + bb.m_vec(0) * t,
            a.m_vec(1) * it + bb.m_vec(1) * t,
            a.m_vec(2) * it + bb.m_vec(2) * t,
            a.m_vec(3) * it + bb.m_vec(3) * t);
    out.normalize();
    return out;
}

inline hkQuaternion QuatBlend(const hkQuaternion& a, const hkQuaternion& b, float t, bool useNlerp);

// Forward declarations for SQUAD (defined after QuatFromTo)
inline hkQuaternion QuatLog(const hkQuaternion& q);
inline hkQuaternion QuatExp(const hkQuaternion& q);
inline hkQuaternion QuatMulInv(const hkQuaternion& a, const hkQuaternion& b);
inline hkQuaternion ComputeSquadIntermediate(const hkQuaternion& qPrev, const hkQuaternion& qCurr, const hkQuaternion& qNext);
inline hkQuaternion QuatSquad(const hkQuaternion& q0, const hkQuaternion& q1, const hkQuaternion& s0, const hkQuaternion& s1, float t);

inline bool SampleEditorKey(const std::vector<Scene3DRenderer::EditorKey>& keys,
                            float frame, int interpMode, bool useNlerpRotation, hkQuaternion& out)
{
    if (keys.empty()) return false;

    // Convert frame to milliseconds for precise interpolation
    float frameTimeMs = 33.33333f;  // 30 FPS = 33.33ms per frame
    float timeMs = frame * frameTimeMs;

    if (timeMs < keys.front().timeMs) return false;
    if (interpMode == 1 && keys.size() >= 2)
    {
        for (size_t i = 0; i + 1 < keys.size(); ++i)
        {
            float t0 = keys[i].timeMs;
            float t1 = keys[i + 1].timeMs;
            if (timeMs < t0 || timeMs > t1) continue;
            hkQuaternion q0 = EditorKeyToQuat(keys[i]);
            hkQuaternion q1 = EditorKeyToQuat(keys[i + 1]);
            if (t1 - t0 < 1e-6f) { out = q0; return true; }

            // Calculate linear alpha
            float alpha = (timeMs - t0) / (t1 - t0);

            // Apply easing function to alpha
            float easedAlpha = EvaluateEasing(alpha, keys[i].easingType,
                keys[i].easingCp1x, keys[i].easingCp1y,
                keys[i].easingCp2x, keys[i].easingCp2y);

            if (easedAlpha <= 0.0f) { out = q0; return true; }
            if (easedAlpha >= 1.0f) { out = q1; return true; }

            // Use SQUAD for C1-continuous rotation when 3+ keys exist
            if (keys.size() >= 3)
            {
                // Ensure neighborhood consistency for the 4 quats we'll use
                if (QuatDot4(q0, q1) < 0.0f) q1 = QuatNegated(q1);

                // Get surrounding keys for intermediate control points
                hkQuaternion qPrev = (i > 0) ? EditorKeyToQuat(keys[i - 1]) : q0;
                hkQuaternion qNext = (i + 2 < keys.size()) ? EditorKeyToQuat(keys[i + 2]) : q1;
                if (QuatDot4(q0, qPrev) < 0.0f) qPrev = QuatNegated(qPrev);
                if (QuatDot4(q1, qNext) < 0.0f) qNext = QuatNegated(qNext);

                hkQuaternion s0 = ComputeSquadIntermediate(qPrev, q0, q1);
                hkQuaternion s1 = ComputeSquadIntermediate(q0, q1, qNext);
                out = QuatSquad(q0, q1, s0, s1, easedAlpha);
            }
            else
            {
                // 2 keys only — fall back to SLERP
                out = QuatBlend(q0, q1, easedAlpha, useNlerpRotation);
            }
            return true;
        }
    }
    // Stepped interpolation: return last key before current time
    for (int i = (int)keys.size() - 1; i >= 0; --i)
    {
        if (timeMs >= keys[i].timeMs) { out = EditorKeyToQuat(keys[i]); return true; }
    }
    return false;
}

inline float Hermite1D(float v0, float m0, float v1, float m1, float t)
{
    float t2 = t * t;
    float t3 = t2 * t;
    float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    float h10 = t3 - 2.0f * t2 + t;
    float h01 = -2.0f * t3 + 3.0f * t2;
    float h11 = t3 - t2;
    return h00 * v0 + h10 * m0 + h01 * v1 + h11 * m1;
}

inline bool SampleEditorFloatCurveAtTimeMs(const std::vector<Scene3DRenderer::EditorFloatKey>& keys,
                                          float timeMs, int interpMode, float& out)
{
    if (keys.empty()) return false;
    if (timeMs < keys.front().timeMs) return false;

    // Interpolated mode: evaluate the segment curve.
    if (interpMode == 1 && keys.size() >= 2)
    {
        for (size_t i = 0; i + 1 < keys.size(); ++i)
        {
            float t0 = keys[i].timeMs;
            float t1 = keys[i + 1].timeMs;
            if (timeMs < t0 || timeMs > t1) continue;
            float dtMs = t1 - t0;
            if (dtMs < 1e-6f)
            {
                out = keys[i].value;
                return true;
            }

            float alpha = (timeMs - t0) / dtMs;

            float easedAlpha = EvaluateEasing(alpha, keys[i].easingType,
                keys[i].easingCp1x, keys[i].easingCp1y,
                keys[i].easingCp2x, keys[i].easingCp2y);

            int mode = keys[i].interpMode;
            if (mode < Scene3DRenderer::CURVE_CONSTANT) mode = Scene3DRenderer::CURVE_CONSTANT;
            if (mode > Scene3DRenderer::CURVE_CUBIC) mode = Scene3DRenderer::CURVE_CUBIC;

            if (mode == Scene3DRenderer::CURVE_CONSTANT)
            {
                out = keys[i].value;
                return true;
            }
            if (mode == Scene3DRenderer::CURVE_LINEAR)
            {
                out = keys[i].value + (keys[i + 1].value - keys[i].value) * easedAlpha;
                return true;
            }

            // Cubic Hermite: tangents are slopes (value/sec).
            float dtSec = dtMs / 1000.0f;
            float m0 = keys[i].outTangent * dtSec;
            float m1 = keys[i + 1].inTangent * dtSec;
            out = Hermite1D(keys[i].value, m0, keys[i + 1].value, m1, easedAlpha);
            return true;
        }
    }

    // Stepped interpolation (or no segment found): return last key before current time.
    for (int i = (int)keys.size() - 1; i >= 0; --i)
    {
        if (timeMs >= keys[i].timeMs)
        {
            out = keys[i].value;
            return true;
        }
    }
    return false;
}

inline bool SampleEditorTransKey(const std::vector<Scene3DRenderer::EditorFloatKey>& keysX,
                                 const std::vector<Scene3DRenderer::EditorFloatKey>& keysY,
                                 const std::vector<Scene3DRenderer::EditorFloatKey>& keysZ,
                                 float frame, int interpMode, hkVector4& out)
{
    if (keysX.empty()) return false;

    // Convert frame to milliseconds for precise interpolation
    float frameTimeMs = 33.33333f;  // 30 FPS = 33.33ms per frame
    float timeMs = frame * frameTimeMs;

    float x = 0.0f, y = 0.0f, z = 0.0f;
    if (!SampleEditorFloatCurveAtTimeMs(keysX, timeMs, interpMode, x))
        return false;
    // Y/Z are expected to exist, but keep evaluation tolerant.
    if (!keysY.empty()) SampleEditorFloatCurveAtTimeMs(keysY, timeMs, interpMode, y);
    if (!keysZ.empty()) SampleEditorFloatCurveAtTimeMs(keysZ, timeMs, interpMode, z);
    out.set(x, y, z);
    return true;
}

inline bool SampleEditorScaleKey(const std::vector<Scene3DRenderer::EditorFloatKey>& keysX,
                                 const std::vector<Scene3DRenderer::EditorFloatKey>& keysY,
                                 const std::vector<Scene3DRenderer::EditorFloatKey>& keysZ,
                                 float frame, int interpMode, hkVector4& out)
{
    if (keysX.empty()) return false;

    float frameTimeMs = 33.33333f;  // 30 FPS = 33.33ms per frame
    float timeMs = frame * frameTimeMs;

    float x = 1.0f, y = 1.0f, z = 1.0f;
    if (!SampleEditorFloatCurveAtTimeMs(keysX, timeMs, interpMode, x))
        return false;
    if (!keysY.empty()) SampleEditorFloatCurveAtTimeMs(keysY, timeMs, interpMode, y);
    if (!keysZ.empty()) SampleEditorFloatCurveAtTimeMs(keysZ, timeMs, interpMode, z);
    out.set(x, y, z);
    return true;
}

// (QuatDot4, QuatNegated, QuatSlerpShortest, QuatNlerpShortest, Clamp01
//  moved before SampleEditorKey for VS2005 forward-reference compatibility)

// ---------------------------------------------------------------------------
// SQUAD (Spherical Quadrangle) interpolation — implementations.
// Must be after QuatDot4, QuatNegated, QuatSlerpShortest.
// ---------------------------------------------------------------------------
inline hkQuaternion QuatLog(const hkQuaternion& q)
{
    float sinHalf = sqrtf(q.m_vec(0)*q.m_vec(0) + q.m_vec(1)*q.m_vec(1) + q.m_vec(2)*q.m_vec(2));
    hkQuaternion out;
    if (sinHalf < 1e-6f)
    {
        out.set(0.0f, 0.0f, 0.0f, 0.0f);
    }
    else
    {
        float halfAngle = atan2f(sinHalf, q.m_vec(3));
        float scale = halfAngle / sinHalf;
        out.set(q.m_vec(0) * scale, q.m_vec(1) * scale, q.m_vec(2) * scale, 0.0f);
    }
    return out;
}

inline hkQuaternion QuatExp(const hkQuaternion& q)
{
    float halfAngle = sqrtf(q.m_vec(0)*q.m_vec(0) + q.m_vec(1)*q.m_vec(1) + q.m_vec(2)*q.m_vec(2));
    hkQuaternion out;
    if (halfAngle < 1e-6f)
    {
        out.setIdentity();
    }
    else
    {
        float s = sinf(halfAngle) / halfAngle;
        out.set(q.m_vec(0) * s, q.m_vec(1) * s, q.m_vec(2) * s, cosf(halfAngle));
        out.normalize();
    }
    return out;
}

inline hkQuaternion QuatMulInv(const hkQuaternion& a, const hkQuaternion& b)
{
    hkQuaternion inv;
    inv.setInverse(a);
    hkQuaternion bb = b;
    if (QuatDot4(inv, bb) < 0.0f) bb = QuatNegated(bb);
    hkQuaternion result;
    result.setMul(inv, bb);
    return result;
}

inline hkQuaternion ComputeSquadIntermediate(const hkQuaternion& qPrev,
                                              const hkQuaternion& qCurr,
                                              const hkQuaternion& qNext)
{
    hkQuaternion logNext = QuatLog(QuatMulInv(qCurr, qNext));
    hkQuaternion logPrev = QuatLog(QuatMulInv(qCurr, qPrev));
    hkQuaternion negAvg;
    negAvg.set(-(logNext.m_vec(0) + logPrev.m_vec(0)) * 0.25f,
               -(logNext.m_vec(1) + logPrev.m_vec(1)) * 0.25f,
               -(logNext.m_vec(2) + logPrev.m_vec(2)) * 0.25f,
               0.0f);
    hkQuaternion expResult = QuatExp(negAvg);
    hkQuaternion result;
    result.setMul(qCurr, expResult);
    result.normalize();
    return result;
}

inline hkQuaternion QuatSquad(const hkQuaternion& q0, const hkQuaternion& q1,
                               const hkQuaternion& s0, const hkQuaternion& s1, float t)
{
    hkQuaternion slerp01 = QuatSlerpShortest(q0, q1, t);
    hkQuaternion slerpS  = QuatSlerpShortest(s0, s1, t);
    float blend = 2.0f * t * (1.0f - t);
    return QuatSlerpShortest(slerp01, slerpS, blend);
}

inline hkQuaternion QuatFromTo(const hkVector4& fromDir, const hkVector4& toDir)
{
    hkVector4 f = fromDir;
    hkVector4 t = toDir;
    float fl = f.length3();
    float tl = t.length3();
    if (fl < 1e-6f || tl < 1e-6f)
    {
        hkQuaternion q;
        q.setIdentity();
        return q;
    }
    f.normalize3();
    t.normalize3();
    float dot = f.dot3(t);
    if (dot > 0.9999f)
    {
        hkQuaternion q;
        q.setIdentity();
        return q;
    }
    if (dot < -0.9999f)
    {
        hkVector4 axis;
        hkVector4 up;
        hkVector4 right;
        up.set(0.0f, 1.0f, 0.0f);
        right.set(1.0f, 0.0f, 0.0f);
        if (fabsf(f(1)) < 0.99f) axis.setCross(up, f);
        else axis.setCross(right, f);
        float al = axis.length3();
        if (al < 1e-6f)
        {
            hkQuaternion q;
            q.setIdentity();
            return q;
        }
        axis.normalize3();
        hkQuaternion q(axis, 3.1415926f);
        q.normalize();
        return q;
    }
    hkVector4 axis;
    axis.setCross(f, t);
    float axisLen = axis.length3();
    if (axisLen < 1e-6f)
    {
        hkQuaternion q;
        q.setIdentity();
        return q;
    }
    axis.normalize3();
    float clampedDot = dot;
    if (clampedDot < -1.0f) clampedDot = -1.0f;
    if (clampedDot > 1.0f) clampedDot = 1.0f;
    float angle = acosf(clampedDot);
    hkQuaternion q(axis, angle);
    q.normalize();
    return q;
}

inline void QuatToAxisAngle(const hkQuaternion& q, hkVector4& outAxis, float& outAngle)
{
    float w = q.getReal();
    hkVector4 v = q.getImag();
    float vv = v.dot3(v);
    if (vv < 1e-8f)
    {
        outAxis.set(0.0f, 1.0f, 0.0f);
        outAngle = 0.0f;
        return;
    }
    float len = sqrtf(vv);
    outAxis.setMul4(1.0f / len, v);
    float clamped = w;
    if (clamped < -1.0f) clamped = -1.0f;
    if (clamped > 1.0f) clamped = 1.0f;
    outAngle = 2.0f * acosf(clamped);
}

inline hkVector4 RotateVectorByQuat(const hkQuaternion& q, const hkVector4& v)
{
    hkVector4 u = q.getImag();
    float s = q.getReal();
    float dotUV = u.dot3(v);
    hkVector4 cross;
    cross.setCross(u, v);

    hkVector4 term1;
    term1.setMul4(2.0f * dotUV, u);
    hkVector4 term2;
    term2.setMul4((s * s - u.dot3(u)), v);
    hkVector4 term3;
    term3.setMul4(2.0f * s, cross);

    hkVector4 out;
    out.setAdd4(term1, term2);
    out.add4(term3);
    return out;
}

inline hkQuaternion QuatBlend(const hkQuaternion& a, const hkQuaternion& b, float t, bool useNlerp)
{
    t = Clamp01(t);
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    return useNlerp ? QuatNlerpShortest(a, b, t) : QuatSlerpShortest(a, b, t);
}

inline hkVector4 Vec3Lerp(const hkVector4& a, const hkVector4& b, float t)
{
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    const __m128 va = _mm_set_ps(0.0f, a(2), a(1), a(0));
    const __m128 vb = _mm_set_ps(0.0f, b(2), b(1), b(0));
    const __m128 wv = _mm_set1_ps(t);
    const __m128 outv = _mm_add_ps(va, _mm_mul_ps(_mm_sub_ps(vb, va), wv));
    float tmp[4];
    _mm_storeu_ps(tmp, outv);
    hkVector4 out;
    out.set(tmp[0], tmp[1], tmp[2]);
    return out;
#else
    hkVector4 out;
    out.set(a(0) + (b(0) - a(0)) * t,
            a(1) + (b(1) - a(1)) * t,
            a(2) + (b(2) - a(2)) * t);
    return out;
#endif
}

// ---------------------------------------------------------------------------
// Comparator for binary search: compare float frame vs int keyframe index.
// Shared across all animation sampling code.
// ---------------------------------------------------------------------------
struct FloatLessInt {
    bool operator()(float f, int v) const { return f < (float)v; }
};

// ---------------------------------------------------------------------------
// Catmull-Rom interpolation for JsonVec3 translation tracks.
// Given keyframe index k (between k and k+1), alpha in [0,1],
// the full arrays of frames[] and values[], and keyCount.
// Uses the 4 surrounding points (k-1, k, k+1, k+2), clamping at boundaries.
// Produces C1-continuous motion — no velocity discontinuities at keyframes.
// ---------------------------------------------------------------------------
inline JsonVec3 CatmullRomVec3(const std::vector<int>& frames,
                               const std::vector<JsonVec3>& vals,
                               int keyCount, int k, float alpha,
                               bool looping = false)
{
    int i0, i1, i2, i3;
    i1 = k;
    if (looping && keyCount > 2)
    {
        // Wrap indices for looping clips so the tangent at the
        // loop seam sees across the boundary — eliminates velocity
        // spike at the wrap point.
        i0 = (k > 0) ? (k - 1) : (keyCount - 2);
        i2 = (k + 1 < keyCount) ? (k + 1) : 1;
        i3 = (k + 2 < keyCount) ? (k + 2) : ((k + 2) % keyCount + 1);
        if (i3 >= keyCount) i3 = 1;
    }
    else
    {
        // Clamp indices for non-looping clips
        i0 = (k > 0) ? (k - 1) : 0;
        i2 = (k + 1 < keyCount) ? (k + 1) : (keyCount - 1);
        i3 = (k + 2 < keyCount) ? (k + 2) : (keyCount - 1);
    }

    const JsonVec3& p0 = vals[i0];
    const JsonVec3& p1 = vals[i1];
    const JsonVec3& p2 = vals[i2];
    const JsonVec3& p3 = vals[i3];

    float t  = alpha;
    float t2 = t * t;
    float t3 = t2 * t;

    // Standard Catmull-Rom basis: tangent = 0.5 * (p_{i+1} - p_{i-1})
    // q(t) = 0.5 * [(2*p1) + (-p0+p2)*t + (2*p0-5*p1+4*p2-p3)*t^2 + (-p0+3*p1-3*p2+p3)*t^3]
    JsonVec3 out;
    out.x = 0.5f * ((2.0f*p1.x) + (-p0.x + p2.x)*t + (2.0f*p0.x - 5.0f*p1.x + 4.0f*p2.x - p3.x)*t2 + (-p0.x + 3.0f*p1.x - 3.0f*p2.x + p3.x)*t3);
    out.y = 0.5f * ((2.0f*p1.y) + (-p0.y + p2.y)*t + (2.0f*p0.y - 5.0f*p1.y + 4.0f*p2.y - p3.y)*t2 + (-p0.y + 3.0f*p1.y - 3.0f*p2.y + p3.y)*t3);
    out.z = 0.5f * ((2.0f*p1.z) + (-p0.z + p2.z)*t + (2.0f*p0.z - 5.0f*p1.z + 4.0f*p2.z - p3.z)*t2 + (-p0.z + 3.0f*p1.z - 3.0f*p2.z + p3.z)*t3);
    return out;
}

inline void BlendLocalPose(hkQsTransform* inOutLocalA,
                           const hkQsTransform* localB,
                           int boneCount,
                           float alpha,
                           const float* perBoneMask, /* optional */
                           bool useNlerpRotation)
{
    if (!inOutLocalA || !localB || boneCount <= 0) return;
    alpha = Clamp01(alpha);
    if (alpha <= 0.0f) return;

    for (int i = 0; i < boneCount; ++i)
    {
        float w = alpha;
        if (perBoneMask) w *= perBoneMask[i];
        w = Clamp01(w);
        if (w <= 0.0f) continue;

        const hkQsTransform& a = inOutLocalA[i];
        const hkQsTransform& b = localB[i];

        hkQuaternion ra = a.getRotation();
        hkQuaternion rb = b.getRotation();
        hkQuaternion r = QuatBlend(ra, rb, w, useNlerpRotation);

        hkVector4 t = Vec3Lerp(a.getTranslation(), b.getTranslation(), w);
        hkVector4 s = Vec3Lerp(a.getScale(), b.getScale(), w);

        inOutLocalA[i].setRotation(r);
        inOutLocalA[i].setTranslation(t);
        inOutLocalA[i].setScale(s);
    }
}

inline void ApplyAdditiveLocalPose(hkQsTransform* inOutBase,
                                  const hkQsTransform* additive,
                                  const hkQsTransform* additiveRef,
                                  int boneCount,
                                  float alpha,
                                  const float* perBoneMask, /* optional */
                                  bool useNlerpRotation)
{
    if (!inOutBase || !additive || !additiveRef || boneCount <= 0) return;
    alpha = Clamp01(alpha);
    if (alpha <= 0.0f) return;

    for (int i = 0; i < boneCount; ++i)
    {
        float w = alpha;
        if (perBoneMask) w *= perBoneMask[i];
        w = Clamp01(w);
        if (w <= 0.0f) continue;

        hkQuaternion baseR = inOutBase[i].getRotation();
        hkQuaternion addR = additive[i].getRotation();
        hkQuaternion refR = additiveRef[i].getRotation();

        hkQuaternion invRef;
        invRef.setInverse(refR);
        hkQuaternion delta;
        delta.setMul(invRef, addR);
        delta.normalize();

        hkQuaternion ident;
        ident.setIdentity();
        hkQuaternion scaledDelta = QuatBlend(ident, delta, w, useNlerpRotation);

        hkQuaternion outR;
        outR.setMul(baseR, scaledDelta);
        outR.normalize();

        hkVector4 baseT = inOutBase[i].getTranslation();
        hkVector4 addT = additive[i].getTranslation();
        hkVector4 refT = additiveRef[i].getTranslation();
        hkVector4 outT;
        outT.set(baseT(0) + (addT(0) - refT(0)) * w,
                 baseT(1) + (addT(1) - refT(1)) * w,
                 baseT(2) + (addT(2) - refT(2)) * w);

        hkVector4 baseS = inOutBase[i].getScale();
        hkVector4 addS = additive[i].getScale();
        hkVector4 refS = additiveRef[i].getScale();
        hkVector4 outS;
        outS.set(baseS(0) + (addS(0) - refS(0)) * w,
                 baseS(1) + (addS(1) - refS(1)) * w,
                 baseS(2) + (addS(2) - refS(2)) * w);

        inOutBase[i].setRotation(outR);
        inOutBase[i].setTranslation(outT);
        inOutBase[i].setScale(outS);
    }
}

#endif // SCENE3D_RENDERER_INTERNAL_H
