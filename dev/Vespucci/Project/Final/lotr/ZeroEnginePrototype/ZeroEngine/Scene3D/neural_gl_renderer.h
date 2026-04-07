// ============================================================================
// neural_gl_renderer.h — A Biologically Accurate Neural Network Visualizer
//                        Inside a Reverse-Engineered 2009 Game Engine
// ============================================================================
//
// WHY THE FUCK DOES A LORD OF THE RINGS GAME VIEWER HAVE A NEURAL
// NETWORK RENDERER?
//
// Because this project stopped being "just" a game reverse-engineering
// tool a long time ago. It became a platform. A framework. A place where
// I can build ANYTHING because the rendering infrastructure already
// exists — D3D9 for the game content, OpenGL 3.3 for experimental
// rendering, ImGui for UI, Havok for physics/animation, Win32 for
// threading. All the pieces are here. So when I got interested in
// computational neuroscience, I didn't go build a new project from
// scratch. I built it HERE. Inside the engine. Because the engine
// was already alive and hungry for new features.
//
// And I didn't build some bullshit abstract node-and-line diagram
// like every other neural network visualizer out there. I built a
// BIOLOGICALLY ACCURATE 3D neuron renderer. Here's what's scientifically
// real about this shit:
//
// ════════════════════════════════════════════════════════════════════
// NEUROSCIENCE ACCURACY — THIS ISN'T JUST PRETTY SPHERES
// ════════════════════════════════════════════════════════════════════
//
// 1. MEMBRANE POTENTIAL VISUALIZATION (voltage field)
//    Real neurons maintain a resting membrane potential of ~-70mV.
//    When stimulated, they depolarize toward ~+40mV (action potential).
//    Our voltage field [0,1] maps to this range. The color ramp:
//      Deep blue (0.0) → resting potential (-70mV, polarized)
//      Cyan (0.2)      → slight depolarization (-55mV)
//      Green (0.4)     → threshold region (-40mV, about to fire)
//      Yellow (0.5)    → firing threshold reached
//      Orange (0.7)    → peak depolarization (+20mV)
//      White (1.0)     → maximum firing (+40mV, full action potential)
//    This matches the standard neurophysiology voltage→color conventions
//    used in calcium imaging and voltage-sensitive dye studies.
//
// 2. MEMBRANE WOBBLE (procedural displacement)
//    Real neuron cell membranes are NOT rigid spheres. They're lipid
//    bilayers that constantly fluctuate due to thermal motion, osmotic
//    pressure changes, and cytoskeletal dynamics. Our vertex shader
//    displaces the soma surface using 4 octaves of sinusoidal noise:
//      disp = sin(seed + phase) * 0.50          ← bulk membrane motion
//           + sin(seed*2.7 + phase*1.6) * 0.30  ← medium-scale ripples
//           + sin(seed*5.1 + phase*2.3) * 0.15  ← fine surface detail
//           + sin(seed*9.8 + phase*3.1) * 0.05  ← micro-fluctuations
//    The amplitude INCREASES with voltage (wobbleAmp = 0.03 + voltage*0.08)
//    because real membranes physically deform during depolarization —
//    this is a documented phenomenon (Zhang et al., 2001; Bhatt et al., 2015).
//    When a neuron fires, its membrane swells ~1nm due to ion channel
//    conformational changes. We exaggerate this for visual clarity.
//
// 3. HEBBIAN LEARNING GLOW (hebbianGlow field)
//    "Neurons that fire together, wire together" — Donald Hebb, 1949.
//    Hebbian learning is the foundation of synaptic plasticity. When a
//    presynaptic neuron consistently drives a postsynaptic neuron to
//    fire, the synapse between them strengthens (Long-Term Potentiation,
//    LTP). Our hebbianGlow field represents this: neurons with high
//    Hebbian activity glow brighter, representing recently potentiated
//    pathways. The glow is additive in the fragment shader (glowAdd =
//    base * vGlow * 0.3) because LTP literally increases the density
//    of AMPA receptors on the postsynaptic membrane, making the cell
//    more responsive — which we represent as increased luminance.
//
// 4. SUBSURFACE SCATTERING / WRAP LIGHTING
//    Real neural tissue is translucent. Light passes THROUGH the cell
//    body. The cytoplasm scatters photons. This is why brain tissue
//    appears pinkish-white rather than opaque. Our wrap lighting
//    approximation:
//      NdotL = (dot(N,L) + wrap) / (1.0 + wrap), wrap = 0.35
//    ...simulates subsurface scattering by allowing light contribution
//    even on surfaces facing away from the light source. The scatter
//    color (orange→white) approximates the warm tones of real neural
//    tissue under transmitted illumination, matching what you see
//    in fluorescence microscopy preparations.
//
// 5. BIOLUMINESCENCE (externalLight field)
//    Some neurons express GFP (Green Fluorescent Protein) or GCaMP
//    (genetically encoded calcium indicators) in laboratory settings.
//    These literally make neurons GLOW when they're active. Our
//    externalLight field simulates this: it's the "calcium indicator"
//    channel. In a real lab, you'd see this through a fluorescence
//    microscope. Here, we add it directly to the fragment output.
//
// 6. METABOLIC FATIGUE (metabolicLoad field)
//    Real neurons consume ATP to maintain ion gradients (Na+/K+ pump).
//    Sustained high-frequency firing depletes local glucose and oxygen,
//    leading to metabolic fatigue. Neurons literally get TIRED. Our
//    metabolicLoad field [0,1] tracks this — high metabolic load
//    indicates a neuron that's been firing heavily and is approaching
//    energy depletion. This affects visual representation through the
//    flags system.
//
// 7. SYNAPTIC MORPHOLOGY (tapered tubes)
//    Real axons taper from the soma (cell body) toward the synaptic
//    terminal. They're not uniform cylinders. Our edge tubes taper
//    from source to destination: baseRadius * mix(1.0, 0.6, t).
//    The weight parameter controls thickness, corresponding to
//    myelination level — heavily myelinated axons are thicker and
//    conduct faster (saltatory conduction). Thick tube = strong,
//    fast connection. Thin tube = weak synapse.
//
// 8. FRESNEL RIM GLOW
//    The rim glow uses Schlick's Fresnel approximation:
//      fresnel = pow(1.0 - NdotV, 2.5)
//    This isn't just aesthetic — it simulates the optical properties
//    of cell membranes under oblique illumination. In real microscopy,
//    neurons viewed at glancing angles show enhanced edge brightness
//    due to the refractive index mismatch between the membrane
//    (n≈1.46) and the surrounding medium (n≈1.33 for water). The
//    intensity scales with voltage because depolarization changes the
//    local charge distribution, subtly altering the membrane's
//    dielectric properties.
//
// 9. PULSE PROPAGATION (pulse0Progress, pulse1Progress)
//    Action potentials travel along axons at 1-120 m/s depending on
//    myelination. Our pulseProgress fields [0,1] represent a traveling
//    wave of depolarization moving from source to destination along
//    each synapse. Two slots allow for multiple in-flight signals
//    (real axons can carry trains of action potentials with refractory
//    periods of ~1-2ms between spikes).
//
// 10. INHIBITORY vs EXCITATORY (flags bit0)
//     Real neural networks have two fundamental neuron types:
//     excitatory (glutamatergic, ~80% of cortical neurons) and
//     inhibitory (GABAergic, ~20%). Excitatory neurons depolarize
//     their targets. Inhibitory neurons hyperpolarize theirs. The
//     flags field tracks this because the rendering should distinguish
//     them — different color tints, different connection rendering.
//     This isn't decoration. It's the fundamental yin-yang of neural
//     computation: excitation and inhibition in balance. Disrupt that
//     balance and you get epilepsy (too much excitation) or coma
//     (too much inhibition).
//
// ════════════════════════════════════════════════════════════════════
// RENDERING PIPELINE (in imgui_glue_dll.cpp, the 20K-line monster)
// ════════════════════════════════════════════════════════════════════
//
// The D3D9 render-to-texture pipeline adds post-processing on top:
//
// Pass 1: GEOMETRY — Render neurons (instanced spheres) and synapses
//         (instanced tapered tubes) to a render target texture
// Pass 2: DEPTH — Linearize depth buffer to a separate texture for
//         screen-space effects
// Pass 3: SSAO — Screen-Space Ambient Occlusion using depth-based
//         hemisphere sampling (16 kernel samples). Darkens crevices
//         between densely packed neurons, adding depth perception.
//         Uses bilateral blur to preserve neuron edges.
// Pass 4: BLOOM BRIGHT-PASS — Extract pixels above luminance threshold.
//         Active neurons glow. Inactive ones don't.
// Pass 5: BLOOM BLUR — Separable Gaussian blur (horizontal + vertical)
//         on the bright-pass texture. Creates the diffuse glow halo
//         around firing neurons — like the optical bloom you see in
//         real fluorescence microscopy due to out-of-focus light.
// Pass 6: COMPOSITE — Combine scene + SSAO + bloom + exponential²
//         depth fog. The fog simulates the optical density of neural
//         tissue — you can't see deep into the brain because photons
//         scatter and absorb. Distant neurons fade into a warm fog.
//
// All of this runs at 60fps in a render-to-texture that gets displayed
// as an ImGui::Image() inside the docking UI. A full neuroscience
// visualization pipeline running inside a Lord of the Rings game viewer.
//
// WHY?
//
// Because the infrastructure was already here. Because I could. Because
// after 7 years of reverse-engineering death, I wanted to build
// something ALIVE. Neurons are alive. They fire. They learn. They
// grow. They die. They're the most complex computational system in
// the known universe, and rendering them with scientific accuracy
// inside a game engine felt like the most beautifully fucked thing
// I could possibly do with the tools I'd built.
//
// Pandemic built an engine to render orcs dying. I used it to render
// neurons living. Somewhere in that inversion is the whole point
// of this project.
//
// ============================================================================
#pragma once

#include <cstdint>
#include <vector>

// Per-neuron instance data uploaded to GPU each frame (48 bytes)
struct NeuralInstanceData {
    float posX, posY, posZ;   // world position (nn.y -> X, sin(phase)*R*4 -> Y, layer*Z_DIST -> Z)
    float radius;             // soma radius
    float voltage;            // normalized [0,1] for color mapping
    float phase;              // membrane wobble phase
    float hebbianGlow;        // [0,1]
    float externalLight;      // [0,1] bioluminescence
    float metabolicLoad;      // [0,1] fatigue
    uint32_t flags;           // bit0: isInhibitory, bit1: isPortal
    float pad[2];             // align to 48 bytes
};

// Per-edge instance data uploaded to GPU each frame (48 bytes)
struct EdgeInstanceData {
    float srcX, srcY, srcZ;   // source node world position
    float dstX, dstY, dstZ;   // dest node world position
    float activity;           // [0,1] signal activity
    float weight;             // [0.05, 1.0] fiber thickness
    uint32_t flags;           // bit0: isInhibitory, bit1: isGapJunction
    float pulse0Progress;     // [0,1] or -1 if no pulse
    float pulse1Progress;     // [0,1] or -1 if no pulse
    float pad;
};

// Snapshot of neural network state, copied from main thread each frame
struct NeuralGLSnapshot {
    std::vector<NeuralInstanceData> nodes;
    std::vector<EdgeInstanceData>   edges;

    // Camera
    float camAngleX, camAngleY, zoom;

    // Animation time
    float animTime;

    // Brain wave state
    float oscillationPhase;

    // Control flags
    bool active;        // true when neural view is open
    bool shouldClose;   // signal to shut down the render thread

    // Frame counter for change detection
    uint64_t frameNumber;
};

// === Public API (called from main thread in imgui_glue_dll.cpp) ===

// Launch the OpenGL render thread and open the GLFW window.
// Safe to call multiple times -- does nothing if already running.
void NeuralGL_Start();

// Signal the render thread to close and wait for it to exit.
// Safe to call multiple times -- does nothing if not running.
void NeuralGL_Stop();

// Copy current neural simulation state to the shared snapshot.
// Called once per frame from the main thread while the neural view is active.
void NeuralGL_UpdateSnapshot(
    const NeuralInstanceData* nodeData, int nodeCount,
    const EdgeInstanceData*   edgeData, int edgeCount,
    float camAngleX, float camAngleY, float zoom,
    float animTime, float oscillationPhase);

// Check if the render thread is currently running.
bool NeuralGL_IsRunning();

// Clean shutdown -- call from ImGuiGlue_Shutdown()
void NeuralGL_Shutdown();
