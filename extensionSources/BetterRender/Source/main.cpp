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

#define PACKED __attribute__((packed))
#define EXPORT __declspec(dllexport)
#define C_FUNC extern "C"

#include "render.h"

// -----------------------------------

static HRESULT (*gameDXGIPresent) (IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flag);
static bool dxDeviceInited = false;

LRESULT CALLBACK DummyWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

HRESULT hookDXGIPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    gameDXGIPresent(pSwapChain, SyncInterval, Flags);

    ID3D11Device* device;
    ID3D11DeviceContext* context;
    pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&device);
    device->GetImmediateContext(&context);

    if (!dxDeviceInited) {
        render_init(device);
        dxDeviceInited = true;
    }
    render_draw(context);

    return S_OK;
}

C_FUNC static int _init(lua_State* L) {
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