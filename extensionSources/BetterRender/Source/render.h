#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <directxmath.h>

ID3D11VertexShader* vertexShader;
ID3D11PixelShader* pixelShader;
ID3D11InputLayout* inputLayout;
ID3D11Buffer* vertexBuffer;

struct Vertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT4 color;
};

Vertex vertices[] = {
    { { -0.5f,  0.5f, 0.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
    { {  0.5f,  0.5f, 0.0f }, { 0.0f, 1.0f, 0.0f, 1.0f } },
    { {  0.5f, -0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } },
    { { -0.5f, -0.5f, 0.0f }, { 1.0f, 1.0f, 0.0f, 1.0f } }
};

void render_init(ID3D11Device* device) {
    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.ByteWidth = sizeof(vertices);
    bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bufferDesc.CPUAccessFlags = 0;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = vertices;

    device->CreateBuffer(&bufferDesc, &initData, &vertexBuffer);

    const char* vsSource = 
    "struct VS_INPUT {"
    "    float4 Pos : POSITION;"
    "    float4 Color : COLOR;"
    "};"
    "struct PS_INPUT {"
    "    float4 Pos : SV_POSITION;"
    "    float4 Color : COLOR;"
    "};"
    "PS_INPUT VS(VS_INPUT input) {"
    "    PS_INPUT output;"
    "    output.Pos = input.Pos;"
    "    output.Color = input.Color;"
    "    return output;"
    "}";

    const char* psSource = 
    "struct PS_INPUT {"
    "    float4 Pos : SV_POSITION;"
    "    float4 Color : COLOR;"
    "};"
    "float4 PS(PS_INPUT input) : SV_TARGET {"
    "    return input.Color;"
    "}";

    ID3DBlob* vsBlob;
    ID3DBlob* psBlob;
    D3DCompile(vsSource, strlen(vsSource), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vsBlob, nullptr);
    D3DCompile(psSource, strlen(psSource), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &psBlob, nullptr);

    device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vertexShader);
    device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pixelShader);

    // Определение входного формата
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    UINT numElements = ARRAYSIZE(layout);
    device->CreateInputLayout(layout, numElements, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &inputLayout);

    vsBlob->Release();
}

void render_draw(ID3D11DeviceContext* context) {
    // Установка шейдеров
    context->VSSetShader(vertexShader, nullptr, 0);
    context->PSSetShader(pixelShader, nullptr, 0);
    
    // Установка входного формата
    context->IASetInputLayout(inputLayout);
    
    // Установка вершинного буфера
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
    
    // Установка примитивной топологии
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    
    // Отрисовка
    context->Draw(4, 0); // Отрисовка 4 вершин (квадрат)
}