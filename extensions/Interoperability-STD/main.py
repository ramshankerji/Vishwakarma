# Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
""".std importer — the first Vishwakarma extension.

The host streams the raw .std file bytes over IPC; this worker parses them
with the pure-Python .std reader (imported as a plain sibling module — live
Python objects, no serialization inside the worker) and streams structural
nodes/members back as CreateGeometryBatch messages.
"""

from __future__ import annotations

import sys
import traceback

import vishwakarma_api as vk
import InteroperabilityWithSTDFile as std_reader
import profile_mapping

BATCH_SIZE = 5000
UINT32_MAX = 0xFFFFFFFF


def _valid_id(value) -> bool:
    return isinstance(value, int) and 0 < value <= UINT32_MAX


def _std_si_to_vishwakarma_node(node_id, x_m, y_m, z_m):
    # Both sides are SI meters (ExtensionIPC.proto); only the axes change:
    # STAAD Y-up -> Vishwakarma Z-up.
    return (node_id, float(x_m), -float(z_m), float(y_m))


def run() -> None:
    channel = vk.HostChannel()
    request = channel.recv_import_request()

    try:
        model = std_reader.read_std_bytes(request.file_bytes, request.file_name)
    except Exception as exc:  # Parser failure must reach the host, not crash silently.
        channel.send_result(False, f"Failed to parse '{request.file_name}': {exc}")
        return

    # Prefer SI-normalized coordinates (meters); raw values only when the file
    # declared no unit context at all.
    coordinates = model.nodes_si if model.nodes_si else model.nodes

    nodes = [
        _std_si_to_vishwakarma_node(node_id, x, y, z)
        for node_id, (x, y, z) in coordinates.items()
        if _valid_id(node_id)
    ]
    known_node_ids = {node[0] for node in nodes}
    members = [
        (member_id, start_id, end_id,
         profile_mapping.designation_for(model.member_profile_by_member.get(member_id)))
        for member_id, (start_id, end_id) in model.members.items()
        if _valid_id(member_id) and start_id in known_node_ids and end_id in known_node_ids
    ]

    skipped_members = len(model.members) - len(members)
    mapped_members = sum(1 for member in members if member[3])
    channel.send_log(
        f"Parsed '{request.file_name}': {len(nodes)} nodes, {len(members)} members"
        f" ({mapped_members} with profile names)"
        + (f" ({skipped_members} members skipped: missing/invalid node refs)" if skipped_members else "")
    )

    for start in range(0, len(nodes), BATCH_SIZE):
        channel.send_geometry_batch(nodes[start:start + BATCH_SIZE], [])
    for start in range(0, len(members), BATCH_SIZE):
        channel.send_geometry_batch([], members[start:start + BATCH_SIZE])

    channel.send_result(True, "", len(nodes), len(members))


if __name__ == "__main__":
    try:
        run()
    except Exception:
        traceback.print_exc(file=sys.stderr)
        sys.exit(1)
