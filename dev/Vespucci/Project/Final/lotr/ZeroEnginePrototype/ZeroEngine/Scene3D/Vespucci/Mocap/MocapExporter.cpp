// ============================================================================
// MocapExporter.cpp -- Writing Animations That Pandemic Never Made
// ============================================================================
//
// This is where webcam motion becomes a real Conquest animation file.
// 62 bones, ThreeComp40 encoded quaternions, blocked into 256-frame
// chunks exactly like the shipped game data. fprintf() all the way down
// because we're writing JSON by hand like fucking animals.
//
// The block structure mirrors what we found in the original .anim binaries:
// each block holds up to 256 frames, each bone gets 3 sub-objects
// (translation/rotation/scale), rotation uses ThreeComp40 with flags=0xF0,
// and there's a 4-entry padding in the frame index array because Pandemic's
// parser checks (frameData.size() >= valueCount + 4). WHY? Nobody knows.
// The original devs are scattered to the wind and EA killed the studio
// in 2009. We just replicate what works.
//
// Empty bones get identity blocks so the reference pose fills in.
// Only bones with actual SMPL motion data get rotation tracks.
// Root motion goes in obj2 as XYZ vectors per frame.
//
// VS2005 compatible. Havok 5.5.0 ThreeComp40 encoding throughout.
// ============================================================================

#include "MocapExporter.h"
#include "Scene3DRendererInternal.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#pragma warning(disable: 4996)

// Game skeleton bone names (must match exactly)
static const char* s_conquestBones[GAME_BONE_COUNT] = {
    "",
    "bone_globalsrt",
    "Bone_Root",
    "Bone_Lumbar1",
    "Bone_UpperBody",
    "Bone_Lumbar2",
    "Bone_Lumbar3",
    "Bone_LShoulder",
    "Bone_Neck",
    "Bone_RShoulder",
    "Bone_LBicep",
    "Bone_LForearm",
    "Bone_LForearmRoll",
    "Bone_LHand",
    "Bone_LHand_attach",
    "Bone_LIndex1",
    "Bone_LMiddle1",
    "Bone_LPinky1",
    "Bone_LRing1",
    "Bone_LThumb1",
    "Bone_LIndex2",
    "Bone_LIndex3",
    "Bone_LMiddle2",
    "Bone_LMiddle3",
    "Bone_LPinky2",
    "Bone_LPinky3",
    "Bone_LRing2",
    "Bone_LRing3",
    "Bone_LThumb2",
    "Bone_LThumb3",
    "Bone_Head",
    "Bone_jaw",
    "Bone_RBicep",
    "Bone_RForearm",
    "Bone_RForearmRoll",
    "Bone_RHand",
    "Bone_RHand_attach",
    "Bone_RIndex1",
    "Bone_RMiddle1",
    "Bone_RPinky1",
    "Bone_RRing1",
    "Bone_RThumb1",
    "Bone_RIndex2",
    "Bone_RIndex3",
    "Bone_RMiddle2",
    "Bone_RMiddle3",
    "Bone_RPinky2",
    "Bone_RPinky3",
    "Bone_RRing2",
    "Bone_RRing3",
    "Bone_RThumb2",
    "Bone_RThumb3",
    "Bone_LThigh",
    "Bone_RThigh",
    "Bone_LShin",
    "Bone_LFootBone1",
    "Bone_LFootBone2",
    "Bone_LFootBone3",
    "Bone_RShin",
    "Bone_RFootBone1",
    "Bone_RFootBone2",
    "Bone_RFootBone3"
};

// Write an empty bone block (3 empty objects)
// obj0 = translation (Type2 only), obj1 = rotation (ThreeComp40 only), obj2 = scale (Type2 only)
static void WriteEmptyBoneBlock(FILE* fp)
{
    fprintf(fp, "[{\"nbytes\":0,\"flags\":0,\"s1\":0,\"s2\":0,\"data\":[],\"vals_a\":[],\"vals\":{\"Type2\":[]}},");
    fprintf(fp, "{\"nbytes\":0,\"flags\":0,\"s1\":0,\"s2\":0,\"data\":[],\"vals\":{\"ThreeComp40\":[]}},");
    fprintf(fp, "{\"nbytes\":0,\"flags\":0,\"s1\":0,\"s2\":0,\"data\":[],\"vals_a\":[],\"vals\":{\"Type2\":[]}}]");
}

// Write a rotation-only bone block with ThreeComp40 encoded quaternions
static void WriteRotationBoneBlock(FILE* fp, const std::vector<RetargetedFrame>& frames,
                                    int boneIdx, int blockBase, int blockFrameCount)
{
    // Obj0: Translation — empty (Type2 only, no ThreeComp40)
    fprintf(fp, "[{\"nbytes\":0,\"flags\":0,\"s1\":0,\"s2\":0,\"data\":[],\"vals_a\":[],\"vals\":{\"Type2\":[]}},");

    // Obj1: Rotation — ThreeComp40 encoded
    // flags=240 (0xF0) and s1=frameCount-1, matching game format
    int nbytes = blockFrameCount * 5;
    fprintf(fp, "{\"nbytes\":%d,\"flags\":240,\"s1\":%d,\"s2\":0,\"data\":[", nbytes, blockFrameCount - 1);

    // Frame indices — 4 padding entries then actual frame indices
    // (parser checks frameData.size() >= valueCount + 4)
    fprintf(fp, "0,0,0,0");
    for (int f = 0; f < blockFrameCount; ++f)
    {
        fprintf(fp, ",%d", f);
    }
    fprintf(fp, "],\"vals\":{\"ThreeComp40\":[");

    // Encode each frame's quaternion
    for (int f = 0; f < blockFrameCount; ++f)
    {
        int frameIdx = blockBase + f;
        if (frameIdx >= (int)frames.size()) frameIdx = (int)frames.size() - 1;

        hkQuaternion q = frames[frameIdx].GetBoneQuat(boneIdx);
        ThreeComp40 tc = EncodeThreeComp40Strict(q, NULL);

        fprintf(fp, "{\"a\":%d,\"b\":%d,\"c\":%d,\"d\":%d,\"e\":%d}",
                tc.a, tc.b, tc.c, tc.d, tc.e);
        if (f < blockFrameCount - 1) fprintf(fp, ",");
    }
    fprintf(fp, "]}},");

    // Obj2: Scale — empty (Type2 only, no ThreeComp40)
    fprintf(fp, "{\"nbytes\":0,\"flags\":0,\"s1\":0,\"s2\":0,\"data\":[],\"vals_a\":[],\"vals\":{\"Type2\":[]}}]");
}

bool MocapExporter::ExportConquestJSON(
    const char* outputPath,
    const char* animName,
    const std::vector<RetargetedFrame>& frames,
    const MocapBridge& bridge,
    float fps)
{
    if (frames.empty()) return false;

    FILE* fp = fopen(outputPath, "w");
    if (!fp) return false;

    int numFrames = (int)frames.size();
    float frameTime = (fps > 0.0f) ? (1.0f / fps) : 0.033333f;
    float duration = (numFrames > 1) ? (float)(numFrames - 1) * frameTime : frameTime;
    int maxFramesPerBlock = 256;

    // Calculate number of blocks
    int numBlocks = (numFrames + maxFramesPerBlock - 1) / maxFramesPerBlock;

    // ============================================================
    // Write top-level JSON
    // ============================================================
    fprintf(fp, "{\n");

    // --- info ---
    fprintf(fp, "  \"info\": {\n");
    fprintf(fp, "    \"key\": \"%s\",\n", animName);
    fprintf(fp, "    \"gamemodemask\": 11,\n");
    fprintf(fp, "    \"offset\": 0,\n");
    fprintf(fp, "    \"size\": 0,\n");
    fprintf(fp, "    \"kind\": 3,\n");
    fprintf(fp, "    \"unk_5\": %f,\n", duration);
    fprintf(fp, "    \"vals_num\": %d,\n", GAME_BONE_COUNT);
    fprintf(fp, "    \"vals2_num\": 0,\n");
    fprintf(fp, "    \"unk_8\": 0,\n");
    fprintf(fp, "    \"vala\": %d,\n", numFrames);
    fprintf(fp, "    \"unk_10\": 1,\n");
    fprintf(fp, "    \"unk_11\": %d,\n", maxFramesPerBlock);
    fprintf(fp, "    \"data_offset\": 0,\n");
    fprintf(fp, "    \"unk_13\": 0,\n");
    fprintf(fp, "    \"unk_14\": 0,\n");
    fprintf(fp, "    \"t_scale\": %f,\n", frameTime);
    fprintf(fp, "    \"block_starts_offset\": 0,\n");
    fprintf(fp, "    \"block_starts_num\": %d,\n", numBlocks);
    fprintf(fp, "    \"block_starts2_offset\": 0,\n");
    fprintf(fp, "    \"block_starts2_num\": %d,\n", numBlocks);
    fprintf(fp, "    \"obj_c3_offset\": 0,\n");
    fprintf(fp, "    \"obj_c3_num\": 0,\n");
    fprintf(fp, "    \"obj_c4_offset\": 0,\n");
    fprintf(fp, "    \"obj_c4_num\": 0,\n");
    fprintf(fp, "    \"block_offset\": 0,\n");
    fprintf(fp, "    \"block_size\": 0,\n");
    fprintf(fp, "    \"obj3_num\": 0,\n");
    fprintf(fp, "    \"obj3_offset\": 0,\n");
    fprintf(fp, "    \"bones_num1\": %d,\n", GAME_BONE_COUNT);
    fprintf(fp, "    \"unk_29\": %d,\n", GAME_BONE_COUNT);
    fprintf(fp, "    \"obj1_num\": 0,\n");
    fprintf(fp, "    \"bones_offset\": 0,\n");
    fprintf(fp, "    \"unk_32\": 0,\n");
    fprintf(fp, "    \"obj1_offset\": 0,\n");
    fprintf(fp, "    \"obj2_offset\": 0,\n");
    fprintf(fp, "    \"obj2_num\": %d,\n", numFrames);
    fprintf(fp, "    \"obj5_offset\": 0\n");
    fprintf(fp, "  },\n");

    // --- obj1 (empty) ---
    fprintf(fp, "  \"obj1\": [],\n");

    // --- obj2 (root motion) ---
    fprintf(fp, "  \"obj2\": [\n");
    for (int f = 0; f < numFrames; ++f)
    {
        fprintf(fp, "    {\"x\": %f, \"y\": %f, \"z\": %f, \"w\": 0.0}",
                frames[f].rootTranslation[0],
                frames[f].rootTranslation[1],
                frames[f].rootTranslation[2]);
        if (f < numFrames - 1) fprintf(fp, ",");
        fprintf(fp, "\n");
    }
    fprintf(fp, "  ],\n");

    // --- events (empty) ---
    fprintf(fp, "  \"events\": [],\n");

    // --- bones ---
    fprintf(fp, "  \"bones\": [\n");
    for (int b = 0; b < GAME_BONE_COUNT; ++b)
    {
        fprintf(fp, "    \"%s\"", s_conquestBones[b]);
        if (b < GAME_BONE_COUNT - 1) fprintf(fp, ",");
        fprintf(fp, "\n");
    }
    fprintf(fp, "  ],\n");

    // --- obj5_a, obj5_b, obj_c3, obj_c4 (empty) ---
    fprintf(fp, "  \"obj5_a\": [],\n");
    fprintf(fp, "  \"obj5_b\": [],\n");
    fprintf(fp, "  \"obj_c3\": [],\n");
    fprintf(fp, "  \"obj_c4\": [],\n");

    // --- blocks ---
    fprintf(fp, "  \"blocks\": [\n");

    for (int block = 0; block < numBlocks; ++block)
    {
        int blockBase = block * maxFramesPerBlock;
        int blockFrameCount = numFrames - blockBase;
        if (blockFrameCount > maxFramesPerBlock) blockFrameCount = maxFramesPerBlock;

        // blocks[block] = [ [bone0, bone1, ...], [] ]
        fprintf(fp, "    [[\n");

        for (int b = 0; b < GAME_BONE_COUNT; ++b)
        {
            fprintf(fp, "      ");

            // Only write rotation tracks for bones that have actual SMPL data.
            // Bones with identity quaternions should be empty so they keep
            // the skeleton's reference pose (which positions the character).
            bool hasMotion = false;
            if (b >= 2 && !frames.empty())
            {
                // Check if this bone has non-identity rotation in any frame
                const float* q0 = frames[0].boneRotations[b];
                // Identity quat = (0,0,0,1)
                float identDist = q0[0]*q0[0] + q0[1]*q0[1] + q0[2]*q0[2] + (q0[3]-1.0f)*(q0[3]-1.0f);
                hasMotion = (identDist > 0.001f);
            }

            if (hasMotion)
            {
                WriteRotationBoneBlock(fp, frames, b, blockBase, blockFrameCount);
            }
            else
            {
                WriteEmptyBoneBlock(fp);
            }

            if (b < GAME_BONE_COUNT - 1) fprintf(fp, ",");
            fprintf(fp, "\n");
        }

        fprintf(fp, "    ],[]]");
        if (block < numBlocks - 1) fprintf(fp, ",");
        fprintf(fp, "\n");
    }

    fprintf(fp, "  ]\n");
    fprintf(fp, "}\n");

    fclose(fp);
    return true;
}
