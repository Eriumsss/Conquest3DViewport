// Neural Network 3D Test — Standalone GLFW + OpenGL 3.3 Core
// 2 neurons communicating via a synapse, orbital camera, voltage-to-color shading.
// No ImGui, no D3D9, no shared state. Pure OpenGL rendering.

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

// ============================================================
// MATH — column-major 4x4 matrices for OpenGL
// ============================================================
static const float PI = 3.14159265f;

struct Vec3 { float x, y, z; };
struct Mat4 { float m[16]; }; // column-major: m[col*4+row]

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

static Mat4 mat4_translate(float x, float y, float z) {
    Mat4 r = mat4_identity();
    r.m[12]=x; r.m[13]=y; r.m[14]=z;
    return r;
}

static Mat4 mat4_scale(float s) {
    Mat4 r = {};
    r.m[0]=s; r.m[5]=s; r.m[10]=s; r.m[15]=1;
    return r;
}

static Mat4 mat4_lookAt(Vec3 eye, Vec3 target, Vec3 up) {
    float fx=target.x-eye.x, fy=target.y-eye.y, fz=target.z-eye.z;
    float fLen=sqrtf(fx*fx+fy*fy+fz*fz);
    if (fLen<0.0001f) fLen=0.0001f;
    fx/=fLen; fy/=fLen; fz/=fLen;
    // right = cross(fwd, up)
    float rx=fy*up.z-fz*up.y, ry=fz*up.x-fx*up.z, rz=fx*up.y-fy*up.x;
    float rLen=sqrtf(rx*rx+ry*ry+rz*rz);
    if (rLen<0.0001f) rLen=0.0001f;
    rx/=rLen; ry/=rLen; rz/=rLen;
    // up = cross(right, fwd)
    float ux=ry*fz-rz*fy, uy=rz*fx-rx*fz, uz=rx*fy-ry*fx;
    Mat4 m = {};
    m.m[0]=rx;  m.m[1]=ux;  m.m[2]=-fx;  m.m[3]=0;
    m.m[4]=ry;  m.m[5]=uy;  m.m[6]=-fy;  m.m[7]=0;
    m.m[8]=rz;  m.m[9]=uz;  m.m[10]=-fz; m.m[11]=0;
    m.m[12]=-(rx*eye.x+ry*eye.y+rz*eye.z);
    m.m[13]=-(ux*eye.x+uy*eye.y+uz*eye.z);
    m.m[14]= (fx*eye.x+fy*eye.y+fz*eye.z);
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
struct Vertex { float x,y,z, nx,ny,nz, u,v; };

static void buildUVSphere(int rings, int segs, std::vector<Vertex>& verts, std::vector<unsigned int>& idx) {
    verts.clear(); idx.clear();
    for (int r=0;r<=rings;r++) {
        float phi = PI * r / rings;
        float sp=sinf(phi), cp=cosf(phi);
        for (int s=0;s<=segs;s++) {
            float theta = 2.0f*PI*s/segs;
            float st=sinf(theta), ct=cosf(theta);
            Vertex v;
            v.nx=sp*ct; v.ny=cp; v.nz=sp*st;
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

static void buildTube(float* p1, float r1, float* p2, float r2, int segs,
    std::vector<Vertex>& verts, std::vector<unsigned int>& idx)
{
    float dx=p2[0]-p1[0], dy=p2[1]-p1[1], dz=p2[2]-p1[2];
    float len=sqrtf(dx*dx+dy*dy+dz*dz);
    if (len<0.001f) return;
    float ax=dx/len, ay=dy/len, az=dz/len;
    float rx,ry,rz;
    if (fabsf(ay)<0.99f) { rx=az; ry=0; rz=-ax; }
    else { rx=0; ry=-az; rz=ay; }
    float rl=sqrtf(rx*rx+ry*ry+rz*rz);
    if (rl<0.001f) return;
    rx/=rl; ry/=rl; rz/=rl;
    float ux=ay*rz-az*ry, uy=az*rx-ax*rz, uz=ax*ry-ay*rx;
    unsigned int base=(unsigned int)verts.size();
    for (int ring=0;ring<=1;ring++) {
        float cx=(ring==0)?p1[0]:p2[0], cy=(ring==0)?p1[1]:p2[1], cz=(ring==0)?p1[2]:p2[2];
        float cr=(ring==0)?r1:r2;
        for (int s=0;s<=segs;s++) {
            float theta=2.0f*PI*s/segs;
            float ct=cosf(theta), st=sinf(theta);
            float nx2=ct*rx+st*ux, ny2=ct*ry+st*uy, nz2=ct*rz+st*uz;
            Vertex v;
            v.x=cx+nx2*cr; v.y=cy+ny2*cr; v.z=cz+nz2*cr;
            v.nx=nx2; v.ny=ny2; v.nz=nz2;
            v.u=(float)ring; v.v=(float)s/segs;
            verts.push_back(v);
        }
    }
    for (int s=0;s<segs;s++) {
        unsigned int a=base+s, b=base+s+1, c=base+segs+1+s, d=base+segs+1+s+1;
        idx.push_back(a); idx.push_back(c); idx.push_back(b);
        idx.push_back(b); idx.push_back(c); idx.push_back(d);
    }
}

// ============================================================
// SHADERS — GLSL 330
// ============================================================
static const char* somaVS = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNorm;
layout(location=2) in vec2 aUV;
uniform mat4 uMVP;
uniform mat4 uModel;
uniform float uMemPhase;
uniform float uWobbleAmp;
out vec3 vNorm;
out vec3 vWorldPos;
out float vVoltage;
void main() {
    float seed = aPos.x*3.7 + aPos.y*5.3 + aPos.z*2.1;
    float disp = sin(seed + uMemPhase)*0.50
               + sin(seed*2.7 + uMemPhase*1.6)*0.30
               + sin(seed*5.1 + uMemPhase*2.3)*0.15
               + sin(seed*9.8 + uMemPhase*3.1)*0.05;
    vec3 displaced = aPos + aNorm * disp * uWobbleAmp;
    gl_Position = uMVP * vec4(displaced, 1.0);
    vNorm = mat3(uModel) * aNorm;
    vWorldPos = (uModel * vec4(displaced, 1.0)).xyz;
    vVoltage = aUV.x;
}
)";

static const char* somaFS = R"(
#version 330 core
uniform vec3 uLightDir;
uniform vec3 uCamPos;
in vec3 vNorm;
in vec3 vWorldPos;
in float vVoltage;
out vec4 FragColor;

vec3 VoltageToColor(float v) {
    vec3 c0=vec3(0.04,0.04,0.45), c1=vec3(0.00,0.55,0.75);
    vec3 c2=vec3(0.10,0.75,0.20), c3=vec3(1.00,0.90,0.05);
    vec3 c4=vec3(1.00,0.45,0.00), c5=vec3(1.00,1.00,1.00);
    if (v<0.20) return mix(c0,c1,v/0.20);
    if (v<0.40) return mix(c1,c2,(v-0.20)/0.20);
    if (v<0.50) return mix(c2,c3,(v-0.40)/0.10);
    if (v<0.70) return mix(c3,c4,(v-0.50)/0.20);
    return mix(c4,c5,(v-0.70)/0.30);
}

void main() {
    vec3 N=normalize(vNorm), L=normalize(-uLightDir), V=normalize(uCamPos-vWorldPos);
    vec3 base=VoltageToColor(clamp(vVoltage,0.0,1.0));
    float wrap=0.35;
    float NdotL=clamp((dot(N,L)+wrap)/(1.0+wrap),0.0,1.0);
    vec3 scatter=mix(vec3(0.75,0.25,0.15),vec3(1),clamp(dot(N,L)+0.3,0.0,1.0));
    float NdotV=clamp(dot(N,V),0.0,1.0);
    float fresnel=pow(1.0-NdotV,2.5);
    float rimInt=mix(0.25,2.0,clamp(vVoltage,0.0,1.0));
    vec3 rimCol=mix(base,vec3(1),vVoltage*0.6);
    vec3 rim=rimCol*fresnel*rimInt;
    vec3 ambient=base*vec3(0.12,0.10,0.18);
    vec3 result=ambient+base*NdotL*scatter+rim;
    FragColor=vec4(clamp(result,0.0,1.0),1.0);
}
)";

static const char* tubeVS = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNorm;
layout(location=2) in vec2 aUV;
uniform mat4 uMVP;
out vec3 vNorm;
out vec3 vWorldPos;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vNorm = aNorm;
    vWorldPos = aPos;
}
)";

static const char* tubeFS = R"(
#version 330 core
uniform vec3 uLightDir;
uniform vec3 uCamPos;
uniform float uActivity;
in vec3 vNorm;
in vec3 vWorldPos;
out vec4 FragColor;
void main() {
    vec3 N=normalize(vNorm), L=normalize(-uLightDir), V=normalize(uCamPos-vWorldPos);
    vec3 base=mix(vec3(0.28,0.08,0.50), vec3(0.80,0.70,1.00), clamp(uActivity,0.0,1.0));
    float wrap=0.25;
    float NdotL=clamp((dot(N,L)+wrap)/(1.0+wrap),0.0,1.0);
    float NdotV=clamp(dot(N,V),0.0,1.0);
    float fresnel=pow(1.0-NdotV,2.0);
    vec3 rim=base*fresnel*(0.4+uActivity*1.2);
    vec3 result=base*0.07+base*NdotL*0.85+rim;
    float alpha=0.4+uActivity*0.5;
    FragColor=vec4(clamp(result,0.0,1.0),alpha);
}
)";

// ============================================================
// SHADER HELPERS
// ============================================================
static GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    int ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetShaderInfoLog(s, 512, nullptr, log);
        printf("Shader compile error: %s\n", log);
    }
    return s;
}

static GLuint createProgram(const char* vs, const char* fs) {
    GLuint v=compileShader(GL_VERTEX_SHADER, vs);
    GLuint f=compileShader(GL_FRAGMENT_SHADER, fs);
    GLuint p=glCreateProgram();
    glAttachShader(p,v); glAttachShader(p,f);
    glLinkProgram(p);
    int ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetProgramInfoLog(p, 512, nullptr, log);
        printf("Program link error: %s\n", log);
    }
    glDeleteShader(v); glDeleteShader(f);
    return p;
}

// ============================================================
// NEURON SIMULATION
// ============================================================
struct Neuron {
    float voltage;       // normalized [0,1] — 0=resting, 1=peak
    float phase;         // membrane wobble phase
    float fireTimer;     // seconds until next autonomous fire
    float firePeriod;    // seconds between fires
    Vec3 pos;            // world position
    float radius;        // soma radius
};

struct Synapse {
    int src, dst;
    float activity;      // [0,1] visual activity on the tube
    float signalTime;    // time when signal was sent (-1 = none)
    float delay;         // propagation delay in seconds
    float weight;
};

static void updateSimulation(Neuron* neurons, int nCount, Synapse* synapses, int sCount, float dt, float t) {
    for (int i=0;i<nCount;i++) {
        Neuron& n = neurons[i];
        // Decay voltage toward resting
        n.voltage *= expf(-dt * 3.0f);
        if (n.voltage < 0.001f) n.voltage = 0;
        // Advance wobble phase
        n.phase += dt * (1.5f + n.voltage * 3.0f);
        // Autonomous firing
        n.fireTimer -= dt;
        if (n.fireTimer <= 0) {
            n.voltage = 1.0f; // fire!
            n.fireTimer = n.firePeriod;
            // Send signal on all outgoing synapses
            for (int s=0;s<sCount;s++)
                if (synapses[s].src == i && synapses[s].signalTime < 0)
                    synapses[s].signalTime = t;
        }
    }
    for (int s=0;s<sCount;s++) {
        Synapse& syn = synapses[s];
        // Propagate signal
        if (syn.signalTime >= 0) {
            float elapsed = t - syn.signalTime;
            syn.activity = elapsed / syn.delay;
            if (elapsed >= syn.delay) {
                // Signal arrived at destination
                neurons[syn.dst].voltage = 1.0f;
                syn.signalTime = -1;
                syn.activity = 1.0f;
            }
        } else {
            syn.activity *= expf(-dt * 2.0f);
        }
    }
}

// ============================================================
// CAMERA
// ============================================================
static float camAngleX = 0.3f, camAngleY = 0.0f, camZoom = 1.0f;
static double lastMouseX = 0, lastMouseY = 0;
static bool dragging = false;

static void mouseButtonCB(GLFWwindow* w, int button, int action, int mods) {
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        dragging = (action == GLFW_PRESS);
        glfwGetCursorPos(w, &lastMouseX, &lastMouseY);
    }
}

static void cursorPosCB(GLFWwindow* w, double xpos, double ypos) {
    if (dragging) {
        camAngleY += (float)(xpos - lastMouseX) * 0.008f;
        camAngleX += (float)(ypos - lastMouseY) * 0.008f;
        if (camAngleX < -1.5f) camAngleX = -1.5f;
        if (camAngleX >  1.5f) camAngleX =  1.5f;
    }
    lastMouseX = xpos; lastMouseY = ypos;
}

static void scrollCB(GLFWwindow* w, double xoff, double yoff) {
    camZoom *= (yoff > 0) ? 1.1f : (1.0f/1.1f);
    if (camZoom < 0.2f) camZoom = 0.2f;
    if (camZoom > 5.0f) camZoom = 5.0f;
}

// ============================================================
// MESH — VAO/VBO/EBO
// ============================================================
struct Mesh {
    GLuint vao, vbo, ebo;
    int indexCount;
};

static Mesh uploadMesh(const std::vector<Vertex>& verts, const std::vector<unsigned int>& idx) {
    Mesh m;
    m.indexCount = (int)idx.size();
    glGenVertexArrays(1, &m.vao);
    glGenBuffers(1, &m.vbo);
    glGenBuffers(1, &m.ebo);
    glBindVertexArray(m.vao);
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(Vertex), verts.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size()*sizeof(unsigned int), idx.data(), GL_STATIC_DRAW);
    // pos
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    glEnableVertexAttribArray(0);
    // normal
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(3*sizeof(float)));
    glEnableVertexAttribArray(1);
    // uv
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)(6*sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    return m;
}

// ============================================================
// MAIN
// ============================================================
int main() {
    if (!glfwInit()) { printf("GLFW init failed\n"); return 1; }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SAMPLES, 4);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "Neural Test - GLFW+OpenGL", nullptr, nullptr);
    if (!window) { printf("Window creation failed\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    glfwSetMouseButtonCallback(window, mouseButtonCB);
    glfwSetCursorPosCallback(window, cursorPosCB);
    glfwSetScrollCallback(window, scrollCB);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        printf("GLAD init failed\n"); return 1;
    }
    printf("OpenGL %s\n", glGetString(GL_VERSION));

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE);
    glClearColor(6.0f/255, 8.0f/255, 18.0f/255, 1.0f);

    // Compile shaders
    GLuint somaProg = createProgram(somaVS, somaFS);
    GLuint tubeProg = createProgram(tubeVS, tubeFS);

    // Build sphere mesh (higher quality than D3D9's 8x12)
    std::vector<Vertex> sphereV; std::vector<unsigned int> sphereI;
    buildUVSphere(16, 24, sphereV, sphereI);
    Mesh sphereMesh = uploadMesh(sphereV, sphereI);

    // Setup neurons
    Neuron neurons[2];
    memset(neurons, 0, sizeof(neurons));
    neurons[0].pos = {-2.0f, 0.0f, 0.0f};
    neurons[0].radius = 0.8f;
    neurons[0].firePeriod = 2.5f;
    neurons[0].fireTimer = 1.0f;
    neurons[1].pos = { 2.0f, 0.0f, 0.0f};
    neurons[1].radius = 0.7f;
    neurons[1].firePeriod = 999.0f; // only fires when signal arrives
    neurons[1].fireTimer = 999.0f;

    // Setup synapse
    Synapse synapses[1];
    synapses[0].src = 0; synapses[0].dst = 1;
    synapses[0].activity = 0; synapses[0].signalTime = -1;
    synapses[0].delay = 0.8f; synapses[0].weight = 1.0f;

    // Build tube mesh
    float tubeR = 0.08f;
    float p1[3] = {neurons[0].pos.x + neurons[0].radius*0.9f, 0, 0};
    float p2[3] = {neurons[1].pos.x - neurons[1].radius*0.9f, 0, 0};
    std::vector<Vertex> tubeV; std::vector<unsigned int> tubeI;
    buildTube(p1, tubeR, p2, tubeR*0.6f, 8, tubeV, tubeI);
    Mesh tubeMesh = uploadMesh(tubeV, tubeI);

    float camDist = 6.0f;
    Vec3 camTarget = {0, 0, 0};
    float lightDir[3] = {-0.5f, -0.8f, 0.3f};

    double lastTime = glfwGetTime();
    int frameCount = 0;
    double fpsTime = lastTime;

    // Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        double now = glfwGetTime();
        float dt = (float)(now - lastTime);
        lastTime = now;
        if (dt > 0.1f) dt = 0.1f;

        // FPS counter in title
        frameCount++;
        if (now - fpsTime >= 1.0) {
            char title[256];
            snprintf(title, sizeof(title),
                "Neural Test | FPS: %d | V0=%.2f V1=%.2f | Drag to orbit, scroll to zoom",
                frameCount, neurons[0].voltage, neurons[1].voltage);
            glfwSetWindowTitle(window, title);
            frameCount = 0; fpsTime = now;
        }

        // Update simulation
        updateSimulation(neurons, 2, synapses, 1, dt, (float)now);

        // Camera
        float dist = camDist / camZoom;
        Vec3 eye;
        eye.x = camTarget.x + dist * sinf(camAngleY) * cosf(camAngleX);
        eye.y = camTarget.y + dist * sinf(camAngleX);
        eye.z = camTarget.z + dist * cosf(camAngleY) * cosf(camAngleX);

        int ww, wh;
        glfwGetFramebufferSize(window, &ww, &wh);
        if (ww < 1) ww = 1; if (wh < 1) wh = 1;
        glViewport(0, 0, ww, wh);

        Mat4 view = mat4_lookAt(eye, camTarget, {0,1,0});
        Mat4 proj = mat4_perspective(50.0f * PI / 180.0f, (float)ww/(float)wh, 0.1f, 100.0f);
        Mat4 vp = mat4_multiply(proj, view);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Draw neurons
        glUseProgram(somaProg);
        glUniform3f(glGetUniformLocation(somaProg, "uLightDir"), lightDir[0], lightDir[1], lightDir[2]);
        glUniform3f(glGetUniformLocation(somaProg, "uCamPos"), eye.x, eye.y, eye.z);

        for (int i=0; i<2; i++) {
            Neuron& n = neurons[i];
            Mat4 model = mat4_multiply(mat4_translate(n.pos.x, n.pos.y, n.pos.z), mat4_scale(n.radius));
            Mat4 mvp = mat4_multiply(vp, model);
            glUniformMatrix4fv(glGetUniformLocation(somaProg, "uMVP"), 1, GL_FALSE, mvp.m);
            glUniformMatrix4fv(glGetUniformLocation(somaProg, "uModel"), 1, GL_FALSE, model.m);
            glUniform1f(glGetUniformLocation(somaProg, "uMemPhase"), n.phase);
            glUniform1f(glGetUniformLocation(somaProg, "uWobbleAmp"), 0.03f + n.voltage * 0.08f);

            // Bake voltage into UV.x of sphere vertices
            std::vector<Vertex> vCopy = sphereV;
            for (auto& v : vCopy) v.u = n.voltage;
            glBindVertexArray(sphereMesh.vao);
            glBindBuffer(GL_ARRAY_BUFFER, sphereMesh.vbo);
            glBufferSubData(GL_ARRAY_BUFFER, 0, vCopy.size()*sizeof(Vertex), vCopy.data());
            glDrawElements(GL_TRIANGLES, sphereMesh.indexCount, GL_UNSIGNED_INT, 0);
        }

        // Draw tube
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);

        glUseProgram(tubeProg);
        Mat4 tubeModel = mat4_identity();
        Mat4 tubeMVP = mat4_multiply(vp, tubeModel);
        glUniformMatrix4fv(glGetUniformLocation(tubeProg, "uMVP"), 1, GL_FALSE, tubeMVP.m);
        glUniform3f(glGetUniformLocation(tubeProg, "uLightDir"), lightDir[0], lightDir[1], lightDir[2]);
        glUniform3f(glGetUniformLocation(tubeProg, "uCamPos"), eye.x, eye.y, eye.z);
        glUniform1f(glGetUniformLocation(tubeProg, "uActivity"), synapses[0].activity);

        glBindVertexArray(tubeMesh.vao);
        glDrawElements(GL_TRIANGLES, tubeMesh.indexCount, GL_UNSIGNED_INT, 0);

        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);

        glfwSwapBuffers(window);
    }

    glDeleteProgram(somaProg);
    glDeleteProgram(tubeProg);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
