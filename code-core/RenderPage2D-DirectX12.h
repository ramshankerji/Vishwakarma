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

/* Which run of a page's records one object owns (id.md §11.4, step 2d). An object is never split
across pages - the filler opens a new page rather than straddle one - so a single run says all of
it, which is what lets a modify find the records to hide from the objectId alone. */
struct Cad2DPagePlacement {
    uint64_t objectId = 0;
    uint32_t firstRecord = 0;
    uint32_t count = 0;
};

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

    Step 2d added the SECOND such field, inside the records: a record's 4-byte `flags` word, where
    selection and hiding are single aligned stores at a known offset. Everything else about a
    published page is still immutable. */
    /* A text page's index count is DERIVED from this, never stored: glyph quads are emitted 4
    vertices to 6 indices in lockstep, so `count / 4 * 6` is exact. A second atomic would be a
    second source of truth, and a reader that loaded the two on either side of an append could
    size an index view past the end of the vertices it can see. One load, both numbers. */
    std::atomic<uint32_t> count{ 0 };

    /* COPY-THREAD PRIVATE - no render thread reads this. What the page holds, in fill order,
    including the runs a modify has since hidden. Retiring the page needs it to take exactly its
    own objects out of TabCad2DStorage::gpuLocation, and the compaction needs it to re-place the
    survivors. Text pages leave it empty: glyph quads carry no flags word, so a text object is
    never hidden and never registered. */
    std::vector<Cad2DPagePlacement> placements;

    /* COPY-THREAD PRIVATE. Records of the `count` above that no live object owns any more - the
    runs append-plus-hide left behind (id.md §11.4, step 2e). They still cost their slot AND a
    vertex-shader invocation apiece, because InstanceCount cannot skip them; the shader can only
    make them draw nothing. Past kCad2DCompactionHoleShare of the page the copy thread packs them
    out. A placement is a hole exactly when gpuLocation no longer names it, which is why this is
    incremented at the two places that stop naming one rather than derived by a scan. */
    uint32_t holeRecords = 0;
};

/* Compact a page once this fraction of its CAPACITY is holes - 1/4, as id.md §11.3 decision 2's
sizing assumed. Measured against capacity rather than fill so that a nearly-empty page is left
alone: 90 holes among 100 records waste 2.8 KB and 90 shader invocations, which is not worth a
1 MB page copy, while 8,192 holes in a line page are worth exactly that. */
constexpr uint32_t kCad2DCompactionHoleShare = 4;

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
    /* Which of the nine vectors holds the record. It has to be stored rather than read from the
    record, because you cannot reach the record's own dataType until you know which vector to
    index - that circularity is the whole reason this field exists. It is FILLED from dataType,
    in Cad2DIndexAppendedRecord below. */
    VishwakarmaStorage::ObjectType type = VishwakarmaStorage::ObjectType::Unknown;
    uint32_t index = 0;
};

/* Where an object's LIVE GPU records are (id.md §11.4, step 2d). The mirror of InstanceRegistry on
the 3D side, and the same caveat applies: this maps to a GPU location and is explicitly NOT the CPU
object directory §3 wants - `recordIndex` above is the one that finds the engineering record.

An object appears here at most once. A modify erases the entry, hides the old run and re-registers
the object wherever its new records landed, so the entry always names the run that is drawn. An
object with no entry has no drawn records: text (no flags word, never registered), a degenerate
shape that expanded to nothing, or a deleted one. */
struct Cad2DGpuLocation {
    Cad2DPageGPU* page = nullptr;
    uint32_t firstRecord = 0;
    uint32_t count = 0;
};

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

    /* COPY-THREAD PRIVATE, the two of them - written and read only inside ProcessCad2DCopyBatch,
    which is why neither takes a mutex (decision 4). They are what step 2d needs and 2c did not:
    to hide a record you have to find it, and to know which flags to change you have to know which
    ones you last wrote.

    stampedSelection is the selection set as the GPU records currently carry it. Diffing it against
    selectedObjectIds is what turns a selection click into a handful of 4-byte stores instead of a
    container rebuild - and it is why SelectionRefresh no longer has to name a container. */
    std::unordered_map<uint64_t, Cad2DGpuLocation> gpuLocation;
    std::unordered_set<uint64_t> stampedSelection;

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
    /* The type comes from the record ITSELF - META_DATA::dataType, set by every 2D record's
    constructor. This used to call a nine-overload Cad2DKindOf table that hardcoded the same
    mapping a second time; the table went when all nine types gained dataType, because two
    statements of one truth is one too many. */
    storage.recordIndex[records.back().memoryID] = Cad2DRecordLocation{
        static_cast<VishwakarmaStorage::ObjectType>(records.back().dataType),
        static_cast<uint32_t>(records.size() - 1) };
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
