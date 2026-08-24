// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dx12.h>
#include <wrl.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ConstantsApplication.h" // MV_MAX_SUBTABS: one Cad2DViewState per sub-tab slot.
#include "RenderPage2D.h" // GPU record ABI layouts (Cad2D*GPURecord, Cad2DViewConstants, Cad2DViewState).

struct DX12ResourcesPerWindow;
struct DX12ResourcesUI;

using Microsoft::WRL::ComPtr;

/* A Page2D GPU page: ONE container, ONE record class, fixed capacity (id.md §11.2, step 2c). It
used to be one page per container holding all three classes, which is why every edit rebuilt the
whole drawing.

One class per page because no 2D object emits more than one - lines, polylines and polygons all
expand to line records, circles, ellipses and arcs to curve records, text to glyph quads. So an
object lives in exactly one page and a page never mixes strides. */
enum class Cad2DPageKind : uint8_t { Line = 0, Curve = 1, Text = 2 };

/* 1 MB of records per page: 32,768 lines, 16,384 curves or 10,922 glyph quads. Small enough that
rebuilding one is cheap and a nearly-empty Page2D wastes little, large enough that a million-line
sheet is ~32 pages rather than thousands. An object needing more than this gets a page sized to
fit it - a 40,000-segment polyline has to land somewhere. */
constexpr uint32_t kCad2DPageBytes = 1u << 20;
constexpr uint32_t kCad2DLinePageCapacity = kCad2DPageBytes / sizeof(Cad2DLineGPURecord);
constexpr uint32_t kCad2DCurvePageCapacity = kCad2DPageBytes / sizeof(Cad2DCurveGPURecord);
// Rounded down to whole quads, with the index region sized to match exactly: 6 indices per 4
// vertices, so neither region can fill before the other.
constexpr uint32_t kCad2DTextPageVertexCapacity =
    (kCad2DPageBytes / sizeof(Cad2DTextVertex)) / 4 * 4;
constexpr uint32_t kCad2DTextPageIndexCapacity = kCad2DTextPageVertexCapacity / 4 * 6;

struct Cad2DPageGPU {
    uint64_t containerMemoryId = 0;
    Cad2DPageKind kind = Cad2DPageKind::Line;

    ComPtr<ID3D12Resource> buffer;         // Line / curve records, or text vertices.
    ComPtr<ID3D12Resource> indexBuffer;    // Text only.
    ComPtr<ID3D12Resource> indirectBuffer; // Line / curve only: one command, InstanceCount = count.

    uint32_t capacity = 0;      // Records (or vertices) the buffer can hold. Immutable.
    uint32_t indexCapacity = 0; // Text only. Immutable.

    /* THE ONE FIELD MUTATED AFTER PUBLISH, and the reason appends need no clone. New records are
    written into the unpublished tail [count, capacity), which no drawing frame reads, and only
    then does this advance - a single aligned 4-byte store whose old and new values are both
    valid, so a render thread reads one or the other and never a torn count. That is invariant 2's
    stated exception, the same one InstanceSlotOf and VisibilityMask already use on the 3D side.
    Everything else about a published page is immutable; a modify or a delete rebuilds it. */
    /* A text page's index count is DERIVED from this, never stored: glyph quads are emitted 4
    vertices to 6 indices in lockstep, so `count / 4 * 6` is exact. A second atomic would be a
    second source of truth, and a reader that loaded the two on either side of an append could
    size an index view past the end of the vertices it can see. One load, both numbers. */
    std::atomic<uint32_t> count{ 0 };
};

struct Cad2DPageSnapshot {
    std::vector<Cad2DPageGPU*> pages;
    /* containerMemoryId -> that container's pages, built once when the snapshot is published and
    read-only thereafter - the same directory GeometryPageSnapshot carries, and for the same
    reason: a view visits only the pages it can draw instead of walking every page in the tab. */
    std::unordered_map<uint64_t, std::vector<Cad2DPageGPU*>> pagesByContainer;
};

struct DX12Resources2DPerTab {
    ComPtr<ID3D12RootSignature> lineRootSignature;
    ComPtr<ID3D12PipelineState> linePSO;
    ComPtr<ID3D12CommandSignature> lineCommandSignature;

    ComPtr<ID3D12RootSignature> curveRootSignature;
    ComPtr<ID3D12PipelineState> curvePSO;
    ComPtr<ID3D12CommandSignature> curveCommandSignature;

    ComPtr<ID3D12RootSignature> textRootSignature;
    ComPtr<ID3D12PipelineState> textPSO;
    // The view constant buffer is per window (DX12ResourcesPerWindow::cad2dViewConstantBuffer),
    // not here, so two windows showing different Page2Ds of this tab render independent views.
};

// Opaque platform alias (same pattern as PlatformTabGpu / PlatformWindowGpu): TabCad2DStorage
// holds its GPU-side state through this name; other platforms bind it to their own struct.
using Page2DGpuResources = DX12Resources2DPerTab;

/* objectId -> which of the seven record vectors holds it, and where (id.md §11.4, step 2b). This
replaces the seven per-batch index rebuilds the copy thread used to do, so ingesting K commands
into an N-record tab costs O(K) hash operations rather than O(N).

The stored position stays valid because the record vectors are APPEND-ONLY: nothing erases from
them, and soft delete (isDeleted) keeps the slot. Whoever eventually compacts those vectors has to
rebuild this map in the same pass. */
struct Cad2DRecordLocation {
    VishwakarmaStorage::ObjectType type = VishwakarmaStorage::ObjectType::Unknown;
    uint32_t index = 0;
};

// Which vector a record belongs to, deduced from its own type - so the generic appenders (the
// asset-master lambdas, the copy-thread upsert) need no extra argument to index what they added.
inline VishwakarmaStorage::ObjectType Cad2DKindOf(const Cad2DLineRecordCPU&) { return VishwakarmaStorage::ObjectType::Line2D; }
inline VishwakarmaStorage::ObjectType Cad2DKindOf(const Cad2DPolylineRecordCPU&) { return VishwakarmaStorage::ObjectType::Polyline2D; }
inline VishwakarmaStorage::ObjectType Cad2DKindOf(const Cad2DPolygonRecordCPU&) { return VishwakarmaStorage::ObjectType::Polygon2D; }
inline VishwakarmaStorage::ObjectType Cad2DKindOf(const Cad2DCircleRecordCPU&) { return VishwakarmaStorage::ObjectType::Circle2D; }
inline VishwakarmaStorage::ObjectType Cad2DKindOf(const Cad2DEllipseRecordCPU&) { return VishwakarmaStorage::ObjectType::Ellipse2D; }
inline VishwakarmaStorage::ObjectType Cad2DKindOf(const Cad2DArcRecordCPU&) { return VishwakarmaStorage::ObjectType::Arc2D; }
inline VishwakarmaStorage::ObjectType Cad2DKindOf(const Cad2DTextRecordCPU&) { return VishwakarmaStorage::ObjectType::Text2D; }
inline VishwakarmaStorage::ObjectType Cad2DKindOf(const Cad2DAssetDefinitionRecordCPU&) { return VishwakarmaStorage::ObjectType::Asset2DDefinition; }
inline VishwakarmaStorage::ObjectType Cad2DKindOf(const Cad2DAssetInsertRecordCPU&) { return VishwakarmaStorage::ObjectType::Asset2DInsert; }

struct TabCad2DStorage {
    Page2DGpuResources dx;
    // Pan/zoom moved OUT of here into DATASETTAB::viewports[slot].page2DView (graphics.md, 10M plan
    // Step 6, item 2). It is view state, not content state, and it now sits beside the Scene3D
    // camera in the Viewport that owns both - which is what lets several Viewports show one Page2D
    // panned differently. This struct keeps only what belongs to the CONTENT.

    // 2D click-selection (CPU hit-testing). Selected object ids; the copy thread reads this while
    // rebuilding pages and stamps kCad2DSelectedFlag into the matching GPU records.
    std::mutex selection2DMutex;
    std::unordered_set<uint64_t> selectedObjectIds;

    std::mutex cpuRecordsMutex;
    std::vector<Cad2DLineRecordCPU> lineRecords;
    std::vector<Cad2DPolylineRecordCPU> polylineRecords;
    std::vector<Cad2DPolygonRecordCPU> polygonRecords;
    std::vector<Cad2DCircleRecordCPU> circleRecords;
    std::vector<Cad2DEllipseRecordCPU> ellipseRecords;
    std::vector<Cad2DArcRecordCPU> arcRecords;
    std::vector<Cad2DTextRecordCPU> textRecords;
    // Virtual asset containers (engineering-thread data; nothing here reaches the GPU).
    std::vector<Cad2DAssetDefinitionRecordCPU> assetDefinitionRecords;
    std::vector<Cad2DAssetInsertRecordCPU> assetInsertRecords;

    /* objectId -> (type, position) for ALL NINE record vectors above, guarded by cpuRecordsMutex
    just as they are. Every appender maintains it through Cad2DIndexAppendedRecord; miss one and
    the next upsert of that id appends a DUPLICATE record instead of finding the original.

    The two asset vectors joined at step 2b': they are never ingested by the copy thread, but the
    .yyy loader resolves every incoming record by id, and leaving those two out would have left
    AppendAsset2DInsertToTab scanning a vector that is allowed to hold a million inserts. */
    std::unordered_map<uint64_t, Cad2DRecordLocation> recordIndex;

    std::atomic<Cad2DPageSnapshot*> activeSnapshot{ nullptr };
    std::vector<std::unique_ptr<Cad2DPageGPU>> activePages;

    struct RetiredSnapshot { Cad2DPageSnapshot* snapshot = nullptr; uint64_t retireFence = 0; };
    struct RetiredPage { std::unique_ptr<Cad2DPageGPU> page; uint64_t retireFence = 0; };
    std::vector<RetiredSnapshot> retiredSnapshots;
    std::vector<RetiredPage> retiredPages;

    std::atomic<uint32_t> demoLineCounter{ 0 };
    std::atomic<bool> demoTextQueued{ false };
    // Grid cell the next Cad2DGenerateBulkLines line lands in. Separate from demoLineCounter so
    // the benchmark sheet stays byte-identical between runs whether or not Auto Random has been
    // on - step 2c compares frames of it (id.md §11.4).
    std::atomic<uint32_t> bulkLineCounter{ 0 };

    std::atomic<bool> lineCreationMode{ false };
    std::atomic<bool> lineCreationHasPreviousPoint{ false };
    std::atomic<double> lineCreationPreviousXCU{ 0.0 };
    std::atomic<double> lineCreationPreviousYCU{ 0.0 };

    std::atomic<bool> polylineCreationMode{ false };
    uint64_t polylineCreationObjectId = 0;
    std::vector<Cad2DPoint2D> polylineCreationPoints;

    std::atomic<bool> polygonCreationMode{ false };
    std::atomic<bool> polygonCreationHasCenter{ false };
    std::atomic<double> polygonCreationCenterXCU{ 0.0 };
    std::atomic<double> polygonCreationCenterYCU{ 0.0 };

    std::atomic<bool> circleCreationMode{ false };
    std::atomic<bool> circleCreationHasCenter{ false };
    std::atomic<double> circleCreationCenterXCU{ 0.0 };
    std::atomic<double> circleCreationCenterYCU{ 0.0 };

    std::atomic<bool> ellipseCreationMode{ false };
    std::atomic<uint32_t> ellipseCreationStep{ 0 };
    std::atomic<double> ellipseCreationCenterXCU{ 0.0 };
    std::atomic<double> ellipseCreationCenterYCU{ 0.0 };
    std::atomic<double> ellipseCreationRadiusXCU{ 0.0 };

    std::atomic<bool> arcCreationMode{ false };
    std::atomic<uint32_t> arcCreationStep{ 0 };
    std::atomic<double> arcCreationCenterXCU{ 0.0 };
    std::atomic<double> arcCreationCenterYCU{ 0.0 };
    std::atomic<double> arcCreationStartXCU{ 0.0 };
    std::atomic<double> arcCreationStartYCU{ 0.0 };

    std::atomic<bool> textCreationMode{ false };
    std::atomic<bool> textCreationHasAnchor{ false };
    std::atomic<double> textCreationXCU{ 0.0 };
    std::atomic<double> textCreationYCU{ 0.0 };
    uint64_t textCreationObjectId = 0;
    std::string textCreationDraft;

    // Asset-insert mode: armed by Commands::INSERT_ASSET2D; each Page2D click places an instance.
    // The selected definition id is written by the UI thread (Insert Asset pane dropdown) and read
    // by the engineering thread on click; 0 / stale falls back to the first definition.
    std::atomic<bool> assetInsertMode{ false };
    std::atomic<uint64_t> assetInsertSelectedDefinitionId{ 0 };

    // Selection transform mode (Cad2DTransformKind). The kind atomic is also read by the render
    // thread to trail the EDIT_* command icon next to the cursor.
    std::atomic<uint32_t> transform2DKind{ 0 };
    std::atomic<uint32_t> transform2DStep{ 0 };
    std::atomic<double> transform2DP1XCU{ 0.0 };
    std::atomic<double> transform2DP1YCU{ 0.0 };
    std::atomic<double> transform2DP2XCU{ 0.0 };
    std::atomic<double> transform2DP2YCU{ 0.0 };
};

// Records the append in recordIndex. Call it IMMEDIATELY after a push_back on one of the seven
// record vectors, holding cpuRecordsMutex: it stores the element's position, so it has to read the
// vector that just grew.
template <class Record>
inline void Cad2DIndexAppendedRecord(TabCad2DStorage& storage, const std::vector<Record>& records) {
    storage.recordIndex[records.back().objectId] =
        Cad2DRecordLocation{ Cad2DKindOf(records.back()), static_cast<uint32_t>(records.size() - 1) };
}

void InitCad2DTabResources(TabCad2DStorage& storage);
void CleanupCad2DTabResources(TabCad2DStorage& storage);
// Takes the pan/zoom as a parameter, mirroring how the Scene3D renderer takes a camera: the two
// renderers receive a container, a view state and a viewport, and never reach for view state
// themselves (graphics.md, "Key boundary rule" + 10M plan Step 6).
void RenderPage2D(ID3D12GraphicsCommandList* commandList, DX12ResourcesPerWindow& winRes,
    TabCad2DStorage& storage, DX12ResourcesUI& uiResources, int monitorId,
    uint64_t activeContainerMemoryId, const Cad2DViewState& view);
// 2D half of the copy thread. Staging goes through the global upload ring and the COPY-type
// allocator/list are owned by GpuCopyThread and passed in, exactly as ProcessScene3DCopyBatch takes
// them (graphics.md, 10M plan Step 0). Contract: the list is CLOSED on entry and CLOSED on return.
void ProcessCad2DCopyBatch(const std::vector<CommandToCopyThread2D>& batch,
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>& commandAllocator,
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>& commandList);
void PruneCad2DRetiredResources(TabCad2DStorage& storage, uint64_t safeRetireFence);
void ReleaseCad2DRetiredResources(TabCad2DStorage& storage);
