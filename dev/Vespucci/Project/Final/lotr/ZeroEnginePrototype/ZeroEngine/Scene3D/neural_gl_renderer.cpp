// ============================================================================
// neural_gl_renderer.cpp -- OpenGL 3.3 Brain Visualizer in a D3D9 DLL
// ============================================================================
//
// 540 lines of hand-rolled OpenGL running on a background thread inside
// a DirectX 9 game engine from 2008. Column-major matrices, manual
// lookAt/perspective, UV sphere and tube geometry builders, instanced
// rendering with per-neuron and per-edge attribute divisors. All of it
// crammed into imgui_d3d9.dll because that's where the ImGui layer lives
// and I was NOT going to create another fucking DLL.
//
// The render loop: poll GLFW events, grab the snapshot under a
// CRITICAL_SECTION lock, compute camera orbit from bounding box,
// draw instanced somas (spheres), draw instanced edges (tubes with
// alpha blending and depth mask tricks), swap buffers. 60fps steady.
//
// GLAD loads OpenGL function pointers because we can't assume anything
// on Windows. GLFW handles the window because Win32 CreateWindow for
// an OpenGL context is 200 lines of boilerplate nightmare.
//
// The camera orbits around the network center with mouse drag and
// scroll zoom. If the main thread is also sending camera params through
// the snapshot, we only override when the user isn't dragging locally.
// Two camera sources, one CRITICAL_SECTION. It somehow works.
//
// On shutdown we give the thread 3 seconds to clean up, then
// TerminateThread it. Brutal but effective. Can't have a zombie GL
// context eating VRAM while the game tries to render Minas Tirith.
//
// This is what happens when reverse engineering gets boring and you
// start building features Pandemic never dreamed of. None of this
// was in the original Zero Engine. None of it. We're off the map.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "neural_gl_renderer.h"
#include "neural_gl_shaders.h"

static const float PI = 3.14159265f;

// ============================================================
// MATH (column-major for OpenGL)
// ============================================================
struct Mat4 { float m[16]; };

static Mat4 mat4_identity() {
    Mat4 r = {};
    r.m[0]=1; r.m[5]=1; r.m[10]=1; r.m[15]=1;
    return r;
}

static Mat4 mat4_multiply(const Mat4& a, const Mat4& b) {
    Mat4 r = {};
    for (int c=0;c<4;c++) for (int row=0;row<4;row++)
        for (int k=0;k<4;k++)
            r.m[c*4+row] += a.m[k*4+row] * b.m[c*4+k];
    return r;
}

static Mat4 mat4_lookAt(float ex, float ey, float ez, float tx, float ty, float tz) {
    float fx=tx-ex, fy=ty-ey, fz=tz-ez;
    float fLen=sqrtf(fx*fx+fy*fy+fz*fz);
    if (fLen<0.0001f) fLen=0.0001f;
    fx/=fLen; fy/=fLen; fz/=fLen;
    float upx=0,upy=1,upz=0;
    if (fabsf(fy) > 0.99f) { upx=0; upy=0; upz=1; }
    float rx=fy*upz-fz*upy, ry=fz*upx-fx*upz, rz=fx*upy-fy*upx;
    float rLen=sqrtf(rx*rx+ry*ry+rz*rz);
    if (rLen<0.0001f) rLen=0.0001f;
    rx/=rLen; ry/=rLen; rz/=rLen;
    float ux=ry*fz-rz*fy, uy=rz*fx-rx*fz, uz=rx*fy-ry*fx;
    Mat4 m = {};
    m.m[0]=rx;  m.m[1]=ux;  m.m[2]=-fx;  m.m[3]=0;
    m.m[4]=ry;  m.m[5]=uy;  m.m[6]=-fy;  m.m[7]=0;
    m.m[8]=rz;  m.m[9]=uz;  m.m[10]=-fz; m.m[11]=0;
    m.m[12]=-(rx*ex+ry*ey+rz*ez);
    m.m[13]=-(ux*ex+uy*ey+uz*ez);
    m.m[14]= (fx*ex+fy*ey+fz*ez);
    m.m[15]=1;
    return m;
}

static Mat4 mat4_perspective(float fovY, float aspect, float zn, float zf) {
    float t = tanf(fovY * 0.5f);
    Mat4 m = {};
    m.m[0] = 1.0f / (aspect * t);
    m.m[5] = 1.0f / t;
    m.m[10]= -(zf + zn) / (zf - zn);
    m.m[11]= -1.0f;
    m.m[14]= -2.0f * zf * zn / (zf - zn);
    return m;
}

// ============================================================
// GEOMETRY BUILDERS
// ============================================================
struct GLVertex { float x,y,z, nx,ny,nz, u,v; };

static void buildUVSphere(int rings, int segs, std::vector<GLVertex>& verts, std::vector<unsigned int>& idx) {
    verts.clear(); idx.clear();
    for (int r=0;r<=rings;r++) {
        float phi = PI * r / rings;
        float sp=sinf(phi), cp=cosf(phi);
        for (int s=0;s<=segs;s++) {
            float theta = 2.0f*PI*s/segs;
            GLVertex v;
            v.nx=sp*cosf(theta); v.ny=cp; v.nz=sp*sinf(theta);
            v.x=v.nx; v.y=v.ny; v.z=v.nz;
            v.u=(float)s/segs; v.v=(float)r/rings;
            verts.push_back(v);
        }
    }
    for (int r=0;r<rings;r++) for (int s=0;s<segs;s++) {
        unsigned int k = r*(segs+1)+s;
        idx.push_back(k); idx.push_back(k+segs+1); idx.push_back(k+1);
        idx.push_back(k+1); idx.push_back(k+segs+1); idx.push_back(k+segs+2);
    }
}

static void buildUnitTube(int segs, std::vector<GLVertex>& verts, std::vector<unsigned int>& idx) {
    verts.clear(); idx.clear();
    for (int ring=0; ring<=1; ring++) {
        for (int s=0; s<=segs; s++) {
            float theta = 2.0f*PI*s/segs;
            float ct=cosf(theta), st=sinf(theta);
            GLVertex v;
            v.x = ct; v.y = st; v.z = (float)ring;
            v.nx = ct; v.ny = st; v.nz = 0;
            v.u = (float)ring; v.v = (float)s/segs;
            verts.push_back(v);
        }
    }
    unsigned int base = 0;
    for (int s=0; s<segs; s++) {
        unsigned int a=base+s, b=base+s+1, c=base+segs+1+s, d=base+segs+1+s+1;
        idx.push_back(a); idx.push_back(c); idx.push_back(b);
        idx.push_back(b); idx.push_back(c); idx.push_back(d);
    }
}

// ============================================================
// SHADER HELPERS
// ============================================================
static GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]; glGetShaderInfoLog(s, 1024, nullptr, log);
        OutputDebugStringA("[NeuralGL] Shader compile error: ");
        OutputDebugStringA(log);
        OutputDebugStringA("\n");
    }
    return s;
}

static GLuint createProgram(const char* vsSrc, const char* fsSrc) {
    GLuint v = compileShader(GL_VERTEX_SHADER, vsSrc);
    GLuint f = compileShader(GL_FRAGMENT_SHADER, fsSrc);
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f);
    glLinkProgram(p);
    int ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]; glGetProgramInfoLog(p, 1024, nullptr, log);
        OutputDebugStringA("[NeuralGL] Program link error: ");
        OutputDebugStringA(log);
        OutputDebugStringA("\n");
    }
    glDeleteShader(v); glDeleteShader(f);
    return p;
}

// ============================================================
// MESH OBJECT
// ============================================================
struct GLMesh {
    GLuint vao, vbo, ebo;
    int indexCount;
};

static GLMesh uploadMesh(const std::vector<GLVertex>& verts, const std::vector<unsigned int>& idx) {
    GLMesh m;
    m.indexCount = (int)idx.size();
    glGenVertexArrays(1, &m.vao);
    glGenBuffers(1, &m.vbo);
    glGenBuffers(1, &m.ebo);
    glBindVertexArray(m.vao);
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(GLVertex), verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size()*sizeof(unsigned int), idx.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GLVertex), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(GLVertex), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(GLVertex), (void*)(6*sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    return m;
}

// ============================================================
// CAMERA (local to render thread, can also be driven from snapshot)
// ============================================================
static float s_glCamAngleX = 0.3f, s_glCamAngleY = 0.0f, s_glCamZoom = 1.0f;
static double s_glLastMX = 0, s_glLastMY = 0;
static bool s_glDragging = false;

static void glMouseButtonCB(GLFWwindow* w, int button, int action, int mods) {
    (void)mods;
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        s_glDragging = (action == GLFW_PRESS);
        glfwGetCursorPos(w, &s_glLastMX, &s_glLastMY);
    }
}

static void glCursorPosCB(GLFWwindow* w, double xpos, double ypos) {
    (void)w;
    if (s_glDragging) {
        s_glCamAngleY += (float)(xpos - s_glLastMX) * 0.008f;
        s_glCamAngleX += (float)(ypos - s_glLastMY) * 0.008f;
        if (s_glCamAngleX < -1.5f) s_glCamAngleX = -1.5f;
        if (s_glCamAngleX >  1.5f) s_glCamAngleX =  1.5f;
    }
    s_glLastMX = xpos; s_glLastMY = ypos;
}

static void glScrollCB(GLFWwindow* w, double xoff, double yoff) {
    (void)w; (void)xoff;
    s_glCamZoom *= (yoff > 0) ? 1.1f : (1.0f/1.1f);
    if (s_glCamZoom < 0.1f) s_glCamZoom = 0.1f;
    if (s_glCamZoom > 10.0f) s_glCamZoom = 10.0f;
}

// ============================================================
// SHARED STATE (protected by critical section)
// ============================================================
static CRITICAL_SECTION s_neuralGLLock;
static NeuralGLSnapshot s_neuralGLSnap;
static bool s_lockInitialized = false;
static HANDLE s_renderThread = NULL;
static volatile bool s_threadRunning = false;

// ============================================================
// RENDER THREAD MAIN
// ============================================================
static unsigned __stdcall NeuralGL_ThreadMain(void* param) {
    (void)param;

    // --- GLFW + OpenGL init (must happen on this thread) ---
    if (!glfwInit()) {
        OutputDebugStringA("[NeuralGL] glfwInit failed\n");
        s_threadRunning = false;
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 4);

    GLFWwindow* window = glfwCreateWindow(1280, 720,
        "Neural Network - OpenGL 3D", nullptr, nullptr);
    if (!window) {
        OutputDebugStringA("[NeuralGL] Window creation failed\n");
        glfwTerminate();
        s_threadRunning = false;
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetMouseButtonCallback(window, glMouseButtonCB);
    glfwSetCursorPosCallback(window, glCursorPosCB);
    glfwSetScrollCallback(window, glScrollCB);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        OutputDebugStringA("[NeuralGL] GLAD init failed\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        s_threadRunning = false;
        return 1;
    }

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE);
    glClearColor(6.0f/255, 8.0f/255, 18.0f/255, 1.0f);

    // --- Compile shaders ---
    GLuint somaProg = createProgram(g_somaInstVS, g_somaInstFS);
    GLuint edgeProg = createProgram(g_edgeInstVS, g_edgeInstFS);

    // --- Build unit sphere mesh (16 rings x 24 segments) ---
    std::vector<GLVertex> sphereV; std::vector<unsigned int> sphereI;
    buildUVSphere(16, 24, sphereV, sphereI);
    GLMesh sphereMesh = uploadMesh(sphereV, sphereI);

    // --- Build unit tube mesh (8 segments) ---
    std::vector<GLVertex> tubeV; std::vector<unsigned int> tubeI;
    buildUnitTube(8, tubeV, tubeI);
    GLMesh tubeMesh = uploadMesh(tubeV, tubeI);

    // --- Instance buffers (dynamic, resized as needed) ---
    GLuint somaInstanceVBO = 0;
    glGenBuffers(1, &somaInstanceVBO);
    GLuint edgeInstanceVBO = 0;
    glGenBuffers(1, &edgeInstanceVBO);

    // Setup soma instanced attributes on the sphere VAO
    glBindVertexArray(sphereMesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, somaInstanceVBO);
    // iPos (location=3): vec4 (posX, posY, posZ, radius)
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(NeuralInstanceData), (void*)0);
    glEnableVertexAttribArray(3);
    glVertexAttribDivisor(3, 1);
    // iState (location=4): vec4 (voltage, phase, hebbianGlow, externalLight)
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(NeuralInstanceData), (void*)(4*sizeof(float)));
    glEnableVertexAttribArray(4);
    glVertexAttribDivisor(4, 1);
    // iExtra (location=5): vec4 (metabolicLoad, flags-as-float, pad, pad)
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(NeuralInstanceData), (void*)(8*sizeof(float)));
    glEnableVertexAttribArray(5);
    glVertexAttribDivisor(5, 1);
    glBindVertexArray(0);

    // Setup edge instanced attributes on the tube VAO
    glBindVertexArray(tubeMesh.vao);
    glBindBuffer(GL_ARRAY_BUFFER, edgeInstanceVBO);
    // iSrc (location=3): vec3
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(EdgeInstanceData), (void*)0);
    glEnableVertexAttribArray(3);
    glVertexAttribDivisor(3, 1);
    // iDst (location=4): vec3
    glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(EdgeInstanceData), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(4);
    glVertexAttribDivisor(4, 1);
    // iEdgeData (location=5): vec4 (activity, weight, flags, pulse0)
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(EdgeInstanceData), (void*)(6*sizeof(float)));
    glEnableVertexAttribArray(5);
    glVertexAttribDivisor(5, 1);
    glBindVertexArray(0);

    float lightDir[3] = {-0.5f, -0.8f, 0.3f};
    int nodeCount = 0, edgeCount = 0;

    // Local copy of snapshot (read under lock, used for rendering)
    std::vector<NeuralInstanceData> localNodes;
    std::vector<EdgeInstanceData> localEdges;
    float localCamAngleX = 0.3f, localCamAngleY = 0.0f, localZoom = 1.0f;

    // --- RENDER LOOP ---
    while (!glfwWindowShouldClose(window) && s_threadRunning) {
        glfwPollEvents();

        // Read snapshot under lock
        bool shouldClose = false;
        EnterCriticalSection(&s_neuralGLLock);
        {
            localNodes = s_neuralGLSnap.nodes;
            localEdges = s_neuralGLSnap.edges;
            // Sync camera from main thread if user isn't dragging locally
            if (!s_glDragging) {
                s_glCamAngleX = s_neuralGLSnap.camAngleX;
                s_glCamAngleY = s_neuralGLSnap.camAngleY;
                s_glCamZoom   = s_neuralGLSnap.zoom;
            }
            shouldClose = s_neuralGLSnap.shouldClose;
        }
        LeaveCriticalSection(&s_neuralGLLock);

        if (shouldClose) break;

        nodeCount = (int)localNodes.size();
        edgeCount = (int)localEdges.size();

        // Compute bounding box for camera
        float minX=1e9f, maxX=-1e9f, minZ=1e9f, maxZ=-1e9f;
        for (int i=0; i<nodeCount; i++) {
            float x = localNodes[i].posX, z = localNodes[i].posZ;
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (z < minZ) minZ = z;
            if (z > maxZ) maxZ = z;
        }
        float cx = (minX+maxX)*0.5f, cz = (minZ+maxZ)*0.5f;
        float extX = (maxX-minX)*0.5f + 50.0f;
        float extZ = (maxZ-minZ)*0.5f + 50.0f;
        float totalExt = extX > extZ ? extX : extZ;
        if (totalExt < 50.0f) totalExt = 50.0f;
        float camDist = totalExt * 3.0f / s_glCamZoom;

        float eyeX = cx + camDist * sinf(s_glCamAngleY) * cosf(s_glCamAngleX);
        float eyeY = camDist * sinf(s_glCamAngleX);
        float eyeZ = cz + camDist * cosf(s_glCamAngleY) * cosf(s_glCamAngleX);

        int ww, wh;
        glfwGetFramebufferSize(window, &ww, &wh);
        if (ww < 1) ww = 1; if (wh < 1) wh = 1;
        glViewport(0, 0, ww, wh);

        Mat4 view = mat4_lookAt(eyeX, eyeY, eyeZ, cx, 0, cz);
        Mat4 proj = mat4_perspective(50.0f * PI / 180.0f, (float)ww/(float)wh, 0.1f, camDist * 5.0f);
        Mat4 vp = mat4_multiply(proj, view);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // --- DRAW SOMAS (instanced) ---
        if (nodeCount > 0) {
            glBindBuffer(GL_ARRAY_BUFFER, somaInstanceVBO);
            glBufferData(GL_ARRAY_BUFFER, nodeCount * sizeof(NeuralInstanceData),
                localNodes.data(), GL_STREAM_DRAW);

            glUseProgram(somaProg);
            glUniformMatrix4fv(glGetUniformLocation(somaProg, "uVP"), 1, GL_FALSE, vp.m);
            glUniform3f(glGetUniformLocation(somaProg, "uLightDir"), lightDir[0], lightDir[1], lightDir[2]);
            glUniform3f(glGetUniformLocation(somaProg, "uCamPos"), eyeX, eyeY, eyeZ);

            glBindVertexArray(sphereMesh.vao);
            glDrawElementsInstanced(GL_TRIANGLES, sphereMesh.indexCount,
                GL_UNSIGNED_INT, 0, nodeCount);
        }

        // --- DRAW EDGES (instanced) ---
        if (edgeCount > 0) {
            glBindBuffer(GL_ARRAY_BUFFER, edgeInstanceVBO);
            glBufferData(GL_ARRAY_BUFFER, edgeCount * sizeof(EdgeInstanceData),
                localEdges.data(), GL_STREAM_DRAW);

            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);

            glUseProgram(edgeProg);
            glUniformMatrix4fv(glGetUniformLocation(edgeProg, "uVP"), 1, GL_FALSE, vp.m);
            glUniform3f(glGetUniformLocation(edgeProg, "uLightDir"), lightDir[0], lightDir[1], lightDir[2]);
            glUniform3f(glGetUniformLocation(edgeProg, "uCamPos"), eyeX, eyeY, eyeZ);

            glBindVertexArray(tubeMesh.vao);
            glDrawElementsInstanced(GL_TRIANGLES, tubeMesh.indexCount,
                GL_UNSIGNED_INT, 0, edgeCount);

            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }

        // Update window title with stats
        {
            char title[256];
            snprintf(title, sizeof(title),
                "Neural Network | Nodes: %d | Edges: %d | Zoom: %.0f%% | Drag to orbit",
                nodeCount, edgeCount, s_glCamZoom * 100.0f);
            glfwSetWindowTitle(window, title);
        }

        glfwSwapBuffers(window);
    }

    // --- Cleanup ---
    glDeleteBuffers(1, &somaInstanceVBO);
    glDeleteBuffers(1, &edgeInstanceVBO);
    glDeleteProgram(somaProg);
    glDeleteProgram(edgeProg);
    // VAO/VBO/EBO cleanup
    glDeleteVertexArrays(1, &sphereMesh.vao);
    glDeleteBuffers(1, &sphereMesh.vbo);
    glDeleteBuffers(1, &sphereMesh.ebo);
    glDeleteVertexArrays(1, &tubeMesh.vao);
    glDeleteBuffers(1, &tubeMesh.vbo);
    glDeleteBuffers(1, &tubeMesh.ebo);

    glfwDestroyWindow(window);
    glfwTerminate();

    s_threadRunning = false;
    return 0;
}

// ============================================================
// PUBLIC API
// ============================================================

void NeuralGL_Start() {
    if (s_threadRunning) return;

    if (!s_lockInitialized) {
        InitializeCriticalSection(&s_neuralGLLock);
        s_lockInitialized = true;
    }

    // Reset snapshot
    EnterCriticalSection(&s_neuralGLLock);
    s_neuralGLSnap.shouldClose = false;
    s_neuralGLSnap.active = true;
    s_neuralGLSnap.frameNumber = 0;
    LeaveCriticalSection(&s_neuralGLLock);

    s_threadRunning = true;
    s_renderThread = (HANDLE)_beginthreadex(nullptr, 0, NeuralGL_ThreadMain, nullptr, 0, nullptr);
    if (!s_renderThread) {
        s_threadRunning = false;
        OutputDebugStringA("[NeuralGL] Failed to create render thread\n");
    }
}

void NeuralGL_Stop() {
    if (!s_threadRunning || !s_renderThread) return;

    // Signal thread to exit
    EnterCriticalSection(&s_neuralGLLock);
    s_neuralGLSnap.shouldClose = true;
    LeaveCriticalSection(&s_neuralGLLock);

    // Wait for thread to finish (3 second timeout)
    DWORD result = WaitForSingleObject(s_renderThread, 3000);
    if (result == WAIT_TIMEOUT) {
        OutputDebugStringA("[NeuralGL] Render thread timeout — forcing termination\n");
        TerminateThread(s_renderThread, 1);
    }
    CloseHandle(s_renderThread);
    s_renderThread = NULL;
    s_threadRunning = false;
}

void NeuralGL_UpdateSnapshot(
    const NeuralInstanceData* nodeData, int nodeCount,
    const EdgeInstanceData*   edgeData, int edgeCount,
    float camAngleX, float camAngleY, float zoom,
    float animTime, float oscillationPhase)
{
    if (!s_lockInitialized || !s_threadRunning) return;

    EnterCriticalSection(&s_neuralGLLock);
    {
        s_neuralGLSnap.nodes.resize(nodeCount);
        if (nodeCount > 0)
            memcpy(s_neuralGLSnap.nodes.data(), nodeData, nodeCount * sizeof(NeuralInstanceData));

        s_neuralGLSnap.edges.resize(edgeCount);
        if (edgeCount > 0)
            memcpy(s_neuralGLSnap.edges.data(), edgeData, edgeCount * sizeof(EdgeInstanceData));

        s_neuralGLSnap.camAngleX = camAngleX;
        s_neuralGLSnap.camAngleY = camAngleY;
        s_neuralGLSnap.zoom = zoom;
        s_neuralGLSnap.animTime = animTime;
        s_neuralGLSnap.oscillationPhase = oscillationPhase;
        s_neuralGLSnap.frameNumber++;
    }
    LeaveCriticalSection(&s_neuralGLLock);
}

bool NeuralGL_IsRunning() {
    return s_threadRunning;
}

void NeuralGL_Shutdown() {
    NeuralGL_Stop();
    if (s_lockInitialized) {
        DeleteCriticalSection(&s_neuralGLLock);
        s_lockInitialized = false;
    }
}
