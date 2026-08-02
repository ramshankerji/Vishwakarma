// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

/* GPU draw-command compaction (website/software/graphics.md, 10M plan Step 7).

WHAT THIS COMPACTS - COMMANDS, NOT GEOMETRY. One thread runs per persistent draw TEMPLATE of a page
(a template is one 24-byte IndirectCommand in the page's indirectBuffer). This shader reads those
templates and copies only the survivors into a dense output buffer. It never reads, copies or moves
a single vertex or index byte; the geometry pages stay exactly where they are. Do not confuse this
with PAGE compaction (defragmentation, copy thread), which is the unrelated thing that allocates a
fresh 4 MB page and copies live vertex/index ranges into it to reclaim holes.

ONE EXECUTEINDIRECT PER VIEWPORT. Every page of the Viewport dispatches into the SAME output buffer
and the SAME count, so the render thread issues exactly one draw call for the whole view rather than
one per page. That is why each command carries its page's vertex and index buffer VIEWS (the CPU
passes them in as root constants below, since the dispatch is already per page): a command that names
its own buffers can sit next to a command from another page. The per-page IASetVertexBuffers /
IASetIndexBuffer pair is gone from the draw loop entirely.

The dispatches deliberately have NO barriers between them. They only ever touch the count through
InterlockedAdd, so they are order-independent and may overlap; the resulting order of commands within
the buffer varies, which does not matter for depth-tested opaque draws. A single barrier after the
last dispatch hands the buffer to the draw.

NO TEMPORARY ALLOCATION. VisibleOut and VisibleCount are a PERSISTENT per-monitor scratch created
once when the render thread starts and reused every frame - nothing is allocated here or per frame.
The CPU resets VisibleCount to 0 once per Viewport (a 4-byte CopyBufferRegion from a zero buffer)
before the first dispatch, so the InterlockedAdd slots start at 0 and pack tight.

THE 64-BIT VISIBILITY FLAG IS PROCESSED HERE, and that test IS the compaction filter. A template
survives only if IsVisibleInSubTab() finds this object's VisibilityMask bit for the drawn sub-tab
set; hidden and cross-sub-tab-filtered objects are DROPPED before they ever become draw commands,
instead of being vertex-shaded and collapsed to a degenerate primitive as on the legacy path (the
interim Step 5 cost). The mask is authored by the copy thread (WriteVisibilityMask); this shader
only reads it. A coarser container-level sub-tab filter (Step 6) already ran on the CPU to pick which
pages a Viewport visits at all - this is the finer, per-object level within those pages.

Root descriptors only - no shader-visible descriptor heap - because the buffer views travel in the
constants rather than in a page directory the shader would have to read: templates SRV, mask SRV,
output UAV, and a raw count buffer.

IndirectCommand mirrors the 24-byte CPU struct in RenderScene3D.h; VisibleCommand mirrors the
56-byte VisibleIndirectCommand there AND the D3D12 command signature built in InitD3DPerTab
{VBV, IBV, Root-Constant b1, DRAW_INDEXED} - keep all three in lockstep. IsVisibleInSubTab mirrors
the two scene vertex shaders - see ShaderSceneVertex.hlsl for the bit convention. */

struct IndirectCommand {
    uint gpuInstanceIndex;        // Root Constant b1, written per command by the command signature.
    uint IndexCountPerInstance;
    uint InstanceCount;
    uint StartIndexLocation;      // Absolute (page-base-relative) since Step 7 constraint 1.
    int  BaseVertexLocation;
    uint StartInstanceLocation;
};

/* 56 bytes as fourteen tightly packed scalar uints. Written out member by member rather than with
uint2 / uint4 on purpose: a structured-buffer element of plain uints has no alignment padding to
reason about, so the stride is unambiguously 56 and matches the command signature's ByteStride.
The two 64-bit GPU addresses are carried as explicit lo/hi pairs, which is also their exact memory
layout, so no 64-bit integer shader support is required. */
struct VisibleCommand {
    uint vertexAddressLo;   // D3D12_VERTEX_BUFFER_VIEW
    uint vertexAddressHi;
    uint vertexSizeInBytes;
    uint vertexStrideInBytes;
    uint indexAddressLo;    // D3D12_INDEX_BUFFER_VIEW
    uint indexAddressHi;
    uint indexSizeInBytes;
    uint indexFormat;
    uint gpuInstanceIndex;  // Root Constant b1
    uint IndexCountPerInstance; // D3D12_DRAW_INDEXED_ARGUMENTS
    uint InstanceCount;
    uint StartIndexLocation;
    int  BaseVertexLocation;
    uint StartInstanceLocation;
};

StructuredBuffer<IndirectCommand>   Templates      : register(t0); // = the page's indirectBuffer.
StructuredBuffer<uint2>             VisibilityMask : register(t1); // Per gpuInstanceIndex, uint2.
RWStructuredBuffer<VisibleCommand>  VisibleOut     : register(u0); // Compacted output, whole Viewport.
RWByteAddressBuffer                 VisibleCount   : register(u1); // Command count at byte 0.

/* Twelve scalar uints. Every member is a scalar, so none can straddle a float4 register and the
cbuffer is exactly 12 DWORDs - which is what the root signature declares and what the render thread
pushes with SetComputeRoot32BitConstants. Only the first three change meaning per Viewport; the
eight view fields change per page, which is the entire reason the dispatch stays per page. */
cbuffer CullParams : register(b0) {
    uint templateCount;      // Number of templates in THIS page (== page.indirectCount).
    uint subTabBit;          // Which mask bit to test. >= 64 shows everything - reachable only by a
                             // draw with no sub-tab (print path), since MV_MAX_SUBTABS == 64.
    uint maxCommands;        // Capacity of VisibleOut, in commands. Overflow is dropped, not wrapped.
    uint vertexAddressLo;    // This page's vertex buffer view (page base, vertexHead bytes, stride).
    uint vertexAddressHi;
    uint vertexSizeInBytes;
    uint vertexStrideInBytes;
    uint indexAddressLo;     // This page's index buffer view (page base over the WHOLE page).
    uint indexAddressHi;
    uint indexSizeInBytes;
    uint indexFormat;
    uint cullPadding;        // Pads the cbuffer to a whole register. Unused.
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

    // Append. The count is reset to 0 on the CPU (CopyBufferRegion from a zero buffer) once per
    // Viewport, before the first page's dispatch, so the slot returned is a compact 0-based index
    // into VisibleOut spanning every page of the Viewport.
    uint slot;
    VisibleCount.InterlockedAdd(0, 1, slot);
    // Overflow is DROPPED rather than wrapped: writing past the buffer would corrupt live commands.
    // The count is deliberately left over-counted so the CPU's fence-gated readback can report the
    // overflow; ExecuteIndirect itself draws min(MaxCommandCount, count) and so stays in bounds.
    if (slot >= maxCommands) return;

    VisibleCommand out_;
    out_.vertexAddressLo      = vertexAddressLo;
    out_.vertexAddressHi      = vertexAddressHi;
    out_.vertexSizeInBytes    = vertexSizeInBytes;
    out_.vertexStrideInBytes  = vertexStrideInBytes;
    out_.indexAddressLo       = indexAddressLo;
    out_.indexAddressHi       = indexAddressHi;
    out_.indexSizeInBytes     = indexSizeInBytes;
    out_.indexFormat          = indexFormat;
    out_.gpuInstanceIndex     = cmd.gpuInstanceIndex;
    out_.IndexCountPerInstance = cmd.IndexCountPerInstance;
    out_.InstanceCount        = cmd.InstanceCount;
    out_.StartIndexLocation   = cmd.StartIndexLocation;
    out_.BaseVertexLocation   = cmd.BaseVertexLocation;
    out_.StartInstanceLocation = cmd.StartInstanceLocation;
    VisibleOut[slot] = out_;
}
