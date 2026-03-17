// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>   // MUST be before d3d12.h
#include <d3d12.h>
#include <d3dx12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <vector>
#include <unordered_map>
#include <iostream>
#include <atomic>

#include "ConstantsApplication.h"
#include "UserInterface-TextTranslations.h"

// Do not #include "विश्वकर्मा.h" otherwise it will lead to circular dependency error. Declare this struct exist.
struct SingleUIWindow; // Add this forward declaration:

using Microsoft::WRL::ComPtr;

/*
User Interface Design: Render the entire UI in DirectX12 itself. UI is a post-geometry overlay.
Fundamental UI Philosophy: Immediate Mode UI (initially). Late stage, Retained mode for Top Ribbons/Active forms.
Pipeline: RenderFrame(){ Render3DScene() RenderUIOverlay() Present() } This file represents UI part.
Rendering Order: Clear > Render3D > DisableDepth > EnableBlend > RenderUI > Present
UI Coordinate System : Use pixel space. Origin: (0,0) = top-left. Screen: X → right, Y → down
Orthographic matrix: float4x4 Ortho = { 2/W, 0, 0, -1, 0, -2/H, 0, 1,  0, 0, 1, 0, 0, 0, 0, 1 }
This allows vertices to be placed directly in pixels.
UI Vertex is different from 3D scene vertex because it mandatorily refers to a texture needing u/v coordinates.
Tab 0 stores common textures and UI elements.
3 primitives:
1st: Rectangles Used for buttons / backgrounds / panels / tab bar, Rendered as: 2 triangles
2nd: Text Rendered via: glyph atlas texture, 3rd: Icons, Also from texture atlas.

We will use FreeType to keep maximum cross platform code base between Windows / Linux / Mac / Android / iOS.
HarfBuzz for Unicode shaping. Initial implementation will skip this and have ASCII characters only.

At startup:
Load font file (TTF) or desired Language / Script. ( All 18 indian scheduled languages covered. )
Generate glyph bitmaps. Pack into texture atlas. Upload to GPU
Glyph packing algorithm: Skyline bin packer

Dedicated Root signature (uiRootSignature), Pipeline State Object (uiPipelineState)
Vertex shader: position → clip space, uv → pass through, color → pass through
Pixel shader: sample atlas, multiply color,
Enable: alpha blending, Blend: SrcAlpha, InvSrcAlpha

Use: DrawIndexedInstanced, No ExecuteIndirect needed. Batch everything.
When drawing text: for each character, lookup glyph, append quad

There are only 2 themes. Light and Dark. No colorful themes. System Light / Dark theme to be followed by default.
This is to facilitate color as discipline code or visually distinguishing different UI elements.
Our initial implementation will not have any Icons !
During advanced stage of development, dedicated fonts depicting the icon will be constructed.
So that same font rendering codes also generates the Icons ! No PNG embedded icon resource at all.
Initial Memory Budget: Font atlas ~4MB, Icon atlas ~2MB, Vertex buffer ~1MB, Index buffer ~256KB

UI Layout:
----------------------------------------------
| TAB BAR    4mm High           MIN-MAX-CLOSE|
--------------------1px-----------------------
| RIBBON    20mm High                        |
--------------------1px-----------------------
| VIEW LIST ( Currently only 1)   4mm High   |
--------------------1px-----------------------
| I |                           |Action  | I |
| C |                           |Pane    | C |
| O |       3D VIEWPORT         |As per  | O |
| N |                           |Right   | N |
| S |                           |Buttons | S |
----------------------------------------------
| What next action user needs to take text   |
----------------------------------------------

Pixels = Millimeters * (Monitor_DPI / 25.4f)

In शंकर::InitD3DPerTab add if (tab.tabID == 0) InitUIResources(tab);
Create the atlas texture once (committed resource, DEFAULT heap).
Upload via Copy Queue exactly like geometry pages (reuse your upload ring buffer you are about to add).
Mark it published immediately and never touch again.
All text in UI shall be mandatorily any of the size : 1.5mm, 2.5mm(default), 4mm. No customizable size.

Phased Rollout ( Phase 4 checklist)

Phase 4A – Tab Bar only (1–2 days)
Hard-coded tab names from publishedTabIndexes.
Colored rects + simple text (even without atlas yet — use dummy color quads).
Clicking changes activeTabIndex and triggers re-render.

Phase 4B – Font Atlas + Text (next)
Add stb_truetype or FreeType atlas.
Render real tab labels + button text.

Phase 4C – Ribbon Bar
Hard-code 3–4 panels (File, Home, View).
8–10 buttons with rect + text (icons later).
Click handlers queue actions to the active tab’s engineering thread.

Phase 4D – Icons & Polish
Custom Font for Icons.
Hover glow, pressed state, tooltips (delayed text draw).

Future: regenerate atlas on WM_DPICHANGED
Far Future: 
Translator / AI edits: Hindi.csv, German.csv, Tamil.csv
Build step runs: generate_ui_text_table.py
Script generates: UserInterface-Text.generated.h
Compiler builds: static constexpr translation tables
Runtime: no file loading, no parsing, pure compile-time arrays
*/

// UI Layout Constants (millimeters)
constexpr float UI_TAB_BAR_HEIGHT_MM = 4.0f;
constexpr float UI_RIBBON_HEIGHT_MM = 20.0f;
constexpr float UI_VIEWLIST_HEIGHT_MM = 4.0f;
constexpr float UI_DIVIDER_WIDTH_PX = 1.0f;

// Colors (ABGR)
constexpr uint32_t COLOR_UI_BG_DARK = 0xFF1E1E1E;
constexpr uint32_t COLOR_UI_TAB_ACTIVE = 0xFF2D2D30;
constexpr uint32_t COLOR_UI_TAB_INACTIVE = 0xFF3E3E42;
constexpr uint32_t COLOR_UI_TAB_HOVER = 0xFF3F3F46;
constexpr uint32_t COLOR_UI_TEXT = 0xFFFFFFFF;

struct UIVertex {// Vertex format
    float x, y, u, v;
    uint32_t color; // ABGR format for DX12 standard
};

struct Glyph { // Store glyph info: Glyph metadata (Phase 4B)
    float uvMinX, uvMinY, uvMaxX, uvMaxY; // Normalized UV coordinates in the atlas (0.0 to 1.0)
    int width, height; // Pixel dimensions for drawing the quad
    int bearingX, bearingY; // Positional offsets (Crucial for baseline alignment)
    int advanceX;
};

// Use char32_t for full UTF-32 Unicode support (necessary for all languages)
inline std::unordered_map<char32_t, Glyph> glyphLookup; // Lookup table.

struct UIButton { // UI Button abstraction
    uint32_t id; // Unique hash for immediate mode tracking
    RECT physicalRect; // Calculated dynamically based on DPI
    const wchar_t* label; // UTF-32 string for HarfBuzz
    int iconIndex; // -1 = no icon, else index into sprite sheet
    char32_t iconCodePoint;// The Unicode point for custom icon font
    bool enabled;
};

struct UITabBarState { // Tab bar state
    int hoveredTab = -1;
    int activeTab; // copied from window.activeTabIndex
};

struct DX12ResourcesUI { // GPU resources
    ComPtr<ID3D12Resource> uiAtlasTexture; // 1024×1024 or 2048×2048 RGBA (or R8 for alpha-only)
    ComPtr<ID3D12Resource> uiVertexBuffer; // Dynamic upload buffer for vertices
    ComPtr<ID3D12Resource> uiIndexBuffer;  // Dynamic upload buffer for indices

    UINT8* pVertexDataBegin = nullptr; // Mapped pointer for immediate writing
    UINT8* pIndexDataBegin = nullptr;
    UINT8* pOrthoDataBegin = nullptr;

    ID3D12Resource* iconAtlas; // Required ?
    ComPtr<ID3D12PipelineState> uiPSO;
    ComPtr<ID3D12RootSignature> uiRootSignature;
    ComPtr<ID3D12Resource> uiOrthoConstantBuffer;

    uint32_t maxVertices = 65536;
    uint32_t maxIndices = 65536 * 3;
};

struct UIDrawContext { // Draw context
    UIVertex* vertexPtr;
    uint16_t* indexPtr;
    uint32_t vertexCount, indexCount;
};

// DirectX12 Immediate Mode UI System (Phase 4A). Tab Bar Rendering Only
// External interfaces of User Interface sub module of the code.
void InitUIResources( DX12ResourcesUI& uiRes, ID3D12Device* device);
void CleanupUIResources( DX12ResourcesUI& uiRes);

void PushRect( UIDrawContext& ctx, float x, float y, float w, float h, uint32_t color, DX12ResourcesUI& uiRes);
void RenderUIOverlay( SingleUIWindow& window, ID3D12GraphicsCommandList* cmdList,
    DX12ResourcesUI& uiRes, float monitorDPI);
