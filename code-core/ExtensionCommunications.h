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
};

struct ImportedStructuralModel {
    std::wstring sourceFile;
    std::vector<ImportedNode> nodes;
    std::vector<ImportedMember> members;
};

// Main thread (UI dispatch): shows the .std open dialog and queues an
// ACTION_TYPE::IMPORT_STD_FILE action on the tab. Returns false when the
// user cancelled or no tab is active.
bool QueueImportStdCommand(DATASETTAB* tab);

// Queues an import of a known file path (no dialog). Used by the UI command
// above and by the VISHWAKARMA_AUTO_IMPORT_STD dev/testing hook.
bool QueueImportStdFile(DATASETTAB* tab, const std::wstring& stdFilePath);

// Engineering thread: runs the out-of-process import worker for a queued
// request. payloadId is the ACTION_DETAILS::objectId of that action (owns
// the path payload; always released here). Returns nullptr and fills error
// on failure. Caller owns the returned model (delete when done).
ImportedStructuralModel* RunQueuedStdImport(uint64_t payloadId, std::string& error);

} // namespace ExtensionCommunications
