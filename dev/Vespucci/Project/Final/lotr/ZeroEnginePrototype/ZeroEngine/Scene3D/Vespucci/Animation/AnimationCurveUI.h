// AnimationCurveUI.h — The Animator's Control Panel (ImGui Edition)
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// ImGui widgets for easing type selection, Bezier curve editing, and IK
// tool controls. Comboboxes, sliders, curve preview widgets — all the
// UI pieces that let you tweak animation blending without recompiling.
// Pandemic's animators had their own Maya-based tools. We have ImGui
// and desperation. But our curve editor actually previews the easing
// in real-time, which is more than Pandemic's internal tools did
// (based on what we could infer from their Lua script comments).
//
// "Time you enjoy wasting is not wasted time." — Bertrand Russell
// I spent 3 days making the Bezier control point handles draggable
// with proper snapping. Was it necessary? No. Did it make animating
// feel 10x better? Yes. Bertrand would understand.
// -----------------------------------------------------------------------

#ifndef ANIMATION_CURVE_UI_H
#define ANIMATION_CURVE_UI_H

#include "AnimationCurve.h"
#include "BezierCurveEditor.h"
#include "InverseKinematics.h"
#include <imgui.h>
#include <cstdio>

// ---------------------------------------------------------------------------
// Easing Type Selector (ImGui Combobox)
// ---------------------------------------------------------------------------

inline bool EasingTypeSelector(const char* label, int& easingType)
{
    static const char* easingNames[] = {
        "Linear",
        "Quad In", "Quad Out", "Quad InOut",
        "Cubic In", "Cubic Out", "Cubic InOut",
        "Quart In", "Quart Out", "Quart InOut",
        "Quint In", "Quint Out", "Quint InOut",
        "Sine In", "Sine Out", "Sine InOut",
        "Expo In", "Expo Out", "Expo InOut",
        "Circ In", "Circ Out", "Circ InOut",
        "Elastic In", "Elastic Out",
        "Back In", "Back Out",
        "Bounce In", "Bounce Out",
        "Bezier Custom"
    };

    return ImGui::Combo(label, &easingType, easingNames, EASING_COUNT);
}

// ---------------------------------------------------------------------------
// Bezier Control Point Editor (ImGui Sliders)
// ---------------------------------------------------------------------------

inline void BezierControlPointEditor(const char* label, float& cp1x, float& cp1y, float& cp2x, float& cp2y, bool& aligned)
{
    ImGui::BeginChild(label, ImVec2(300, 200), true);
    ImGui::Text("%s", label);
    ImGui::Separator();

    // Control Point 1
    ImGui::SliderFloat("CP1 X##cp1x", &cp1x, -1.0f, 2.0f);
    ImGui::SliderFloat("CP1 Y##cp1y", &cp1y, -1.0f, 2.0f);

    // Control Point 2
    ImGui::SliderFloat("CP2 X##cp2x", &cp2x, -1.0f, 2.0f);
    ImGui::SliderFloat("CP2 Y##cp2y", &cp2y, -1.0f, 2.0f);

    // Aligned handles toggle
    ImGui::Checkbox("Aligned Handles", &aligned);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Link handles so they move mirrored");

    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Bezier Curve Preview (Simple ASCII Graph)
// ---------------------------------------------------------------------------

inline void BezierCurvePreview(BezierCurve& curve)
{
    ImGui::Text("Curve Preview (ASCII):");
    ImGui::TextDisabled("   0.0 |");

    // Draw ASCII curve (10x high)
    for (int row = 9; row >= 0; --row)
    {
        float rowValue = row / 10.0f;
        ImGui::Text("   %.1f |", rowValue);
        ImGui::SameLine();

        // Evaluate curve at 20 points across this row
        for (int col = 0; col < 20; ++col)
        {
            float t = col / 20.0f;
            float value = curve.Evaluate(t * 1000.0f);

            if (fabsf(value - rowValue) < 0.055f)
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "█");
            else
                ImGui::TextColored(ImVec4(0.3f, 0.3f, 0.3f, 1.0f), "·");
            if (col < 19) ImGui::SameLine(0, 0);
        }
    }

    ImGui::Text("   0.0 +");
    ImGui::SameLine();
    for (int i = 0; i < 20; ++i) ImGui::Text("-");

    ImGui::Text("         0ms        500ms       1000ms");
}

// ---------------------------------------------------------------------------
// Curve Editor Window (ImGui)
// ---------------------------------------------------------------------------

inline void CurveEditorWindow(BezierCurveEditor& editor, bool& showWindow, const char* windowName = "Curve Editor")
{
    if (!showWindow) return;

    if (ImGui::Begin(windowName, &showWindow, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Bezier Motion Curve Editor");
        ImGui::Separator();

        // Curve list
        ImGui::Text("Points: %d", editor.curve.GetPointCount());
        if (ImGui::Button("Add Point at 0.5s"))
        {
            editor.curve.AddPoint(500.0f, editor.curve.Evaluate(500.0f));
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear"))
        {
            editor.curve.Clear();
        }

        ImGui::Separator();

        // Point list with delete buttons
        ImGui::BeginChild("Points", ImVec2(0, 150), true);
        for (int i = 0; i < editor.curve.GetPointCount(); ++i)
        {
            BezierControlPoint* pt = editor.curve.GetPoint(i);
            if (!pt) continue;

            ImGui::PushID(i);
            char label[32];
            sprintf_s(label, sizeof(label), "Point %d", i);

            if (ImGui::TreeNode(label))
            {
                ImGui::SliderFloat("Time (ms)##time", &pt->time, 0.0f, 1000.0f);
                ImGui::SliderFloat("Value##val", &pt->value, 0.0f, 1.0f);
                ImGui::SliderFloat("CP1 X##cp1x", &pt->cp1x, -1.0f, 2.0f);
                ImGui::SliderFloat("CP1 Y##cp1y", &pt->cp1y, -2.0f, 2.0f);
                ImGui::SliderFloat("CP2 X##cp2x", &pt->cp2x, -1.0f, 2.0f);
                ImGui::SliderFloat("CP2 Y##cp2y", &pt->cp2y, -2.0f, 2.0f);
                ImGui::Checkbox("Aligned Handles", &pt->alignedHandles);

                if (ImGui::Button("Delete"))
                {
                    editor.curve.RemovePoint(i);
                }

                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::Separator();

        // Preview
        BezierCurvePreview(editor.curve);

        ImGui::Separator();

        // Preset curves
        ImGui::Text("Preset Curves:");
        if (ImGui::Button("Ease-In"))
        {
            editor.curve = PresetCurveLibrary::GetPreset("Ease-In");
        }
        ImGui::SameLine();
        if (ImGui::Button("Ease-Out"))
        {
            editor.curve = PresetCurveLibrary::GetPreset("Ease-Out");
        }
        ImGui::SameLine();
        if (ImGui::Button("Ease-InOut"))
        {
            editor.curve = PresetCurveLibrary::GetPreset("Ease-InOut");
        }
        ImGui::SameLine();
        if (ImGui::Button("Bounce"))
        {
            editor.curve = PresetCurveLibrary::GetPreset("Bounce");
        }

        ImGui::End();
    }
}

// ---------------------------------------------------------------------------
// IK Chain Editor Window (ImGui)
// ---------------------------------------------------------------------------

inline void IKEditorWindow(hkObjectArray<IKChain>& chains, int& selectedChainIndex, bool& showWindow, const char* windowName = "IK Editor")
{
    if (!showWindow) return;

    if (ImGui::Begin(windowName, &showWindow, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Inverse Kinematics Editor");
        ImGui::Separator();

        // Chain list
        ImGui::Text("IK Chains: %d", chains.getSize());
        if (ImGui::Button("Add IK Chain"))
        {
            IKChain newChain;
            newChain.name = "New Chain";
            chains.pushBack(newChain);
            selectedChainIndex = chains.getSize() - 1;
        }

        ImGui::Separator();

        // Chain selector
        if (!chains.isEmpty())
        {
            ImGui::BeginChild("Chains", ImVec2(250, 150), true);
            for (int i = 0; i < chains.getSize(); ++i)
            {
                if (ImGui::Selectable(chains[i].name.c_str(), selectedChainIndex == i))
                {
                    selectedChainIndex = i;
                }
            }
            ImGui::EndChild();

            ImGui::Separator();

            // Edit selected chain
            if (selectedChainIndex >= 0 && selectedChainIndex < chains.getSize())
            {
                IKChain& chain = chains[selectedChainIndex];

                char nameBuffer[64];
                strcpy_s(nameBuffer, sizeof(nameBuffer), chain.name.c_str());
                if (ImGui::InputText("Chain Name", nameBuffer, sizeof(nameBuffer)))
                {
                    chain.name = nameBuffer;
                }

                ImGui::Text("Bones in chain: %d", (int)chain.boneIndices.size());

                ImGui::SliderInt("Max Iterations", &chain.maxIterations, 1, 50);
                ImGui::SliderFloat("Tolerance", &chain.tolerance, 0.001f, 0.1f, "%.4f");

                ImGui::Separator();
                ImGui::Text("Target Position:");
                ImGui::DragFloat("Target X##tx", &chain.targetPosition(0), 0.01f);
                ImGui::DragFloat("Target Y##ty", &chain.targetPosition(1), 0.01f);
                ImGui::DragFloat("Target Z##tz", &chain.targetPosition(2), 0.01f);

                ImGui::Separator();
                ImGui::Checkbox("Enable IK", &chain.enabled);
                ImGui::Checkbox("Use Pole Vector", &chain.usePoleVector);

                if (chain.usePoleVector)
                {
                    ImGui::Text("Pole Vector Position:");
                    ImGui::DragFloat("Pole X##px", &chain.poleVectorPosition(0), 0.01f);
                    ImGui::DragFloat("Pole Y##py", &chain.poleVectorPosition(1), 0.01f);
                    ImGui::DragFloat("Pole Z##pz", &chain.poleVectorPosition(2), 0.01f);
                }

                ImGui::Separator();
                ImGui::Text("Joint Constraints:");
                ImGui::Indent();
                for (int j = 0; j < chain.constraints.getSize() && j < 5; ++j)
                {
                    JointConstraint& constraint = chain.constraints[j];
                    ImGui::PushID(j);

                    char jointLabel[32];
                    sprintf_s(jointLabel, sizeof(jointLabel), "Joint %d", j);
                    if (ImGui::TreeNode(jointLabel))
                    {
                        ImGui::Checkbox("Enabled##enabled", &constraint.enabled);
                        ImGui::SliderFloat("Min Angle##min", &constraint.minAngleDeg, -180.0f, 0.0f);
                        ImGui::SliderFloat("Max Angle##max", &constraint.maxAngleDeg, 0.0f, 180.0f);
                        ImGui::TreePop();
                    }

                    ImGui::PopID();
                }
                ImGui::Unindent();

                if (ImGui::Button("Delete Chain"))
                {
                    chains.removeAtAndCopy(selectedChainIndex);
                    selectedChainIndex = -1;
                }
            }
        }

        ImGui::End();
    }
}

// ---------------------------------------------------------------------------
// Keyframe Inspector (Show/Edit Easing on Selected Key)
// ---------------------------------------------------------------------------

inline void KeyframeInspectorWindow(Scene3DRenderer::EditorKey* selectedKey, bool& showWindow, const char* windowName = "Keyframe Inspector")
{
    if (!showWindow || !selectedKey) return;

    if (ImGui::Begin(windowName, &showWindow, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Keyframe Inspector");
        ImGui::Separator();

        ImGui::Text("Frame: %d", selectedKey->frame);
        ImGui::Text("Time: %.2f ms", selectedKey->timeMs);

        ImGui::Separator();
        ImGui::Text("Rotation (Quaternion):");
        ImGui::Text("  X: %.4f", selectedKey->rot[0]);
        ImGui::Text("  Y: %.4f", selectedKey->rot[1]);
        ImGui::Text("  Z: %.4f", selectedKey->rot[2]);
        ImGui::Text("  W: %.4f", selectedKey->rot[3]);

        ImGui::Separator();
        ImGui::Text("Easing Settings:");

        // Easing type selector
        EasingTypeSelector("Easing Type", selectedKey->easingType);

        // Show current easing function name
        ImGui::TextDisabled("Current: %s", GetEasingName(selectedKey->easingType));

        // If custom Bezier, show control points
        if (selectedKey->easingType == EASING_BEZIER_CUBIC)
        {
            ImGui::Separator();
            ImGui::Text("Bezier Control Points:");
            ImGui::SliderFloat("CP1 X", &selectedKey->easingCp1x, -1.0f, 2.0f);
            ImGui::SliderFloat("CP1 Y", &selectedKey->easingCp1y, -1.0f, 2.0f);
            ImGui::SliderFloat("CP2 X", &selectedKey->easingCp2x, -1.0f, 2.0f);
            ImGui::SliderFloat("CP2 Y", &selectedKey->easingCp2y, -1.0f, 2.0f);
        }

        // Preview easing curve
        ImGui::Separator();
        ImGui::Text("Ease Preview:");
        ImGui::Text("   0% -> %.1f%%", EvaluateEasing(0.0f, selectedKey->easingType) * 100.0f);
        ImGui::Text("  25% -> %.1f%%", EvaluateEasing(0.25f, selectedKey->easingType) * 100.0f);
        ImGui::Text("  50% -> %.1f%%", EvaluateEasing(0.5f, selectedKey->easingType) * 100.0f);
        ImGui::Text("  75% -> %.1f%%", EvaluateEasing(0.75f, selectedKey->easingType) * 100.0f);
        ImGui::Text(" 100% -> %.1f%%", EvaluateEasing(1.0f, selectedKey->easingType) * 100.0f);

        ImGui::End();
    }
}

// ---------------------------------------------------------------------------
// Animation Easing Panel (Quick Access)
// ---------------------------------------------------------------------------

inline void AnimationEasingPanel()
{
    ImGui::BeginChild("EasingPanel", ImVec2(0, 150), true);
    ImGui::Text("Common Easing Functions:");

    static int hoveredEasing = -1;

    // Show buttons for common easings
    struct EasingQuick { const char* label; int type; };
    EasingQuick quickEasings[] = {
        { "None", EASING_LINEAR },
        { "Ease Out", EASING_CUBIC_OUT },
        { "Ease In", EASING_CUBIC_IN },
        { "Ease In-Out", EASING_CUBIC_INOUT },
        { "Bounce", EASING_BOUNCE_OUT },
        { "Elastic", EASING_ELASTIC_OUT }
    };

    for (int i = 0; i < 6; ++i)
    {
        if (ImGui::Button(quickEasings[i].label, ImVec2(80, 20)))
        {
            // Application code would set the selected keyframe's easing here
        }
        if (i < 5) ImGui::SameLine();

        if (ImGui::IsItemHovered())
        {
            hoveredEasing = quickEasings[i].type;
            ImGui::SetTooltip("%s", GetEasingName(quickEasings[i].type));
        }
    }

    ImGui::EndChild();
}

#endif // ANIMATION_CURVE_UI_H
