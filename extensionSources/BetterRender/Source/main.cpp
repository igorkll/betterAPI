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

using namespace DirectX;

// ----------------------------------- globals

static ID3D11Device* dxDevice = NULL;
static ID3D11DeviceContext* dxContext = NULL;
static DXGI_SWAP_CHAIN_DESC dxInfo;

// ----------------------------------- resources

typedef struct {
    float x, y;
    float u, v;
} Vertex;

const char* shader_screen_code_v = R"(
    struct VS_IN {
        float2 pos : POSITION;
        float2 uv : TEXCOORD;
    };

    struct VS_OUT {
        float4 pos : SV_POSITION;
        float2 uv : TEXCOORD;
    };

    VS_OUT VS(VS_IN input) {
        VS_OUT output;
        output.pos = float4(input.pos, 0.0, 1.0);
        output.uv = input.uv;
        return output;
    }
)";

const char* shader_screen_code_p = R"(
    struct VS_OUT {
        float4 pos : SV_POSITION;
        float2 uv : TEXCOORD0;
    };

    Texture2D tex : register(t0);
    SamplerState samp : register(s0);

    float4 PS(VS_OUT input) : SV_TARGET {
        return tex.Sample(samp, input.uv);
    }
)";

static ID3D11VertexShader* shader_screen_v;
static ID3D11PixelShader* shader_screen_p;
static ID3D11Buffer* vertex_buffer;
static ID3D11Buffer* index_buffer;

static void check_dx_error(HRESULT hr, ID3DBlob* errorBlob) {
    if (FAILED(hr)) {
        if (errorBlob) {
            OutputDebugStringA((char*)errorBlob->GetBufferPointer());
            errorBlob->Release();
        } else {
            OutputDebugStringA("unknown dx error");
        }
        abort();
    }
}

static void resources_init() {
    // screen shaders
    ID3DBlob* errorBlob = nullptr;
    ID3DBlob* vsBlob;
    ID3DBlob* psBlob;
    
    HRESULT hr = D3DCompile(shader_screen_code_v, strlen(shader_screen_code_v), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vsBlob, &errorBlob);
    check_dx_error(hr, errorBlob);
    hr = D3DCompile(shader_screen_code_p, strlen(shader_screen_code_p), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &psBlob, &errorBlob);
    check_dx_error(hr, errorBlob);

    dxDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &shader_screen_v);
    dxDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &shader_screen_p);

    // vertices and indices
    Vertex vertices[] = {
        { -0.5f,  0.5f, 0.0f, 0.0f },
        {  0.5f,  0.5f, 1.0f, 0.0f },
        {  0.5f, -0.5f, 1.0f, 1.0f },
        { -0.5f, -0.5f, 0.0f, 1.0f }
    };

    WORD indices[] = { 0, 1, 2, 0, 2, 3 };

    D3D11_BUFFER_DESC vbd = { sizeof(vertices), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER };
    D3D11_SUBRESOURCE_DATA vinitData = { vertices };
    dxDevice->CreateBuffer(&vbd, &vinitData, &vertex_buffer);

    D3D11_BUFFER_DESC ibd = { sizeof(indices), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER };
    D3D11_SUBRESOURCE_DATA iinitData = { indices };
    dxDevice->CreateBuffer(&ibd, &iinitData, &index_buffer);
}

// ----------------------------------- render target

typedef struct {
    ID3D11Texture2D* texture;
    ID3D11ShaderResourceView* textureView;
} RenderTarget;

static HRESULT (*gameDXGIPresent) (IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flag);
static std::vector<RenderTarget*> renderTargets;

static RenderTarget* RenderTarget_create_screen() {
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
    if (FAILED(hr)) abort();
    free(textureData);

    hr = dxDevice->CreateShaderResourceView(renderTarget->texture, nullptr, &renderTarget->textureView);
    if (FAILED(hr)) abort();

    renderTargets.push_back(renderTarget);
    return renderTarget;
}

static void RenderTarget_free(RenderTarget* renderTarget) {
    auto it = std::find(renderTargets.begin(), renderTargets.end(), renderTarget);
    if (it != renderTargets.end()) {
        renderTargets.erase(it);
    }

    renderTarget->textureView->Release();
    renderTarget->texture->Release();
    free(renderTarget);
}

static void RenderTarget_draw(RenderTarget* renderTarget) {
    float posX = 100.0f;
    float posY = 200.0f;
    float scale = 1.0f;

    XMMATRIX worldMatrix = XMMatrixScaling(scale, scale, 1.0f) * XMMatrixTranslation(posX / dxInfo.BufferDesc.Width * 2 - 1, -posY / dxInfo.BufferDesc.Height * 2 + 1, 0.0f);

    struct ConstantBuffer {
        XMMATRIX transform;
    };

    ID3D11Buffer* constantBuffer;
    D3D11_BUFFER_DESC cbDesc = { sizeof(ConstantBuffer), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE };
    dxDevice->CreateBuffer(&cbDesc, nullptr, &constantBuffer);

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    dxContext->Map(constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
    memcpy(mappedResource.pData, &worldMatrix, sizeof(XMMATRIX));
    dxContext->Unmap(constantBuffer, 0);

    // -----------------------------------
    dxContext->VSSetShader(shader_screen_v, nullptr, 0);
    dxContext->PSSetShader(shader_screen_p, nullptr, 0);
    dxContext->PSSetShaderResources(0, 1, &renderTarget->textureView);

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    dxContext->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);
    dxContext->IASetIndexBuffer(index_buffer, DXGI_FORMAT_R16_UINT, 0);
    dxContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    dxContext->DrawIndexed(6, 0, 0);
}

HRESULT hookDXGIPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    if (!dxDevice) {
        pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&dxDevice);
        pSwapChain->GetDesc(&dxInfo);
        dxDevice->GetImmediateContext(&dxContext);
        resources_init();
    }

    for (RenderTarget* renderTarget : renderTargets) {
        RenderTarget_draw(renderTarget);
    }

    return gameDXGIPresent(pSwapChain, SyncInterval, Flags);
}

// ----------------------------------- hook

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

// ----------------------------------- api

C_FUNC static int test(lua_State* L) {
    RenderTarget_create_screen();

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