# Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
"""vishwakarma_api v1 — stable Python API for Vishwakarma extensions.

Extensions talk to the host only through this module. It encapsulates the
wire format (length-prefixed Protobuf Lite over the process stdin/stdout
pipes) so the underlying IPC channel can change without breaking extensions.

This copy adds the Page2D batch used by 2D importers; the .std importer's
copy predates it. Both remain wire-compatible (same ExtensionIPC schema).
"""

from __future__ import annotations

import struct
import sys

import ExtensionIPC_pb2 as _pb

# The host never sends a message larger than this; a bigger prefix means the
# stream is corrupt, so fail fast instead of trying to allocate it.
MAX_MESSAGE_BYTES = 256 * 1024 * 1024


class HostChannel:
    """Framed protobuf channel to the host over stdin/stdout."""

    def __init__(self):
        self._in = sys.stdin.buffer
        self._out = sys.stdout.buffer

    def _read_exact(self, count: int) -> bytes:
        chunks = []
        remaining = count
        while remaining > 0:
            chunk = self._in.read(remaining)
            if not chunk:
                raise EOFError("host closed the pipe")
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)

    def recv_import_request(self) -> "_pb.ImportFileRequest":
        (length,) = struct.unpack("<I", self._read_exact(4))
        if length > MAX_MESSAGE_BYTES:
            raise ValueError(f"host message of {length} bytes exceeds limit")
        message = _pb.HostToWorker()
        message.ParseFromString(self._read_exact(length))
        if message.WhichOneof("msg") != "import_file_request":
            raise ValueError("unexpected message from host")
        return message.import_file_request

    def _send(self, message: "_pb.WorkerToHost") -> None:
        payload = message.SerializeToString()
        self._out.write(struct.pack("<I", len(payload)))
        self._out.write(payload)
        self._out.flush()

    def send_log(self, text: str) -> None:
        message = _pb.WorkerToHost()
        message.log.text = text
        self._send(message)

    def send_page2d_batch(self, lines=(), texts=(), polygons=()) -> None:
        """lines: iterable of (x1, y1, x2, y2);
        texts: iterable of (x, y, height, rotation_radians, justification, text);
        polygons: iterable of (center_x, center_y, radius, segment_count, rotation_degrees)."""
        message = _pb.WorkerToHost()
        batch = message.create_page2d_batch
        for x1, y1, x2, y2 in lines:
            line = batch.lines.add()
            line.x1 = x1
            line.y1 = y1
            line.x2 = x2
            line.y2 = y2
        for x, y, height, rotation_radians, justification, text in texts:
            record = batch.texts.add()
            record.x = x
            record.y = y
            record.height = height
            record.rotation_radians = rotation_radians
            record.justification = justification
            record.text = text
        for center_x, center_y, radius, segment_count, rotation_degrees in polygons:
            polygon = batch.polygons.add()
            polygon.center_x = center_x
            polygon.center_y = center_y
            polygon.radius = radius
            polygon.segment_count = segment_count
            polygon.rotation_degrees = rotation_degrees
        self._send(message)

    def send_result(self, success: bool, error: str = "",
                    total_nodes: int = 0, total_members: int = 0,
                    total_elements: int = 0) -> None:
        message = _pb.WorkerToHost()
        message.result.success = success
        message.result.error = error
        message.result.total_nodes = total_nodes
        message.result.total_members = total_members
        message.result.total_elements = total_elements
        self._send(message)
