#include "debugrenderer.h"
#include "config.h"
#include <cstdarg>
#include <cstdio>
//Eriumsss
// ============================================================================
// Debug Renderer Implementation
// Uses D3DXCreateFont from the game's already-loaded d3dx9_38.dll
// ============================================================================

// D3DX Font interface and function pointer
typedef interface ID3DXFont ID3DXFont;
typedef ID3DXFont* LPD3DXFONT;

// D3DXCreateFontA function signature
typedef HRESULT (WINAPI *D3DXCreateFontA_t)(
    IDirect3DDevice9* pDevice,
    INT Height, UINT Width,
    UINT Weight, UINT MipLevels,
    BOOL Italic,
    DWORD CharSet, DWORD OutputPrecision,
    DWORD Quality, DWORD PitchAndFamily,
    LPCSTR pFaceName,
    LPD3DXFONT* ppFont
);

// ID3DXFont vtable (we only need DrawTextA at index 14)
// Actually let's define the interface properly
#undef INTERFACE
#define INTERFACE ID3DXFont
DECLARE_INTERFACE_(ID3DXFont, IUnknown)
{
    // IUnknown
    STDMETHOD(QueryInterface)(THIS_ REFIID iid, LPVOID *ppv) PURE;
    STDMETHOD_(ULONG, AddRef)(THIS) PURE;
    STDMETHOD_(ULONG, Release)(THIS) PURE;
    // ID3DXFont
    STDMETHOD(GetDevice)(THIS_ IDirect3DDevice9** ppDevice) PURE;
    STDMETHOD(GetDescA)(THIS_ void* pDesc) PURE;
    STDMETHOD(GetDescW)(THIS_ void* pDesc) PURE;
    STDMETHOD_(BOOL, GetTextMetricsA)(THIS_ TEXTMETRICA* pTextMetrics) PURE;
    STDMETHOD_(BOOL, GetTextMetricsW)(THIS_ TEXTMETRICW* pTextMetrics) PURE;
    STDMETHOD_(HDC, GetDC)(THIS) PURE;
    STDMETHOD(GetGlyphData)(THIS_ UINT Glyph, IDirect3DTexture9** ppTexture, RECT* pBlackBox, POINT* pCellInc) PURE;
    STDMETHOD(PreloadCharacters)(THIS_ UINT First, UINT Last) PURE;
    STDMETHOD(PreloadGlyphs)(THIS_ UINT First, UINT Last) PURE;
    STDMETHOD(PreloadTextA)(THIS_ LPCSTR pString, INT Count) PURE;
    STDMETHOD(PreloadTextW)(THIS_ LPCWSTR pString, INT Count) PURE;
    STDMETHOD_(INT, DrawTextA)(THIS_ void* pSprite, LPCSTR pString, INT Count, LPRECT pRect, DWORD Format, D3DCOLOR Color) PURE;
    STDMETHOD_(INT, DrawTextW)(THIS_ void* pSprite, LPCWSTR pString, INT Count, LPRECT pRect, DWORD Format, D3DCOLOR Color) PURE;
    STDMETHOD(OnLostDevice)(THIS) PURE;
    STDMETHOD(OnResetDevice)(THIS) PURE;
};

namespace DebugRenderer {

    static IDirect3DDevice9* g_Device = nullptr;
    static ID3DXFont* g_D3DXFont = nullptr;
    static bool g_Ready = false;
    static D3DXCreateFontA_t g_pD3DXCreateFontA = nullptr;

    // Vertex format for primitives
    struct Vertex {
        float x, y, z, rhw;
        DWORD color;
    };
    const DWORD VERTEX_FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

    bool Initialize(IDirect3DDevice9* device) {
        if (!device) return false;
        g_Device = device;

        // Try to get D3DXCreateFontA from the game's already-loaded d3dx9_38.dll
        HMODULE hD3DX = GetModuleHandleA("d3dx9_38.dll");
        if (!hD3DX) {
            // Try loading it ourselves
            hD3DX = LoadLibraryA("d3dx9_38.dll");
        }

        if (hD3DX) {
            g_pD3DXCreateFontA = (D3DXCreateFontA_t)GetProcAddress(hD3DX, "D3DXCreateFontA");
        }

        if (g_pD3DXCreateFontA) {
            HRESULT hr = g_pD3DXCreateFontA(
                device,
                Config::FONT_SIZE, 0,
                FW_BOLD, 1,
                FALSE,
                DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,
                ANTIALIASED_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE,
                Config::FONT_NAME,
                &g_D3DXFont
            );
            g_Ready = SUCCEEDED(hr) && g_D3DXFont;
        }

        return g_Ready;
    }

    void Release() {
        if (g_D3DXFont) {
            g_D3DXFont->Release();
            g_D3DXFont = nullptr;
        }
        g_Ready = false;
    }

    void BeginFrame(IDirect3DDevice9* device) {
        if (!g_Ready && device) {
            Initialize(device);
        }
        g_Device = device;
    }

    void EndFrame() {
        // Nothing to do
    }

    void DrawText(int x, int y, DWORD color, const char* text) {
        if (!g_D3DXFont || !text) return;

        RECT rect = { x, y, x + 1000, y + 100 };
        g_D3DXFont->DrawTextA(nullptr, text, -1, &rect, DT_NOCLIP, color);
    }

    void DrawText(int x, int y, DWORD color, const std::string& text) {
        DrawText(x, y, color, text.c_str());
    }

    void DrawTextShadowed(int x, int y, DWORD color, const char* text) {
        DrawText(x + 1, y + 1, 0xFF000000, text);
        DrawText(x, y, color, text);
    }

    void DrawTextF(int x, int y, DWORD color, const char* format, ...) {
        char buffer[512];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        DrawTextShadowed(x, y, color, buffer);
    }

    void DrawLine(float x1, float y1, float x2, float y2, DWORD color) {
        if (!g_Device) return;

        Vertex vertices[2] = {
            { x1, y1, 0.0f, 1.0f, color },
            { x2, y2, 0.0f, 1.0f, color }
        };

        g_Device->SetFVF(VERTEX_FVF);
        g_Device->SetTexture(0, NULL);
        g_Device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_Device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_Device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        g_Device->DrawPrimitiveUP(D3DPT_LINELIST, 1, vertices, sizeof(Vertex));
    }

    void DrawRect(float x, float y, float width, float height, DWORD color) {
        DrawLine(x, y, x + width, y, color);
        DrawLine(x + width, y, x + width, y + height, color);
        DrawLine(x + width, y + height, x, y + height, color);
        DrawLine(x, y + height, x, y, color);
    }

    void DrawFilledRect(float x, float y, float width, float height, DWORD color) {
        if (!g_Device) return;

        Vertex vertices[4] = {
            { x,         y,          0.0f, 1.0f, color },
            { x + width, y,          0.0f, 1.0f, color },
            { x,         y + height, 0.0f, 1.0f, color },
            { x + width, y + height, 0.0f, 1.0f, color }
        };

        g_Device->SetFVF(VERTEX_FVF);
        g_Device->SetTexture(0, NULL);
        g_Device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_Device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_Device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        g_Device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));
    }

    void GetTextSize(const char* text, int* width, int* height) {
        if (width) *width = text ? (int)strlen(text) * 8 : 0;
        if (height) *height = Config::FONT_SIZE;
    }

    bool IsReady() {
        return g_Ready;
    }

    // ---- 3D Rendering Implementation ----

    // Debug info for WorldToScreen diagnostics
    static bool g_W2SDebugEnabled = false;
    static int g_W2SFailReason = 0;  // 0=ok, 1=no device, 2=view fail, 3=proj fail, 4=vp fail, 5=behind cam
    static D3DMATRIX g_LastView = {0};
    static D3DMATRIX g_LastProj = {0};
    static D3DVIEWPORT9 g_LastViewport = {0};

    void EnableW2SDebug(bool enable) { g_W2SDebugEnabled = enable; }
    int GetW2SFailReason() { return g_W2SFailReason; }
    const D3DMATRIX* GetLastViewMatrix() { return &g_LastView; }
    const D3DMATRIX* GetLastProjMatrix() { return &g_LastProj; }
    const D3DVIEWPORT9* GetLastViewport() { return &g_LastViewport; }

    // Game uses standard XYZ coordinate system (Y is height) based on WorldTransform matrix layout
    // Matrix format: [0-11]=rotation, [12]=X, [13]=Y(height), [14]=Z, [15]=1
    bool WorldToScreen(float worldX, float worldY, float worldZ, float* screenX, float* screenY) {
        g_W2SFailReason = 0;
        if (!g_Device) { g_W2SFailReason = 1; return false; }

        // Get view and projection matrices from D3D
        D3DMATRIX view, proj;
        if (FAILED(g_Device->GetTransform(D3DTS_VIEW, &view))) { g_W2SFailReason = 2; return false; }
        if (FAILED(g_Device->GetTransform(D3DTS_PROJECTION, &proj))) { g_W2SFailReason = 3; return false; }

        // Get viewport
        D3DVIEWPORT9 vp;
        if (FAILED(g_Device->GetViewport(&vp))) { g_W2SFailReason = 4; return false; }

        // Store for debug
        g_LastView = view;
        g_LastProj = proj;
        g_LastViewport = vp;

        // Check if matrices are identity (game might not use D3D transforms)
        bool viewIsIdentity = (view._11 == 1.0f && view._22 == 1.0f && view._33 == 1.0f &&
                               view._41 == 0.0f && view._42 == 0.0f && view._43 == 0.0f);

        // Transform world position through view matrix
        // Note: Game uses XZY, so worldY is actually forward/back, worldZ is height
        float vx = worldX * view._11 + worldY * view._21 + worldZ * view._31 + view._41;
        float vy = worldX * view._12 + worldY * view._22 + worldZ * view._32 + view._42;
        float vz = worldX * view._13 + worldY * view._23 + worldZ * view._33 + view._43;
        float vw = worldX * view._14 + worldY * view._24 + worldZ * view._34 + view._44;

        // Transform through projection matrix
        float px = vx * proj._11 + vy * proj._21 + vz * proj._31 + vw * proj._41;
        float py = vx * proj._12 + vy * proj._22 + vz * proj._32 + vw * proj._42;
        float pz = vx * proj._13 + vy * proj._23 + vz * proj._33 + vw * proj._43;
        float pw = vx * proj._14 + vy * proj._24 + vz * proj._34 + vw * proj._44;

        // Check if behind camera
        if (pw <= 0.001f) { g_W2SFailReason = 5; return false; }

        // Perspective divide
        float ndcX = px / pw;
        float ndcY = py / pw;

        // Convert from NDC [-1,1] to screen coordinates
        *screenX = (ndcX + 1.0f) * 0.5f * vp.Width + vp.X;
        *screenY = (1.0f - ndcY) * 0.5f * vp.Height + vp.Y;

        return true;
    }

    void DrawLine3D(float x1, float y1, float z1, float x2, float y2, float z2, DWORD color) {
        float sx1, sy1, sx2, sy2;
        if (WorldToScreen(x1, y1, z1, &sx1, &sy1) && WorldToScreen(x2, y2, z2, &sx2, &sy2)) {
            DrawLine(sx1, sy1, sx2, sy2, color);
        }
    }

    // Game uses standard XYZ: X=left/right, Y=height, Z=forward/back
    void DrawBox3D(float x, float y, float z, float sizeX, float sizeY, float sizeZ, DWORD color) {
        // Calculate half sizes for centering
        float hx = sizeX * 0.5f;
        float hy = sizeY * 0.5f;  // height
        float hz = sizeZ * 0.5f;

        // 8 corners of the box (XYZ order - Y is up)
        float corners[8][3] = {
            {x - hx, y - hy, z - hz},  // 0: bottom-back-left
            {x + hx, y - hy, z - hz},  // 1: bottom-back-right
            {x + hx, y + hy, z - hz},  // 2: top-back-right
            {x - hx, y + hy, z - hz},  // 3: top-back-left
            {x - hx, y - hy, z + hz},  // 4: bottom-front-left
            {x + hx, y - hy, z + hz},  // 5: bottom-front-right
            {x + hx, y + hy, z + hz},  // 6: top-front-right
            {x - hx, y + hy, z + hz},  // 7: top-front-left
        };

        // 12 edges of the box
        int edges[12][2] = {
            {0, 1}, {1, 2}, {2, 3}, {3, 0},  // back face
            {4, 5}, {5, 6}, {6, 7}, {7, 4},  // front face
            {0, 4}, {1, 5}, {2, 6}, {3, 7},  // connecting edges
        };

        for (int i = 0; i < 12; i++) {
            int a = edges[i][0];
            int b = edges[i][1];
            DrawLine3D(corners[a][0], corners[a][1], corners[a][2],
                       corners[b][0], corners[b][1], corners[b][2], color);
        }
    }
}

