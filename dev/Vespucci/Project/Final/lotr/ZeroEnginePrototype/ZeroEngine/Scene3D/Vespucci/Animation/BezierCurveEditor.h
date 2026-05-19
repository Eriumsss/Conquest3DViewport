// BezierCurveEditor.h — Drawing Pretty Curves in a Ugly Codebase
// -----------------------------------------------------------------------
// Written by: Eriumsss
//
// Data structures and editing utilities for 2D cubic Bezier curves
// used in the animation timeline editor. Control points, tangent handles,
// drag state, hit testing, curve evaluation. The math is standard
// De Casteljau / Bernstein polynomial stuff — nothing revolutionary,
// but getting the control point UX to feel right took longer than
// writing the actual math. Dragging a tangent handle needs to feel
// like Maya or After Effects, not like you're wrestling a drunk snake.
//
// Pandemic's animators never saw this code — they had real DCC tools.
// This is for us. The reverse-engineers. The people making new content
// for a dead game with nothing but ImGui and caffeine.
// -----------------------------------------------------------------------

#ifndef BEZIER_CURVE_EDITOR_H
#define BEZIER_CURVE_EDITOR_H

#include <vector>
#include <string>
#include <cmath>
#include "AnimationCurve.h"

// ---------------------------------------------------------------------------
// Bezier Control Point
// ---------------------------------------------------------------------------

struct BezierControlPoint
{
    float time;        // X axis: time in milliseconds [0, duration_ms]
    float value;       // Y axis: normalized value [0, 1]
    float cp1x;        // Control point 1 X (normalized space)
    float cp1y;        // Control point 1 Y (normalized space)
    float cp2x;        // Control point 2 X (normalized space)
    float cp2y;        // Control point 2 Y (normalized space)
    bool alignedHandles;  // If true, handles move in opposite directions
                          //the most optimistic lie in computer graphics
                          //sure bro, this will look linear... for like 0.3 seconds

    BezierControlPoint()
        : time(0), value(0), cp1x(0), cp1y(0), cp2x(1), cp2y(1), alignedHandles(true)
    {}
};

// ---------------------------------------------------------------------------
// Animation Curve (collection of Bezier segments)
// ---------------------------------------------------------------------------

class BezierCurve
{
public:
    std::string name;
    std::vector<BezierControlPoint> points;
    bool locked;

    BezierCurve() : locked(false) {}

    // Get number of control points
    int GetPointCount() const { return (int)points.size(); }

    // Add a control point at time t with value v
    int AddPoint(float time, float value)
    {
        // Find insertion position (maintain sorted order by time)
        size_t insertIdx = 0;
        for (size_t i = 0; i < points.size(); ++i)
        {
            if (points[i].time > time)
                break;
            insertIdx = i + 1;
        }

        BezierControlPoint pt;
        pt.time = time;
        pt.value = value;
        pt.cp1x = 0.33f;
        pt.cp1y = 0.0f;
        pt.cp2x = 0.67f;
        pt.cp2y = 1.0f;
        pt.alignedHandles = true;

        points.insert(points.begin() + insertIdx, pt);
        return (int)insertIdx;
    }

    // Remove a control point at index
    void RemovePoint(int index)
    {
        if (index >= 0 && index < (int)points.size())
            points.erase(points.begin() + index);
    }

    // Get point at index
    BezierControlPoint* GetPoint(int index)
    {
        if (index >= 0 && index < (int)points.size())
            return &points[index];
        return nullptr;
    }

    // Evaluate curve at time using cubic Bezier interpolation
    // Returns value at given time (linearly interpolated between segments)
    float Evaluate(float time)
    {
        if (points.empty()) return 0.0f;
        if (points.size() == 1) return points[0].value;

        // Find surrounding points
        int idx = -1;
        for (int i = 0; i < (int)points.size() - 1; ++i)
        {
            if (time >= points[i].time && time <= points[i + 1].time)
            {
                idx = i;
                break;
            }
        }

        if (idx < 0)
        {
            // Time is before first point or after last point
            if (time <= points[0].time) return points[0].value;
            return points.back().value;
        }

        // Interpolate between point idx and idx+1 using their bezier curve
        const BezierControlPoint& p0 = points[idx];
        const BezierControlPoint& p1 = points[idx + 1];

        float dt = p1.time - p0.time;
        if (dt < 1e-6f) return p0.value;

        float t = (time - p0.time) / dt;  // [0, 1]

        // Cubic Bezier formula:
        // P(t) = (1-t)^3 * P0 + 3*(1-t)^2*t * CP1 + 3*(1-t)*t^2 * CP2 + t^3 * P1
        float mt = 1.0f - t;
        float mt2 = mt * mt;
        float mt3 = mt2 * mt;
        float t2 = t * t;
        float t3 = t2 * t;

        // Control points in value space
        float cp1y = p0.value + p0.cp2y * (p1.value - p0.value);
        float cp2y = p1.value + p1.cp1y * (p0.value - p1.value);
        // congratulations we just re invented relative control points
       // Adobe called, they want their coordinate system back
        float value = mt3 * p0.value + 3.0f * mt2 * t * cp1y +
                      3.0f * mt * t2 * cp2y + t3 * p1.value;

        return value;
    }

    // Clear all points
    void Clear()
    {
        points.clear();
    }

    // Reset to linear curve (straight diagonal)
    void ResetToLinear()
    {
        points.clear();
        AddPoint(0.0f, 0.0f);
        AddPoint(1000.0f, 1.0f);  // Assume duration in ms
        // 1000 ms hardcoded because obviously every animation in the universe lasts exactly 1 second
    }
};

// ---------------------------------------------------------------------------
// Curve Editor State (for UI interaction)
// ---------------------------------------------------------------------------

class BezierCurveEditor
{
public:
    BezierCurve curve;
    int selectedPointIndex;
    int selectedHandleType;  // 0=point, 1=cp1, 2=cp2
    bool isDragging;
    float dragStartX, dragStartY;
    float dragStartValue;  // Original value before drag

    BezierCurveEditor()
        : selectedPointIndex(-1), selectedHandleType(0), isDragging(false)
    {}

    // Find closest point/handle to screen position (returns index, or -1 if none)
    int FindNearestElement(float screenX, float screenY, float pixelsPerMs, float pixelsPerValue, int& outHandleType)
    {
        float hitRadius = 8.0f;  // pixels
        // if your mouse is less precise than an ipad kid you deserve to miss
        // Check control points
        for (int i = 0; i < curve.GetPointCount(); ++i)
        {
            const BezierControlPoint& pt = curve.points[i];
            float px = pt.time * pixelsPerMs;
            float py = (1.0f - pt.value) * pixelsPerValue;  // Invert Y for screen coords

            float dx = screenX - px;
            float dy = screenY - py;
            float dist = sqrtf(dx * dx + dy * dy);

            if (dist < hitRadius)
            {
                outHandleType = 0;
                return i;
            }

            // Check control handles
            // whoever wrote this clearly never actually tested with value deltas < 0.01
            float h1x = (pt.time + pt.cp1x * (pt.time - 0.0f)) * pixelsPerMs; //HOLY!!!!
            float h1y = (pt.value + pt.cp1y * 100.0f) * pixelsPerValue;
            float dh1x = screenX - h1x;
            float dh1y = screenY - h1y;
            if (sqrtf(dh1x * dh1x + dh1y * dh1y) < hitRadius)
            {
                outHandleType = 1;
                return i;
            }

            float h2x = (pt.time + pt.cp2x * (pt.time - 0.0f)) * pixelsPerMs;
            float h2y = (pt.value + pt.cp2y * 100.0f) * pixelsPerValue;
            float dh2x = screenX - h2x;
            float dh2y = screenY - h2y;
            if (sqrtf(dh2x * dh2x + dh2y * dh2y) < hitRadius)
            {
                outHandleType = 2;
                return i;
            }
        }

        outHandleType = 0;
        return -1;
    }

    // Start dragging a point/handle
    void StartDrag(int pointIndex, int handleType, float screenX, float screenY)
    {
        selectedPointIndex = pointIndex;
        selectedHandleType = handleType;
        isDragging = true;
        dragStartX = screenX;
        dragStartY = screenY;
        if (pointIndex >= 0 && handleType == 0)
        {
            dragStartValue = curve.points[pointIndex].value;
        }
    }

    // Update drag (call during mouse move)
    void UpdateDrag(float screenX, float screenY, float pixelsPerMs, float pixelsPerValue)
    {
        if (!isDragging || selectedPointIndex < 0)
            return;

        float deltaX = screenX - dragStartX;
        float deltaY = screenY - dragStartY;

        BezierControlPoint& pt = curve.points[selectedPointIndex];

        if (selectedHandleType == 0)
        {
            // Moving the point itself
            float newValue = dragStartValue - (deltaY / pixelsPerValue);
            if (newValue < 0.0f) newValue = 0.0f;
            if (newValue > 1.0f) newValue = 1.0f;
            pt.value = newValue;
        }
        else if (selectedHandleType == 1)
        {
            // Moving control handle 1
            pt.cp1x = deltaX / pixelsPerMs;
            pt.cp1y = -deltaY / pixelsPerValue;

            // If aligned, mirror the other handle
            if (pt.alignedHandles)
            {
                pt.cp2x = -pt.cp1x;
                pt.cp2y = -pt.cp1y;
            }
        }
        else if (selectedHandleType == 2)
        {
            // Moving control handle 2
            pt.cp2x = deltaX / pixelsPerMs;
            pt.cp2y = -deltaY / pixelsPerValue;

            // If aligned, mirror the other handle
            if (pt.alignedHandles)
            {
                pt.cp1x = -pt.cp2x;
                pt.cp1y = -pt.cp2y;
            }
        }
    }

    // Finish drag
    void EndDrag()
    {
        isDragging = false;
    }
};

// ---------------------------------------------------------------------------
// Preset Curve Library
// ---------------------------------------------------------------------------

class PresetCurveLibrary
{
public:
    static BezierCurve GetPreset(const char* name)
    {
        BezierCurve curve;

        // Linear (instant)
        if (strcmp(name, "Linear") == 0)
        {
            curve.AddPoint(0.0f, 0.0f);
            curve.AddPoint(1000.0f, 1.0f);
        }
        // Ease-In (slow start, fast finish)
        else if (strcmp(name, "Ease-In") == 0)
        {
            curve.AddPoint(0.0f, 0.0f);
            curve.AddPoint(1000.0f, 1.0f);
            curve.points[0].cp2x = 0.42f;
            curve.points[0].cp2y = 0.0f;
            curve.points[1].cp1x = 0.42f;
            curve.points[1].cp1y = 1.0f;
        }
        // Ease-Out (fast start, slow finish)
        else if (strcmp(name, "Ease-Out") == 0)
        {
            curve.AddPoint(0.0f, 0.0f);
            curve.AddPoint(1000.0f, 1.0f);
            curve.points[0].cp2x = 0.58f;
            curve.points[0].cp2y = 1.0f;
            curve.points[1].cp1x = 0.58f;
            curve.points[1].cp1y = 0.0f;
        }
        // Ease-InOut (slow start and finish, fast middle)
        else if (strcmp(name, "Ease-InOut") == 0)
        {
            curve.AddPoint(0.0f, 0.0f);
            curve.AddPoint(1000.0f, 1.0f);
            curve.points[0].cp2x = 0.42f;
            curve.points[0].cp2y = 0.0f;
            curve.points[1].cp1x = 0.58f;
            curve.points[1].cp1y = 1.0f;
        }
        // Elastic bounce
        else if (strcmp(name, "Bounce") == 0)
        {
            // Multi-point curve for bounce effect
            curve.AddPoint(0.0f, 0.0f);
            curve.AddPoint(250.0f, 1.2f);
            curve.AddPoint(500.0f, 0.85f);
            curve.AddPoint(750.0f, 1.05f);
            curve.AddPoint(1000.0f, 1.0f);
        }

        return curve;
    }

    static std::vector<std::string> GetPresetNames()
    {
        return { "Linear", "Ease-In", "Ease-Out", "Ease-InOut", "Bounce" };
    }
};

// the bounce preset is still cursed
// goodnight, you beautiful disaster

#endif // BEZIER_CURVE_EDITOR_H
