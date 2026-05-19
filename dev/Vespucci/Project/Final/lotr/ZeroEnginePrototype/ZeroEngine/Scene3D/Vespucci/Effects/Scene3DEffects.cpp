// Scene3DEffects.cpp — Spawning, Aging, and Killing Thousands of Particles Per Frame
// -----------------------------------------------------------------------
// Written by: Eriumsss
// Particle system implementation: emitter tick, particle spawn, velocity
// integration, color/size curve evaluation, billboard rotation, alpha
// fade, death check, buffer compaction. Every particle lives fast and
// dies young. 0.3 seconds average. Thousands per frame. The constant
// churn of creation and destruction is the most honest code in the
// whole goddamn codebase. Born, updated, rendered, removed. No drama.
// No hidden state. No haunted pointers. Just math and death.
// -----------------------------------------------------------------------

#include "Scene3DEffects.h"
#include "ZeroMath.h"
#include <Graphics/Common/Texture/hkgTexture.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

// EffectColor implementation
EffectColor EffectColor::fromHex(const char* hex)
{
    if (!hex || hex[0] == '\0') return EffectColor();
    
    // Parse "0xAARRGGBB" format
    unsigned int value = 0;
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
        sscanf_s(hex + 2, "%x", &value);
    else
        sscanf_s(hex, "%x", &value);
    
    unsigned char a = (value >> 24) & 0xFF;
    unsigned char r = (value >> 16) & 0xFF;
    unsigned char g = (value >> 8) & 0xFF;
    unsigned char b = value & 0xFF;
    
    return EffectColor(a, r, g, b);
}

EffectColor EffectColor::lerp(const EffectColor& a, const EffectColor& b, float t)
{
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    
    return EffectColor(
        (unsigned char)(a.a + (b.a - a.a) * t),
        (unsigned char)(a.r + (b.r - a.r) * t),
        (unsigned char)(a.g + (b.g - a.g) * t),
        (unsigned char)(a.b + (b.b - a.b) * t)
    );
}

// Particle implementation
Particle::Particle()
{
    position.setZero4();
    velocity.setZero4();
    color = EffectColor();
    startColor = EffectColor();
    endColor = EffectColor();
    colorMultiplier = 1.0f;
    width = 1.0f;
    height = 1.0f;
    startWidth = 1.0f;
    startHeight = 1.0f;
    endWidth = 1.0f;
    endHeight = 1.0f;
    pivotX = 0.5f;
    pivotY = 0.5f;
    rotation.setZero4();
    angularVelStart.setZero4();
    angularVelEnd.setZero4();
    lifetime = 0.0f;
    maxLifetime = 1.0f;
    currentFrame = 0;
    animationTime = 0.0f;
    alphaTestStart = 0;
    alphaTestEnd = 0;
}

void Particle::update(float deltaTime)
{
    // Update lifetime
    lifetime += deltaTime;

    // Update position (physics integration)
    hkVector4 deltaPos;
    deltaPos.setMul4(deltaTime, velocity);
    position.add4(deltaPos);

    // Update rotation (interpolate angular velocity over lifetime)
    float t = 0.0f;
    if (maxLifetime > 0.000001f)
        t = lifetime / maxLifetime;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    hkVector4 angVel = angularVelStart;
    hkVector4 diff;
    diff.setSub4(angularVelEnd, angularVelStart);
    diff.mul4(t);
    angVel.add4(diff);

    hkVector4 deltaRot;
    deltaRot.setMul4(deltaTime, angVel);
    rotation.add4(deltaRot);

    // Update texture animation
    animationTime += deltaTime;
}

// EffectEffector implementation
EffectEffector::EffectEffector()
{
    guid = 0;
    typeId = 0;
    containing = false;
    isEmitterEffector = false;
    force = 0.0f;
    damping = 0.0f;
    bounce = 1.0f;
    height = 0.0f;
    constrict = 0.0f;
    position.setZero4();
    axisY.set(0.0f, 1.0f, 0.0f, 0.0f);
    transformScale = 1.0f;
    controllerCount = 0;
    for (int i = 0; i < 16; ++i)
        controllerGuids[i] = 0;
    forceSegmentCount = 0;
}

// ParticleEmitter implementation
ParticleEmitter::ParticleEmitter()
{
    name[0] = '\0';
    textureName[0] = '\0';
    texture = HK_NULL;
    usesSharedAtlas = false;
    // atlasUVRect defaults to (0,0,1,1) via constructor

    refract = false;
    refractionScale = 1.0f;
    refractionTextureName[0] = '\0';
    refractionTexture = HK_NULL;
    usesSharedAtlasRefraction = false;
    // refractionAtlasUVRect defaults to (0,0,1,1) via constructor

    useAtlas = false;
    frameCount = 1;
    minFrame = 0;
    maxFrame = 0;
    playbackTime = 1.0f;
    loopTextureAnim = true;
    randomizeStartFrame = false;

    uvColumns = 1;
    uvRows = 1;
    animFrameMin = 0;
    animFrameMax = 0;
    
    particleType = BILLBOARD;
    orientMode = ORIENT_NONE;
    blendMode = NORMAL;
    
    rateMin = rateMax = 1.0f;
    lifespanMin = lifespanMax = 60;
    fadeInFrames = 0;
    initialVelocityContribution = 0.0f;
    sortParticles = false;
    
    startWidthMin = startWidthMax = 1.0f;
    startHeightMin = startHeightMax = 1.0f;
    endWidthMin = endWidthMax = 1.0f;
    endHeightMin = endHeightMax = 1.0f;

    pivotXMin = pivotXMax = 0.5f;
    pivotYMin = pivotYMax = 0.5f;
    
    startColorMin = startColorMax = EffectColor(255, 255, 255, 255);
    endColorMin = endColorMax = EffectColor(0, 255, 255, 255);
    startColorMultiplier = 1.0f;
    endColorMultiplier = 1.0f;

    startAlphaTestMin = 0;
    startAlphaTestMax = 0;
    endAlphaTestMin = 0;
    endAlphaTestMax = 0;
    
    initialOrientationMin.setZero4();
    initialOrientationMax.setZero4();
    startRotationVelocityMin.setZero4();
    startRotationVelocityMax.setZero4();
    endRotationVelocityMin.setZero4();
    endRotationVelocityMax.setZero4();
    
    speedMin = speedMax = 0.0f;
    acceleration = 0.0f;
    externalAcceleration.setZero4();
    externalDamping = 0.0f;
    effectors.clear();
    emitterEffectors.clear();
    
    emitterType = POINT;
    volumeEmit = false;
    height = 1.0f;
    angle = 45.0f;
    axisY.set(0.0f, 1.0f, 0.0f, 0.0f);
    
    position.setZero4();
    rotation.setZero4();
    localOffset.setZero4();
    transformScale = 1.0f;
    
    spawnAccumulator = 0.0f;
}

ParticleEmitter::~ParticleEmitter()
{
    hkgTexture* diffuse = texture;

    if (diffuse && !usesSharedAtlas)
    {
        diffuse->removeReference();
    }

    if (refractionTexture && !usesSharedAtlasRefraction && refractionTexture != diffuse)
    {
        refractionTexture->removeReference();
    }

    texture = HK_NULL;
    refractionTexture = HK_NULL;
}

void ParticleEmitter::spawn(int count)
{
    for (int i = 0; i < count; i++)
    {
        Particle p;

        // NOTE: Conquest effect JSON mixes units:
        //  - Emitter shape sizes (Height/Angle) are authored in centimeters
        //  - Particle quad sizes (StartWidth/Height etc.) are authored in centimeters
        //  - Simulation runs at a fixed 30 FPS, so many rates use frame-based units
        float uniformScale = transformScale;
        if (uniformScale < 0.0f) uniformScale = -uniformScale;

        // Convert authored centimeters -> renderer world units (meters-ish), and apply the emitter transform scale.
        // If we skip this conversion, circle/cylinder/sphere emitters spawn particles over a 100x larger area,
        // which shows up as "2D walls everywhere" and can white-out the frame with additive blends.
        float shapeScale = uniformScale * 0.01f;

        // Initialize position based on emitter type
        switch (emitterType)
        {
        case POINT:
            p.position = position;
            break;

        case SPHERE:
        {
            // Random point on/in sphere (Y-up)
            float theta = ((float)rand() / RAND_MAX) * 2.0f * 3.14159f;
            float u = (float)rand() / RAND_MAX;
            float v = (float)rand() / RAND_MAX;
            float cosPhi = 1.0f - 2.0f * u; // [-1, 1]
            float sinPhiSq = 1.0f - cosPhi * cosPhi;
            if (sinPhiSq < 0.0f) sinPhiSq = 0.0f;
            float sinPhi = sqrtf(sinPhiSq);

            float radius = height * shapeScale;
            float r = radius;
            if (volumeEmit)
            {
                // Rough uniform distribution inside the sphere volume
                float t = (float)rand() / RAND_MAX;
                r *= powf(t, 1.0f / 3.0f);
            }

            float x = r * sinPhi * cosf(theta);
            float y = r * cosPhi;
            float z = r * sinPhi * sinf(theta);

            hkVector4 offset;
            offset.set(x, y, z, 0.0f);
            p.position.setAdd4(position, offset);
            break;
        }

        case DOME:
        {
            // Random point on/in a dome (spherical cap / hemisphere), Y-up.
            // 'height' is the radius in game units, 'angle' is the cap angle in degrees (0..180).
            float theta = ((float)rand() / RAND_MAX) * 2.0f * 3.14159f;

            float maxPhi = angle * 3.14159f / 180.0f;
            if (maxPhi < 0.0f) maxPhi = 0.0f;
            if (maxPhi > 3.14159f) maxPhi = 3.14159f;

            float cosPhiMin = cosf(maxPhi);
            float u = (float)rand() / RAND_MAX;
            // Uniform on cap surface: cos(phi) uniform in [cosPhiMin, 1]
            float cosPhi = 1.0f - u * (1.0f - cosPhiMin);
            float sinPhiSq = 1.0f - cosPhi * cosPhi;
            if (sinPhiSq < 0.0f) sinPhiSq = 0.0f;
            float sinPhi = sqrtf(sinPhiSq);

            float radius = height * shapeScale;
            float r = radius;
            if (volumeEmit)
            {
                // Rough uniform distribution inside the dome volume
                float t = (float)rand() / RAND_MAX;
                r *= powf(t, 1.0f / 3.0f);
            }

            float x = r * sinPhi * cosf(theta);
            float y = r * cosPhi;
            float z = r * sinPhi * sinf(theta);

            hkVector4 offset;
            offset.set(x, y, z, 0.0f);
            p.position.setAdd4(position, offset);
            break;
        }

        case CONE:
        {
            // Random point in cone
            float theta = ((float)rand() / RAND_MAX) * 2.0f * 3.14159f;
            float coneHeight = height * shapeScale;
            float h = ((float)rand() / RAND_MAX) * coneHeight;
            float maxR = h * tanf(angle * 3.14159f / 180.0f);
            float r = volumeEmit ? ((float)rand() / RAND_MAX) * maxR : maxR;

            float x = r * cosf(theta);
            float y = h;
            float z = r * sinf(theta);

            hkVector4 offset;
            offset.set(x, y, z, 0.0f);
            p.position.setAdd4(position, offset);
            break;
        }

        case CIRCLE:
        {
            // Random point on a circle (ring) on XZ plane, with optional vertical thickness.
            float theta = ((float)rand() / RAND_MAX) * 2.0f * 3.14159f;

            // Conquest data uses Angle as the circle radius (cm).
            float radius = angle * shapeScale;
            if (volumeEmit)
                radius *= ((float)rand() / RAND_MAX); // Fill interior

            float x = radius * cosf(theta);
            float y = 0.0f;
            if (height > 0.0f)
            {
                // Height is used as a small thickness band (cm) around the emission plane.
                y = (((float)rand() / RAND_MAX) - 0.5f) * (height * shapeScale);
            }
            float z = radius * sinf(theta);

            hkVector4 offset;
            offset.set(x, y, z, 0.0f);
            p.position.setAdd4(position, offset);
            break;
        }

        case CYLINDER:
        {
            // Random point on/in a cylinder aligned with Y axis.
            // Conquest data uses Height (cm) and Angle (cm-ish radius) for cylinders (legacy naming).
            float theta = ((float)rand() / RAND_MAX) * 2.0f * 3.14159f;

            float cylHeight = height * shapeScale;
            float cylRadius = angle * shapeScale;

            float y = (((float)rand() / RAND_MAX) - 0.5f) * cylHeight;

            float r = cylRadius;
            if (volumeEmit)
            {
                // Uniform within disk area.
                float t = (float)rand() / RAND_MAX;
                r *= sqrtf(t);
            }

            float x = r * cosf(theta);
            float z = r * sinf(theta);

            hkVector4 offset;
            offset.set(x, y, z, 0.0f);
            p.position.setAdd4(position, offset);
            break;
        }

        case POLYGON:
        {
            // Approximate polygon emitter as a circle for now.
            float theta = ((float)rand() / RAND_MAX) * 2.0f * 3.14159f;

            // Conquest data uses Angle as the polygon's circumscribed radius (cm) in most FX.
            float radius = angle * shapeScale;
            if (volumeEmit)
            {
                float t = (float)rand() / RAND_MAX;
                radius *= sqrtf(t);
            }

            float x = radius * cosf(theta);
            float y = 0.0f;
            if (height > 0.0f)
                y = (((float)rand() / RAND_MAX) - 0.5f) * (height * shapeScale);
            float z = radius * sinf(theta);

            hkVector4 offset;
            offset.set(x, y, z, 0.0f);
            p.position.setAdd4(position, offset);
            break;
        }

        case DIRECTION:
            // Direction emitters spawn at the emitter origin; direction affects velocity.
            p.position = position;
            break;

        case MESH_EMITTER:
            // For now, just use point position (mesh emitter needs mesh data)
            p.position = position;
            break;
        }

        // Initialize velocity
        float speed = speedMin + ((float)rand() / RAND_MAX) * (speedMax - speedMin);
        // Speed is authored in world units per second (meters-ish). Apply the emitter's uniform scale.
        float speedWorld = speed * uniformScale;
        if (speed > 0.0f)
        {
            // Random direction (unit vector)
            float theta = ((float)rand() / RAND_MAX) * 2.0f * 3.14159f;
            float phi = ((float)rand() / RAND_MAX) * 3.14159f;

            float rx = sinf(phi) * cosf(theta);
            float ry = sinf(phi) * sinf(theta);
            float rz = cosf(phi);

            // Optional: bias initial velocity towards emitter orientation axis.
            float contrib = initialVelocityContribution;
            if (contrib < 0.0f) contrib = 0.0f;
            if (contrib > 1.0f) contrib = 1.0f;

            if (emitterType == DIRECTION)
                contrib = 1.0f;

            float bx = axisY(0);
            float by = axisY(1);
            float bz = axisY(2);
            float bl2 = bx*bx + by*by + bz*bz;
            if (bl2 < 1e-6f)
            {
                bx = 0.0f; by = 1.0f; bz = 0.0f;
            }
            else
            {
                ZNormalize3f(bx, by, bz);
            }

            float vx = rx * (1.0f - contrib) + bx * contrib;
            float vy = ry * (1.0f - contrib) + by * contrib;
            float vz = rz * (1.0f - contrib) + bz * contrib;

            float vl2 = vx*vx + vy*vy + vz*vz;
            if (vl2 < 1e-6f)
            {
                vx = bx; vy = by; vz = bz;
            }
            else
            {
                ZNormalize3f(vx, vy, vz);
            }

            p.velocity.set(vx * speedWorld, vy * speedWorld, vz * speedWorld, 0.0f);
        }
        else
        {
            p.velocity.setZero4();
        }

        // Initialize lifetime
        int lifespanFrames = lifespanMin;
        if (lifespanMax > lifespanMin)
            lifespanFrames += rand() % (lifespanMax - lifespanMin + 1);
        p.maxLifetime = (float)lifespanFrames / 30.0f; // Convert frames to seconds (game runs at 30 FPS)

        // Initialize size (separate width and height for non-square particles like streaks)
        p.startWidth = startWidthMin + ((float)rand() / RAND_MAX) * (startWidthMax - startWidthMin);
        p.startHeight = startHeightMin + ((float)rand() / RAND_MAX) * (startHeightMax - startHeightMin);
        p.endWidth = endWidthMin + ((float)rand() / RAND_MAX) * (endWidthMax - endWidthMin);
        p.endHeight = endHeightMin + ((float)rand() / RAND_MAX) * (endHeightMax - endHeightMin);

        // Sizes are authored in centimeters; convert to renderer units and apply WorldTransform scale.
        p.width = p.startWidth * 0.01f * uniformScale;
        p.height = p.startHeight * 0.01f * uniformScale;

        // Initialize pivot point (0..1). 0=bottom/left, 1=top/right.
        float px = pivotXMin + ((float)rand() / RAND_MAX) * (pivotXMax - pivotXMin);
        float py = pivotYMin + ((float)rand() / RAND_MAX) * (pivotYMax - pivotYMin);
        if (px < 0.0f) px = 0.0f; else if (px > 1.0f) px = 1.0f;
        if (py < 0.0f) py = 0.0f; else if (py > 1.0f) py = 1.0f;
        p.pivotX = px;
        p.pivotY = py;

        // Initialize color
        float startBlend = (float)rand() / RAND_MAX;
        float endBlend = (float)rand() / RAND_MAX;
        p.startColor = EffectColor::lerp(startColorMin, startColorMax, startBlend);
        p.endColor = EffectColor::lerp(endColorMin, endColorMax, endBlend);
        p.color = p.startColor;
        p.colorMultiplier = startColorMultiplier;

        // Initialize rotation (Euler degrees -> radians)
        float rotX = initialOrientationMin(0) + ((float)rand() / RAND_MAX) *
                     (initialOrientationMax(0) - initialOrientationMin(0));
        float rotY = initialOrientationMin(1) + ((float)rand() / RAND_MAX) *
                     (initialOrientationMax(1) - initialOrientationMin(1));
        float rotZ = initialOrientationMin(2) + ((float)rand() / RAND_MAX) *
                     (initialOrientationMax(2) - initialOrientationMin(2));
        p.rotation.set(rotX * 3.14159f / 180.0f,
                       rotY * 3.14159f / 180.0f,
                       rotZ * 3.14159f / 180.0f,
                       0.0f);

        // Angular velocity can vary over lifetime (start -> end).
        float svx = startRotationVelocityMin(0) + ((float)rand() / RAND_MAX) *
                    (startRotationVelocityMax(0) - startRotationVelocityMin(0));
        float svy = startRotationVelocityMin(1) + ((float)rand() / RAND_MAX) *
                    (startRotationVelocityMax(1) - startRotationVelocityMin(1));
        float svz = startRotationVelocityMin(2) + ((float)rand() / RAND_MAX) *
                    (startRotationVelocityMax(2) - startRotationVelocityMin(2));
        p.angularVelStart.set(svx * 3.14159f / 180.0f,
                              svy * 3.14159f / 180.0f,
                              svz * 3.14159f / 180.0f,
                              0.0f);

        float evx = endRotationVelocityMin(0) + ((float)rand() / RAND_MAX) *
                    (endRotationVelocityMax(0) - endRotationVelocityMin(0));
        float evy = endRotationVelocityMin(1) + ((float)rand() / RAND_MAX) *
                    (endRotationVelocityMax(1) - endRotationVelocityMin(1));
        float evz = endRotationVelocityMin(2) + ((float)rand() / RAND_MAX) *
                    (endRotationVelocityMax(2) - endRotationVelocityMin(2));
        p.angularVelEnd.set(evx * 3.14159f / 180.0f,
                            evy * 3.14159f / 180.0f,
                            evz * 3.14159f / 180.0f,
                            0.0f);

        // Initialize texture animation
        p.currentFrame = animFrameMin;
        p.animationTime = 0.0f;
        if (uvColumns * uvRows > 1 && (animFrameMax > animFrameMin))
        {
            int range = animFrameMax - animFrameMin + 1;
            if (range < 1) range = 1;

            if (randomizeStartFrame)
            {
                int offset = (range > 1) ? (rand() % range) : 0;
                p.currentFrame = animFrameMin + offset;

                // Optional: randomize animation phase so looping anims don't sync up.
                if (loopTextureAnim && playbackTime > 0.0f)
                {
                    float loopLenSec = playbackTime / 30.0f;
                    if (loopLenSec > 0.0f)
                        p.animationTime = ((float)offset / (float)range) * loopLenSec;
                }
            }
        }

        // Initialize alpha test (per-particle threshold, 0..255)
        {
            int sMin = startAlphaTestMin;
            int sMax = startAlphaTestMax;
            if (sMin < 0) sMin = 0;
            if (sMax < 0) sMax = 0;
            if (sMin > 255) sMin = 255;
            if (sMax > 255) sMax = 255;
            if (sMax < sMin) { int t = sMax; sMax = sMin; sMin = t; }

            int eMin = endAlphaTestMin;
            int eMax = endAlphaTestMax;
            if (eMin < 0) eMin = 0;
            if (eMax < 0) eMax = 0;
            if (eMin > 255) eMin = 255;
            if (eMax > 255) eMax = 255;
            if (eMax < eMin) { int t = eMax; eMax = eMin; eMin = t; }

            int sRange = sMax - sMin;
            int eRange = eMax - eMin;
            p.alphaTestStart = (sRange > 0) ? (sMin + (rand() % (sRange + 1))) : sMin;
            p.alphaTestEnd = (eRange > 0) ? (eMin + (rand() % (eRange + 1))) : eMin;
        }

        particles.pushBack(p);
    }
}

static float Clamp01(float x)
{
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

static float EvaluateFloatSegments(float baseValue, const FloatSegment* segs, int segCount, float timeFrames)
{
    if (!segs || segCount <= 0)
        return baseValue;

    float bestFactor = 0.0f;
    float bestValue = baseValue;

    for (int i = 0; i < segCount; ++i)
    {
        const FloatSegment& seg = segs[i];
        if (timeFrames < (float)seg.startFrame || timeFrames > (float)seg.endFrame)
            continue;

        float factor = 1.0f;
        if (seg.fadeInFrames > 0)
        {
            float fin = (timeFrames - (float)seg.startFrame) / (float)seg.fadeInFrames;
            factor = (fin < factor) ? fin : factor;
        }
        if (seg.fadeOutFrames > 0)
        {
            float fout = ((float)seg.endFrame - timeFrames) / (float)seg.fadeOutFrames;
            factor = (fout < factor) ? fout : factor;
        }
        factor = Clamp01(factor);

        if (factor > bestFactor)
        {
            bestFactor = factor;
            bestValue = seg.value;
        }
    }

    if (bestFactor <= 0.0f)
        return baseValue;

    return baseValue + (bestValue - baseValue) * bestFactor;
}

void ParticleEmitter::update(float deltaTime, float effectiveRatePerFrame, float effectTimeSeconds)
{
    // Update spawn accumulator
    // Rates in LOTR: Conquest effect data are authored per-frame at 30 FPS (fixed-step game).
    // Our viewport runs with variable delta time, so convert to "frames" by multiplying by 30.
    float rate = effectiveRatePerFrame;
    if (rate < 0.0f) rate = 0.0f;
    spawnAccumulator += rate * deltaTime * 30.0f;

    int spawnCount = (int)spawnAccumulator;
    if (spawnCount > 0)
    {
        spawn(spawnCount);
        spawnAccumulator -= (float)spawnCount;
    }

    const bool hasPerParticleEffectors = (effectors.getSize() > 0 || emitterEffectors.getSize() > 0);
    hkVector4 effectOrigin;
    effectOrigin.setSub4(position, localOffset);
    const float timeFrames = effectTimeSeconds * 30.0f;

    // Update all particles
    for (int i = 0; i < particles.getSize(); i++)
    {
        Particle& p = particles[i];

        // Update particle physics
        p.update(deltaTime);

        // Apply acceleration (gravity, wind, etc.)
        if (acceleration != 0.0f)
        {
            // Acceleration is authored in world units per second^2 (meters-ish). Apply uniform scale.
            float uniformScale = transformScale;
            if (uniformScale < 0.0f) uniformScale = -uniformScale;
            float accelWorld = acceleration * uniformScale;
            hkVector4 accel;
            accel.set(0.0f, accelWorld * deltaTime, 0.0f, 0.0f);
            p.velocity.add4(accel);
        }

        // Apply effectors (volumes and force fields)
        if (hasPerParticleEffectors)
        {
            static const unsigned int kTypeDirectionalA = 0x7C34BCCE;
            static const unsigned int kTypeDirectionalB = 0x237A04BB;
            static const unsigned int kTypeConstrict = 0x796690EA;
            static const unsigned int kTypeBounce = 0x3413B66D;

            float maxDamp = 0.0f;

            for (int pass = 0; pass < 2; ++pass)
            {
                const hkArray<EffectEffector*>* list = (pass == 0) ? &effectors : &emitterEffectors;
                for (int e = 0; e < list->getSize(); ++e)
                {
                    const EffectEffector* ef = (*list)[e];
                    if (!ef)
                        continue;

                    const float effX = effectOrigin(0) + ef->position(0);
                    const float effY = effectOrigin(1) + ef->position(1);
                    const float effZ = effectOrigin(2) + ef->position(2);

                    const float dx = p.position(0) - effX;
                    const float dy = p.position(1) - effY;
                    const float dz = p.position(2) - effZ;

                    const float ny = ef->axisY(1);
                    const float nx = ef->axisY(0);
                    const float nz = ef->axisY(2);

                    const float along = dx * nx + dy * ny + dz * nz;
                    const float rx = dx - nx * along;
                    const float ry = dy - ny * along;
                    const float rz = dz - nz * along;
                    const float radialSq = rx*rx + ry*ry + rz*rz;

                    float radius = ef->transformScale;
                    if (radius < 0.0f) radius = -radius;
                    if (radius > 0.0001f)
                    {
                        const float r2 = radius * radius;
                        if (radialSq > r2)
                            continue;
                    }

                    if (ef->typeId != kTypeBounce && ef->height > 0.0001f)
                    {
                        const float halfH = ef->height * 0.5f;
                        float a = along;
                        if (a < 0.0f) a = -a;
                        if (a > halfH)
                            continue;
                    }

                    const float forceAuth = EvaluateFloatSegments(ef->force, ef->forceSegments, ef->forceSegmentCount, timeFrames);
                    const float forceWorld = forceAuth * 0.01f;
                    if (forceWorld != 0.0f &&
                        (ef->typeId == kTypeDirectionalA ||
                         ef->typeId == kTypeDirectionalB ||
                         ef->typeId == kTypeConstrict))
                    {
                        hkVector4 dv;
                        dv.setMul4(forceWorld * deltaTime, ef->axisY);
                        p.velocity.add4(dv);
                    }

                    if (ef->typeId == kTypeConstrict && ef->constrict != 0.0f)
                    {
                        if (radialSq > 0.000001f)
                        {
                            float cx = rx;
                            float cy = ry;
                            float cz = rz;
                            ZNormalize3f(cx, cy, cz);
                            const float cWorld = ef->constrict * 0.01f;

                            hkVector4 dv;
                            dv.set(-cx * cWorld * deltaTime,
                                   -cy * cWorld * deltaTime,
                                   -cz * cWorld * deltaTime,
                                   0.0f);
                            p.velocity.add4(dv);
                        }
                    }

                    if (ef->typeId == kTypeBounce)
                    {
                        // Plane collision: keep particles on the positive side of the effector normal.
                        if (along < 0.0f)
                        {
                            const float penetration = -along;
                            p.position.set(p.position(0) + nx * penetration,
                                           p.position(1) + ny * penetration,
                                           p.position(2) + nz * penetration,
                                           0.0f);

                            const float vn = p.velocity(0) * nx + p.velocity(1) * ny + p.velocity(2) * nz;
                            if (vn < 0.0f)
                            {
                                float b = ef->bounce;
                                if (b < 0.0f) b = 0.0f;
                                if (b > 1.0f) b = 1.0f;

                                hkVector4 dv;
                                dv.setMul4(-(1.0f + b) * vn, ef->axisY);
                                p.velocity.add4(dv);
                            }
                        }
                    }

                    if (ef->damping > maxDamp)
                        maxDamp = ef->damping;
                }
            }

            if (maxDamp > 0.0f)
            {
                float damp = 1.0f - maxDamp * deltaTime;
                if (damp < 0.0f) damp = 0.0f;
                p.velocity.mul4(damp);
            }
        }
        else
        {
            // Apply external effectors (legacy directional approximation)
            if (externalAcceleration.lengthSquared3() > 0.000001f)
            {
                hkVector4 accel;
                accel.setMul4(deltaTime, externalAcceleration);
                p.velocity.add4(accel);
            }
            if (externalDamping > 0.0f)
            {
                // Simple exponential-ish damping approximation.
                float damp = 1.0f - externalDamping * deltaTime;
                if (damp < 0.0f) damp = 0.0f;
                p.velocity.mul4(damp);
            }
        }

        // Calculate lifetime ratio (0.0 = just spawned, 1.0 = about to die)
        float t = p.getLifetimeRatio();
        if (t > 1.0f) t = 1.0f;

        // Animate color (lerp from start to end based on lifetime)
        p.color = EffectColor::lerp(p.startColor, p.endColor, t);

        // Apply FadeInTime (frames -> seconds). Many effects rely on this to avoid popping.
        if (fadeInFrames > 0)
        {
            float finSec = (float)fadeInFrames / 30.0f;
            if (finSec > 0.000001f)
            {
                float f = p.lifetime / finSec;
                if (f < 0.0f) f = 0.0f;
                if (f > 1.0f) f = 1.0f;
                int a = (int)((float)p.color.a * f);
                if (a < 0) a = 0;
                if (a > 255) a = 255;
                p.color.a = (unsigned char)a;
            }
        }

        // Store color multiplier separately (do not clamp into 0..255 bytes).
        // The shipped game often uses multipliers > 1 for additive glow; clamping here
        // collapses many FX into dull "sheets". Renderers can apply this as float math.
        p.colorMultiplier = startColorMultiplier + (endColorMultiplier - startColorMultiplier) * t;

        // Animate size (lerp from start to end based on lifetime)
        float rawW = p.startWidth + (p.endWidth - p.startWidth) * t;
        float rawH = p.startHeight + (p.endHeight - p.startHeight) * t;

        // Sizes are authored in centimeters; convert to renderer units and apply WorldTransform scale.
        float uniformScale = transformScale;
        if (uniformScale < 0.0f) uniformScale = -uniformScale;
        p.width = rawW * 0.01f * uniformScale;
        p.height = rawH * 0.01f * uniformScale;

        // Animate texture frame (for flipbook animations)
        if (uvColumns * uvRows > 1 &&
            loopTextureAnim &&
            playbackTime > 0.0f &&
            animFrameMax > animFrameMin)
        {
            int totalFrames = animFrameMax - animFrameMin + 1;
            if (totalFrames < 1) totalFrames = 1;

            float loopLenSec = playbackTime / 30.0f; // playbackTime is in 30-FPS frames
            if (loopLenSec <= 0.000001f)
                loopLenSec = 1.0f / 30.0f;

            float framesPerSecond = (float)totalFrames / loopLenSec;
            int frameIndex = (int)floorf(p.animationTime * framesPerSecond);
            if (frameIndex < 0) frameIndex = 0;
            frameIndex = frameIndex % totalFrames;
            p.currentFrame = animFrameMin + frameIndex;
        }
    }

    removeDeadParticles();
}

void ParticleEmitter::removeDeadParticles()
{
    for (int i = particles.getSize() - 1; i >= 0; i--)
    {
        if (particles[i].lifetime >= particles[i].maxLifetime)
        {
            particles.removeAt(i);
        }
    }
}

// EffectLight implementation
EffectLight::EffectLight()
{
    position.setZero4();
    color = EffectColor(255, 255, 255, 255);
    colorScale = 1.0f;
    radius = 1.0f;
    innerRadius = 0.1f;
}

// EffectDefinition implementation
EffectDefinition::EffectDefinition()
{
    name[0] = '\0';
    duration = 0;
    looping = false;
    worldSpace = true;
    sortParticles = false;
}

EffectDefinition::~EffectDefinition()
{
    release();
}

void EffectDefinition::release()
{
    for (int i = 0; i < emitters.getSize(); i++)
    {
        delete emitters[i];
    }
    emitters.clear();
    
    for (int i = 0; i < lights.getSize(); i++)
    {
        delete lights[i];
    }
    lights.clear();

    for (int i = 0; i < effectors.getSize(); i++)
    {
        delete effectors[i];
    }
    effectors.clear();
}
