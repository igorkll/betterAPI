#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <windows.h>
#include <sys/stat.h>
extern "C" {
    #include <luajit/lua.h>
    #include <luajit/lauxlib.h>
    #include <luajit/lualib.h>
}
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <directxmath.h>
#include <vector>
#include <algorithm>

#define PACKED __attribute__((packed))
#define EXPORT __declspec(dllexport)
#define C_FUNC extern "C"

// -----------------------------------

typedef struct {
    ID3D11Texture2D* texture;
    ID3D11ShaderResourceView* textureView;
} RenderTarget;

static HRESULT (*gameDXGIPresent) (IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flag);
static ID3D11Device* dxDevice = NULL;
static ID3D11DeviceContext* dxContext = NULL;
static std::vector<RenderTarget*> renderTargets;

static RenderTarget* createRenderTarget_screen() {
    RenderTarget* renderTarget = (RenderTarget*)malloc(sizeof(RenderTarget));

    D3D11_TEXTURE2D_DESC textureDesc = {0};
    ZeroMemory(&textureDesc, sizeof(textureDesc));
    textureDesc.Width = 256;
    textureDesc.Height = 256;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    textureDesc.CPUAccessFlags = 0;

    UINT32* textureData = (UINT32*)malloc(textureDesc.Width * textureDesc.Height * sizeof(UINT32));
    for (size_t ix = 0; ix < textureDesc.Width; ix++) {
        for (size_t iy = 0; iy < textureDesc.Height; iy++) {
            textureData[ix + (iy * textureDesc.Width)] = 0xFFFF0000;
        }
    }

    D3D11_SUBRESOURCE_DATA initData = {0};
    initData.pSysMem = textureData;
    initData.SysMemPitch = textureDesc.Width * sizeof(UINT32);
    
    HRESULT hr = dxDevice->CreateTexture2D(&textureDesc, nullptr, &renderTarget->texture);
    assert(hr);
    free(textureData);

    hr = dxDevice->CreateShaderResourceView(renderTarget->texture, nullptr, &renderTarget->textureView);
    assert(hr);

    renderTargets.push_back(renderTarget);
    return renderTarget;
}

static void freeRenderTarget(RenderTarget* renderTarget) {
    auto it = std::find(renderTargets.begin(), renderTargets.end(), renderTarget);
    if (it != renderTargets.end()) {
        renderTargets.erase(it);
    }

    renderTarget->textureView->Release();
    renderTarget->texture->Release();
    free(renderTarget);
}

static void render() {
    
}

HRESULT hookDXGIPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    if (!dxDevice) {
        pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&dxDevice);
        dxDevice->GetImmediateContext(&dxContext);
    }

    for (RenderTarget* renderTarget : renderTargets) {
        
    }

    return gameDXGIPresent(pSwapChain, SyncInterval, Flags);
}

// -----------------------------------

static bool hookEnabled = false;

LRESULT CALLBACK DummyWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

C_FUNC static int _init(lua_State* L) {
    if (hookEnabled) return 0;
    hookEnabled = true;

    // dummy windows
    HINSTANCE hInstance = GetModuleHandle(NULL);

    const char CLASS_NAME[] = "DummyWindowClass";
    WNDCLASS wc = {0};
    wc.lpfnWndProc = DummyWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(
        0, CLASS_NAME, "Dummy Window", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
        NULL, NULL, hInstance, NULL
    );

    if (hwnd == NULL) {
        return 0;
    }

    // create dummy swapchain
    DXGI_SWAP_CHAIN_DESC swapChainDesc = {0};
    swapChainDesc.BufferCount = 1;
    swapChainDesc.BufferDesc.Width = 100;
    swapChainDesc.BufferDesc.Height = 100;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.OutputWindow = hwnd;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.Windowed = TRUE;

    IDXGISwapChain* dummySwapChain = NULL;
    ID3D11Device* dummyDevice = NULL;
    ID3D11DeviceContext* dummyContext = NULL;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        NULL,
        D3D_DRIVER_TYPE_HARDWARE,
        NULL,
        0,
        NULL,
        0,
        D3D11_SDK_VERSION,
        &swapChainDesc,
        &dummySwapChain,
        &dummyDevice,
        NULL,
        &dummyContext
    );
    if (FAILED(hr)) {
        return 0;
    }

    // make hook
    void** vft = *((void***)dummySwapChain);
    DWORD oldProtect;
    VirtualProtect(&vft[8], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
    gameDXGIPresent = (HRESULT(*)(IDXGISwapChain*, UINT, UINT))vft[8];
    vft[8] = (void*)hookDXGIPresent;
    VirtualProtect(&vft[8], sizeof(void*), oldProtect, &oldProtect);

    // delete dummy window and swapchain
    DestroyWindow(hwnd);
    if (dummySwapChain) dummySwapChain->Release();
    if (dummyContext) dummyContext->Release();
    if (dummyDevice) dummyDevice->Release();

    return 0;
}

C_FUNC static int test(lua_State* L) {
    createRenderTarget_screen();

    return 0;
}

// -----------------------------------

static const struct luaL_Reg g_functions[] = {
    {"_init", _init},
    {"test", test},
    
    {NULL, NULL}
};

C_FUNC EXPORT int luaopen_BetterRender(lua_State* L) {
    luaL_register(L, "BetterRender", g_functions);

    return 1;
}