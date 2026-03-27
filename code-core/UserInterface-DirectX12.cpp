// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

#include "UserInterface-DirectX12.h"
#include <d3dcompiler.h>
#include <MemoryManagerGPU-DirectX12.h>
#include "विश्वकर्मा.h"
extern शंकर gpu;
extern std::atomic<uint16_t*> publishedTabIndexes;
extern std::atomic<uint16_t>  publishedTabCount;
std::atomic<uint32_t> actionWriteIndex;
// Shader compilation helper
static void CompileShader( const char* code, const char* entry, const char* target, UINT flags,
    ComPtr<ID3DBlob>& outBlob) {

    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile( code, strlen(code), nullptr, nullptr, nullptr, entry, target, flags, 0,
        &outBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) { std::cerr << (char*)errorBlob->GetBufferPointer() << std::endl; }
        ThrowIfFailed(hr);
    }
}

void InitUIResources( DX12ResourcesUI& uiRes, ID3D12Device* device) {
    // Root signature
    CD3DX12_ROOT_PARAMETER1 rootParams[1];
    rootParams[0].InitAsConstantBufferView(0,0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE, D3D12_SHADER_VISIBILITY_VERTEX);
    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootDesc;
    rootDesc.Init_1_1( _countof(rootParams), rootParams, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;

    D3DX12SerializeVersionedRootSignature( &rootDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &signature, &error);
    ThrowIfFailed( device->CreateRootSignature( 0, signature->GetBufferPointer(), signature->GetBufferSize(),
        IID_PPV_ARGS(&uiRes.uiRootSignature)));

    // Shaders

#if defined(_DEBUG)
    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    UINT compileFlags = 0;
#endif

    static const char* vsCode = R"(
cbuffer OrthoConstantBuffer : register(b0) {
    float4x4 ortho;
};

struct VSInput {
    float2 position : POSITION;
    float2 uv       : TEXCOORD0;
    uint   color    : COLOR0;
};

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
    uint   color    : COLOR0;
};

PSInput VSMain(VSInput input) {
    PSInput output;
    float4 pos = float4(input.position,0,1);
    output.position = mul(pos,ortho);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
)";

    static const char* psCode = R"(
struct PSInput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
    uint   color    : COLOR0;
};

float4 PSMain(PSInput input) : SV_TARGET {
    float r = ((input.color >> 0) & 0xFF) / 255.0;
    float g = ((input.color >> 8) & 0xFF) / 255.0;
    float b = ((input.color >> 16) & 0xFF) / 255.0;
    float a = ((input.color >> 24) & 0xFF) / 255.0;
    return float4(r,g,b,a);
}
)";

    ComPtr<ID3DBlob> vsBlob;
    ComPtr<ID3DBlob> psBlob;
    CompileShader(vsCode, "VSMain", "vs_5_0", compileFlags, vsBlob);
    CompileShader(psCode, "PSMain", "ps_5_0", compileFlags, psBlob);
    // Input layout

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0 },
        { "TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0 },
        { "COLOR",0,DXGI_FORMAT_R32_UINT,0,16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0 }
    };

    // PSO
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { layout,_countof(layout) };
    psoDesc.pRootSignature = uiRes.uiRootSignature.Get();
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(vsBlob.Get());
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(psBlob.Get());
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    ThrowIfFailed( device->CreateGraphicsPipelineState( &psoDesc, IID_PPV_ARGS(&uiRes.uiPSO)));

    // Vertex buffer
    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    auto vbDesc = CD3DX12_RESOURCE_DESC::Buffer( uiRes.maxVertices * sizeof(UIVertex));
    ThrowIfFailed( device->CreateCommittedResource( &uploadHeap, D3D12_HEAP_FLAG_NONE, &vbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uiRes.uiVertexBuffer)));

    auto ibDesc = CD3DX12_RESOURCE_DESC::Buffer( uiRes.maxIndices * sizeof(uint16_t));
    ThrowIfFailed( device->CreateCommittedResource( &uploadHeap, D3D12_HEAP_FLAG_NONE, &ibDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uiRes.uiIndexBuffer)));

    auto cbDesc = CD3DX12_RESOURCE_DESC::Buffer(256);
    ThrowIfFailed( device->CreateCommittedResource( &uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uiRes.uiOrthoConstantBuffer)));

    CD3DX12_RANGE readRange(0, 0);
    uiRes.uiVertexBuffer->Map( 0, &readRange, reinterpret_cast<void**>(&uiRes.pVertexDataBegin));
    uiRes.uiIndexBuffer->Map(  0, &readRange, reinterpret_cast<void**>(&uiRes.pIndexDataBegin));
    uiRes.uiOrthoConstantBuffer->Map( 0, &readRange, reinterpret_cast<void**>(&uiRes.pOrthoDataBegin));
    std::wcout << L"UI Resources Initialized (Phase 4A)\n";
}

// Cleanup
void CleanupUIResources(DX12ResourcesUI& uiRes) {
    if (uiRes.uiVertexBuffer) uiRes.uiVertexBuffer->Unmap(0, nullptr);
    if (uiRes.uiIndexBuffer) uiRes.uiIndexBuffer->Unmap(0, nullptr);
    if (uiRes.uiOrthoConstantBuffer) uiRes.uiOrthoConstantBuffer->Unmap(0, nullptr);

    uiRes = {};
}

// PushRect
void PushRect( UIDrawContext& ctx, float x, float y, float w, float h,
    uint32_t color, DX12ResourcesUI& uiRes) {
    if (ctx.vertexCount + 4 > uiRes.maxVertices) return;
    if (ctx.indexCount + 6 > uiRes.maxIndices) return;

    uint16_t base = ctx.vertexCount;

    ctx.vertexPtr[0] = { x,y,0,0,color };
    ctx.vertexPtr[1] = { x + w,y,0,0,color };
    ctx.vertexPtr[2] = { x + w,y + h,0,0,color };
    ctx.vertexPtr[3] = { x,y + h,0,0,color };

    ctx.indexPtr[0] = base + 0;
    ctx.indexPtr[1] = base + 1;
    ctx.indexPtr[2] = base + 2;
    ctx.indexPtr[3] = base + 0;
    ctx.indexPtr[4] = base + 2;
    ctx.indexPtr[5] = base + 3;

    ctx.vertexPtr += 4;
    ctx.indexPtr += 6;
    ctx.vertexCount += 4;
    ctx.indexCount += 6;
}

// Returns true if clicked this frame
bool PushInteractiveRect(UIDrawContext& ctx, float x, float y, float w, float h, uint32_t baseColor,
    uint32_t id, const UIInput& input, DX12ResourcesUI& uiRes, bool enabled = true) {
    uint32_t color = baseColor;

    bool hovered = enabled && (input.mouseX >= x && input.mouseX < x + w &&
        input.mouseY >= y && input.mouseY < y + h);

    if (hovered) color = 0xFF555555; // hover tint (TODO: theme-aware)
    if (hovered && input.leftButtonDown) color = 0xFF333333; // pressed tint
    if (!enabled) color = 0xFF1E1E1E; // If disabled, force a darker/grayer base color
    
    PushRect(ctx, x, y, w, h, color, uiRes);

    if (!enabled) return false;// Disabled controls do NOT respond to clicks
    if (hovered && input.leftButtonPressedThisFrame) {
        return true;
    }
    return false;
}

// Convenience for tabs (fixed width for now)
bool PushTab(UIDrawContext& ctx, float x, float w, float h, uint16_t tabID, bool isActive,
    const UIInput& input, DX12ResourcesUI& uiRes) {
    
    uint32_t color = isActive ? COLOR_UI_TAB_ACTIVE : COLOR_UI_TAB_INACTIVE;
    uint32_t actionID = 0x10000000u | tabID;   // high bit = tab family

    if (PushInteractiveRect(ctx, x, 0, w, h, color, actionID, input, uiRes)) {
        // Optional immediate feedback (UI thread)
        // window.activeTabIndex = tabID;   // you can still do it here if you want instant visual
        return true;
    }
    return false;
}

// This function renders the list of tabs, all top menu buttons (with dropdowns if required),
// side favourite / frequent buttons bars, right side property window, bottom status bar.
// This is also responsible for all relevant DirectX12 configurations required for rendering User Interface.
void RenderUIOverlay(SingleUIWindow& window, ID3D12GraphicsCommandList* cmd, DX12ResourcesUI& uiRes,
    float monitorDPI, const UIInput& input) {
    
    if (!cmd) return; //Defensive check.

    cmd->SetPipelineState(uiRes.uiPSO.Get());
    cmd->SetGraphicsRootSignature(uiRes.uiRootSignature.Get());

    float W = (float)window.dx.WindowWidth;
    float H = (float)window.dx.WindowHeight;
    float* ortho = (float*)uiRes.pOrthoDataBegin;

    ortho[0] = 2 / W; ortho[1] = 0;   ortho[2] = 0; ortho[3] = -1;
    ortho[4] = 0;   ortho[5] = -2 / H; ortho[6] = 0; ortho[7] = 1;
    ortho[8] = 0;   ortho[9] = 0;   ortho[10] = 1; ortho[11] = 0;
    ortho[12] = 0;  ortho[13] = 0;  ortho[14] = 0; ortho[15] = 1;

    cmd->SetGraphicsRootConstantBufferView( 0, uiRes.uiOrthoConstantBuffer->GetGPUVirtualAddress());

    UIDrawContext ctx;
    ctx.vertexPtr = reinterpret_cast<UIVertex*>(uiRes.pVertexDataBegin);
    ctx.indexPtr = reinterpret_cast<uint16_t*>(uiRes.pIndexDataBegin);
    ctx.vertexCount = 0;
    ctx.indexCount = 0;
    float pixelsPerMM = monitorDPI / 25.4f;
    float buttonWidthPx = UI_BUTTON_WIDTH_MM * pixelsPerMM;
    float tabBarHeight = UI_TAB_BAR_HEIGHT_MM * pixelsPerMM;
    float ribbonY = tabBarHeight + UI_DIVIDER_WIDTH_PX;   // ← only one declaration

    // ENGINEERING / PROJECT TABs
    float tabWidth = 120.0f;
    float currentX = 0.0f;

    uint16_t tabCount = publishedTabCount.load(std::memory_order_acquire);
    uint16_t* tabList = publishedTabIndexes.load(std::memory_order_acquire);

    for (uint16_t i = 0; i < tabCount; i++) {
        uint16_t tabID = tabList[i];
        bool active = (window.activeTabIndex == tabID);
        if (PushTab(ctx, currentX, tabWidth, tabBarHeight, tabID, active, input, uiRes)) {
            window.activeTabIndex = tabID; // instant visual feedback
        }
        currentX += tabWidth;
    }

    // TOP BUTTONS (ACTION GROUP BAR)
    currentX = 20.0f; // reset X for action groups

    const float buttonBaseHeight = 32.0f;
    const float buttonGap = 4.0f;
    const float subGroupGap = 18.0f;
    const float groupGap = 28.0f;

    uint32_t currentActionGroupIndex = 0xFFFFFFFF;
    uint32_t currentSubGroupID = 0xFFFFFFFF;

    float groupLabelY = ribbonY;
    float subGroupLabelY = ribbonY + 18.0f;

    for (size_t i = 0; i < TotalUIControls; ++i) {
        const auto& ctrl = AllUIControls[i];

        if (ctrl.actionGrpupIndex != currentActionGroupIndex) { // Action Group change
            if (currentActionGroupIndex != 0xFFFFFFFF) currentX += groupGap;
            currentActionGroupIndex = ctrl.actionGrpupIndex;
            currentSubGroupID = 0xFFFFFFFF;

            PushRect(ctx, currentX, groupLabelY, 6, 48, 0xFF555555, uiRes); // group separator
            currentX += 12.0f;
        }
        
        if (ctrl.actionSubGroupID != currentSubGroupID) { // Sub-Group change
            if (currentSubGroupID != 0xFFFFFFFF) {currentX += subGroupGap;}
            currentSubGroupID = ctrl.actionSubGroupID;

            PushRect(ctx, currentX, subGroupLabelY, 110, 16, 0xFF2D2D30, uiRes); // sub-group label bar
            currentX += 8.0f;
        }

        // Button geometry (vertical stacking support)
        float btnWidth = (ctrl.defaultWidthPX > 0)
            ? ctrl.defaultWidthPX * pixelsPerMM / 25.4f   // interpret as mm
            : buttonWidthPx;
        float btnHeight = buttonBaseHeight * ctrl.noOfVerticalSlots;
        float btnY = ribbonY + 38.0f;

        if (ctrl.noOfVerticalSlots > 1) {btnY += ctrl.verticalSlotNo * buttonBaseHeight;}
        uint32_t baseColor = ctrl.isEnabled ? 0xFF2D2D30 : 0xFF1E1E1E;// Render

        if (ctrl.type == 1 || ctrl.type == 2) {                     // Button or Dropdown trigger
            bool clicked = PushInteractiveRect(ctx, currentX, btnY, btnWidth, btnHeight,
                baseColor, (uint32_t)ctrl.action, input, uiRes, ctrl.isEnabled);

            if (clicked && ctrl.isEnabled) {
                PushUIAction((uint32_t)ctrl.action);
                if (ctrl.zIndex == 1) { // Dropdown trigger
                    window.activeDropdownAction = ctrl.action;
                }
            }

            if (!ctrl.isEnabled) { // Gray-out overlay for disabled controls
                PushRect(ctx, currentX, btnY, btnWidth, btnHeight, 0xAA333333, uiRes);
            }
        }
        else if (ctrl.type == 3) {
            // Future textbox
            PushRect(ctx, currentX, btnY, btnWidth, btnHeight, 0xFF1E1E1E, uiRes);
        }
        else {
            // Plain label
            PushRect(ctx, currentX, btnY, btnWidth, btnHeight, 0xFF2D2D30, uiRes);
        }

        currentX += btnWidth + buttonGap;
    }

    currentX += 30.0f;// Final padding

    // ACTIVE DROPDOWN (placeholder)
    if (window.activeDropdownAction != UIAction::INVALID) {
        float dropX = 400.0f;   // TODO: track real button X for proper positioning
        float dropY = ribbonY + 80.0f;
        PushRect(ctx, dropX, dropY, 160, 220, 0xFF1E1E1E, uiRes);
        window.activeDropdownAction = UIAction::INVALID;   // immediate-mode auto-close
    }

    // DRAW ALL UI GEOMETRY
    if (ctx.indexCount == 0) return;

    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = uiRes.uiVertexBuffer->GetGPUVirtualAddress();
    vbv.SizeInBytes = ctx.vertexCount * sizeof(UIVertex);
    vbv.StrideInBytes = sizeof(UIVertex);

    D3D12_INDEX_BUFFER_VIEW ibv{};
    ibv.BufferLocation = uiRes.uiIndexBuffer->GetGPUVirtualAddress();
    ibv.SizeInBytes = ctx.indexCount * sizeof(uint16_t);
    ibv.Format = DXGI_FORMAT_R16_UINT;

    cmd->IASetVertexBuffers(0, 1, &vbv);
    cmd->IASetIndexBuffer(&ibv);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawIndexedInstanced(ctx.indexCount, 1, 0, 0, 0);
}