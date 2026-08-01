// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

/* GPU draw-command compaction (website/software/graphics.md, 10M plan Step 7 - vertical slice).

WHAT THIS COMPACTS - COMMANDS, NOT GEOMETRY. One thread runs per persistent draw TEMPLATE of a page
(a template is one 24-byte IndirectCommand in the page's indirectBuffer). This shader reads those
templates and copies only the survivors into a dense output buffer. It never reads, copies or moves
a single vertex or index byte; the geometry pages stay exactly where they are. Do not confuse this
with PAGE compaction (defragmentation, copy thread), which is the unrelated thing that allocates a
fresh 4 MB page and copies live vertex/index ranges into it to reclaim holes.

NO TEMPORARY ALLOCATION. The output (VisibleOut) and its count (VisibleCount) are a PERSISTENT
per-monitor scratch buffer created once when the render thread starts and reused for every page of
every frame - nothing is allocated here or per frame. The CPU resets VisibleCount to 0 (a 4-byte
CopyBufferRegion from a zero buffer) before each dispatch, so the InterlockedAdd slots below start
at 0 and pack tight; the render thread then issues one ExecuteIndirect per page using VisibleCount
as the draw count. Reusing the same scratch for the next page is why the caller barriers it back to
UNORDERED_ACCESS between pages (that drains the previous page's ExecuteIndirect first).

THE 64-BIT VISIBILITY FLAG IS PROCESSED HERE, and that test IS the compaction filter. A template
survives only if IsVisibleInSubTab() finds this object's VisibilityMask bit for the drawn sub-tab
set; hidden and cross-sub-tab-filtered objects are DROPPED before they ever become draw commands,
instead of being vertex-shaded and collapsed to a degenerate primitive as on the legacy path (the
interim Step 5 cost). The mask is authored by the copy thread (WriteVisibilityMask); this shader
only reads it. A coarser container-level sub-tab filter (Step 6) already ran on the CPU to pick which
pages a Viewport visits at all - this is the finer, per-object level within those pages.

Root descriptors only - no shader-visible descriptor heap - because the per-page draw structure is
retained in this slice: templates SRV, mask SRV, output UAV, and a raw count buffer.

IndirectCommand mirrors the 24-byte CPU struct in RenderScene3D.h (and the D3D12 command signature
{Root-Constant b1, DRAW_INDEXED}); keep the three in lockstep. IsVisibleInSubTab mirrors the two
scene vertex shaders - see ShaderSceneVertex.hlsl for the bit convention. */

struct IndirectCommand {
    uint gpuInstanceIndex;        // Root Constant b1, written per command by the command signature.
    uint IndexCountPerInstance;
    uint InstanceCount;
    uint StartIndexLocation;      // Absolute (page-base-relative) since Step 7 constraint 1.
    int  BaseVertexLocation;
    uint StartInstanceLocation;
};

StructuredBuffer<IndirectCommand>   Templates    : register(t0); // = the page's indirectBuffer.
StructuredBuffer<uint2>             VisibilityMask : register(t1); // Per gpuInstanceIndex, uint2.
RWStructuredBuffer<IndirectCommand> VisibleOut   : register(u0); // Compacted output for this page.
RWByteAddressBuffer                 VisibleCount : register(u1); // Command count at byte 0.

cbuffer CullParams : register(b0) {
    uint templateCount; // Number of templates in this page (== page.indirectCount).
    uint subTabBit;     // Which mask bit to test; >= 64 shows everything (kNoSubTabBit path).
};

bool IsVisibleInSubTab(uint instanceIndex, uint bit) {
    if (bit >= 64u) return true;
    uint2 mask = VisibilityMask[instanceIndex];
    uint word = bit < 32u ? mask.x : mask.y;
    return (word & (1u << (bit & 31u))) != 0u;
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    if (i >= templateCount) return;

    IndirectCommand cmd = Templates[i];
    if (!IsVisibleInSubTab(cmd.gpuInstanceIndex, subTabBit)) return;

    // Append. The count is reset to 0 on the CPU (CopyBufferRegion from a zero buffer) before this
    // dispatch, so the slot returned is a compact 0-based index into VisibleOut.
    uint slot;
    VisibleCount.InterlockedAdd(0, 1, slot);
    VisibleOut[slot] = cmd;
}
