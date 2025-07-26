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

// -----------------------------------

static HRESULT (*gameDXGIPresent) (IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flag);
static ID3D11Device* device = NULL;
static ID3D11DeviceContext* context = NULL;
static ID3D11Texture2D* overlayTexture;
static ID3D11RenderTargetView* overlayRender;

HRESULT hookDXGIPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    gameDXGIPresent(pSwapChain, SyncInterval, Flags);

    if (!device) {
        pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&device);
        device->GetImmediateContext(&context);

        D3D11_TEXTURE2D_DESC textureDesc = {};
        textureDesc.Width = 512; // Ширина текстуры
        textureDesc.Height = 512; // Высота текстуры
        textureDesc.MipLevels = 1; // Количество уровней мипмапинга
        textureDesc.ArraySize = 1; // Количество текстур в массиве
        textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // Формат текстуры
        textureDesc.SampleDesc.Count = 1; // Количество образцов
        textureDesc.Usage = D3D11_USAGE_DEFAULT; // Использование текстуры
        textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE; // Флаги привязки
        textureDesc.CPUAccessFlags = 0;

        device->CreateTexture2D(&textureDesc, nullptr, &overlayTexture);
        device->CreateRenderTargetView(overlayTexture, nullptr, &overlayRender);
        context->OMSetRenderTargets(1, &overlayRender, nullptr);
    }

    float colorTop[] = { 0.0f, 0.0f, 1.0f, 1.0f };
    float colorBottom[] = { 1.0f, 0.0f, 0.0f, 1.0f };

    for (int y = 0; y < 600; ++y) {
        float t = static_cast<float>(y) / 600.0f;
        float color[] = {
            colorTop[0] * (1 - t) + colorBottom[0] * t,
            colorTop[1] * (1 - t) + colorBottom[1] * t,
            colorTop[2] * (1 - t) + colorBottom[2] * t,
            1.0f
        };
        context->ClearRenderTargetView(overlayRender, color);
        context->OMSetRenderTargets(1, &overlayRender, nullptr);
        context->Draw(3, 0);
    }

    return S_OK;
}

// -----------------------------------

LRESULT CALLBACK DummyWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
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