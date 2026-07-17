// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
//
// All extension-related host code lives in this single translation unit:
// worker process lifecycle, IPC wire protocol, message validation, and the
// sole dispatch path into the Engineering thread. No other file talks to
// extension workers. Design: website/content/software/extensions.md
//
// MVP status: the worker runs as a plain python.exe process (no AppContainer
// sandbox yet); AppContainer + Job Object hardening is the next step.

#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct DATASETTAB;

namespace ExtensionCommunications {

struct ImportedNode {
    uint32_t id = 0;
    float x = 0, y = 0, z = 0; // SI meters, model global axes.
};

struct ImportedMember {
    uint32_t id = 0;
    uint32_t startNodeId = 0;
    uint32_t endNodeId = 0;
    // Vishwakarma catalog designation, already mapped from the STAAD name by the
    // worker (profile_mapping.py). Empty = unmapped; drawn as a placeholder pipe.
    std::string profileDesignation;
};

struct ImportedStructuralModel {
    std::wstring sourceFile;
    std::vector<ImportedNode> nodes;
    std::vector<ImportedMember> members;
};

// 2D elements produced by the DXF importer, destined for the currently open
// Page2D container. Coordinates/sizes are Page2D ComputerUnits.
struct ImportedPage2DLine {
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

struct ImportedPage2DText {
    double x = 0, y = 0;
    float heightCU = 3.5f;
    float rotationRadians = 0.0f;
    uint32_t justification = 4; // Cad2DTextJustification numeric value (0..8).
    std::string textUtf8;
};

struct ImportedPage2DPolygon {
    double centerX = 0, centerY = 0, radius = 0;
    uint32_t segmentCount = 4;
    double rotationDegrees = 0.0;
};

// A 2D asset (DXF "block") definition and its hidden master geometry, in the block's own frame.
struct ImportedAsset2DDefinition {
    uint32_t key = 0;      // Worker-local id referenced by ImportedAsset2DInsert::key.
    double baseX = 0, baseY = 0;
    std::vector<ImportedPage2DLine> lines;
    std::vector<ImportedPage2DText> texts;
    std::vector<ImportedPage2DPolygon> polygons;
};

struct ImportedAsset2DInsert {
    uint32_t key = 0;      // References an ImportedAsset2DDefinition::key.
    double x = 0, y = 0;
    // Per-instance transform baked into the members at materialization; negative scale = mirror.
    double scaleX = 1.0, scaleY = 1.0;
    double rotationDegrees = 0.0; // Counter-clockwise.
};

struct ImportedPage2DContent {
    std::wstring sourceFile;
    std::vector<ImportedPage2DLine> lines;
    std::vector<ImportedPage2DText> texts;
    std::vector<ImportedPage2DPolygon> polygons;
    std::vector<ImportedAsset2DDefinition> assetDefinitions;
    std::vector<ImportedAsset2DInsert> assetInserts;
};

// Main thread (UI dispatch): shows the .std open dialog and queues an
// ACTION_TYPE::IMPORT_STD_FILE action on the tab. Returns false when the
// user cancelled or no tab is active.
bool QueueImportStdCommand(DATASETTAB* tab);

// Queues an import of a known file path (no dialog). Used by the UI command
// above and by the VISHWAKARMA_AUTO_IMPORT_STD dev/testing hook.
bool QueueImportStdFile(DATASETTAB* tab, const std::wstring& stdFilePath);

// Same pair for the DXF importer. The command variant refuses (with a message
// box) when no Page2D internal sub-tab is currently open, per import policy.
bool QueueImportDxfCommand(DATASETTAB* tab);
bool QueueImportDxfFile(DATASETTAB* tab, const std::wstring& dxfFilePath);

// Releases a queued import path payload without running the worker (used when
// the engineering thread aborts an import, e.g. no Page2D open anymore).
void ReleaseQueuedImportPath(uint64_t payloadId);

// Engineering thread: runs the out-of-process import worker for a queued
// request. payloadId is the ACTION_DETAILS::objectId of that action (owns
// the path payload; always released here). Returns nullptr and fills error
// on failure. Caller owns the returned model (delete when done).
ImportedStructuralModel* RunQueuedStdImport(uint64_t payloadId, std::string& error);

// Engineering thread: DXF variant; returns validated Page2D content.
ImportedPage2DContent* RunQueuedDxfImport(uint64_t payloadId, std::string& error);

} // namespace ExtensionCommunications
