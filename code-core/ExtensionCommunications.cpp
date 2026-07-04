// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.
//
// Host-side extension IPC command handler. See ExtensionCommunications.h and
// website/content/software/extensions.md for the full design. Every message
// arriving from a worker is treated as hostile input and validated before it
// touches the model.

#include "preCompiledHeadersWindows.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "विश्वकर्मा.h"
#include "ExtensionCommunications.h"
#include "ExtensionIPC.pb.h"

#include <windows.h>
#include <commdlg.h>

namespace ExtensionCommunications {

namespace {

// Validation limits: a worker violating any of these is terminated.
constexpr uint64_t  kMaxStdFileBytes   = 512ull * 1024 * 1024; // Host refuses larger input files.
constexpr uint32_t  kMaxMessageBytes   = 64u * 1024 * 1024;    // Per worker->host message.
constexpr size_t    kMaxImportedNodes  = 2'000'000;
constexpr size_t    kMaxImportedMembers= 4'000'000;
constexpr ULONGLONG kImportTimeoutMs   = 180'000;              // Whole import, wall clock.

HWND FirstWindowHandleForDialogs() {
    uint16_t* windowList = publishedWindowIndexes.load(std::memory_order_acquire);
    uint16_t windowCount = publishedWindowCount.load(std::memory_order_acquire);
    return windowCount > 0 ? allWindows[windowList[0]].hWnd : nullptr;
}

std::wstring ShowOpenStdDialog() {
    wchar_t fileName[MAX_PATH] = {};

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = FirstWindowHandleForDialogs();
    ofn.lpstrFilter = L"STAAD input files (*.std)\0*.std\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"std";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) return {};
    return fileName;
}

std::string Utf8FromWide(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), (int)wide.size(), utf8.data(), size, nullptr, nullptr);
    return utf8;
}

std::wstring ExecutableDirectory() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path().wstring();
}

std::wstring StdImportTempPath(const wchar_t* leaf) {
    wchar_t tempDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir);
    return std::wstring(tempDir) + leaf;
}

std::wstring StdImportStderrLogPath() {
    return StdImportTempPath(L"vishwakarma_std_import_stderr.log");
}

// Writes a one-line outcome marker so an import can be verified independently
// of the app's console (which is redirected to its own window via AllocConsole).
void WriteImportResultMarker(const std::string& line) {
    std::ofstream marker(std::filesystem::path(StdImportTempPath(L"vishwakarma_std_import_result.log")),
        std::ios::binary | std::ios::trunc);
    if (marker) marker << line << "\n";
}

std::string TailOfStderrLog(const std::wstring& logPath) {
    std::ifstream log(std::filesystem::path(logPath), std::ios::binary);
    if (!log) return {};
    log.seekg(0, std::ios::end);
    const std::streamoff size = log.tellg();
    const std::streamoff tail = (std::min<std::streamoff>)(size, 2000);
    log.seekg(size - tail);
    std::string text(static_cast<size_t>(tail), '\0');
    log.read(text.data(), tail);
    return text;
}

struct WorkerProcess {
    PROCESS_INFORMATION process{};
    HANDLE stdinWrite = nullptr;  // Host writes the request here.
    HANDLE stdoutRead = nullptr;  // Host reads worker messages here.

    ~WorkerProcess() {
        if (stdinWrite) CloseHandle(stdinWrite);
        if (stdoutRead) CloseHandle(stdoutRead);
        if (process.hProcess) {
            // If the worker is still running at teardown, something went wrong: kill it.
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
                TerminateProcess(process.hProcess, 1);
            }
            CloseHandle(process.hProcess);
        }
        if (process.hThread) CloseHandle(process.hThread);
    }
};

bool SpawnImportWorker(WorkerProcess& worker, std::string& error) {
    const std::wstring extensionDir = ExecutableDirectory() + L"\\extensions\\std-importer";
    if (!std::filesystem::exists(std::filesystem::path(extensionDir) / L"main.py")) {
        error = "Extension not found: " + Utf8FromWide(extensionDir) +
                "\\main.py (was the post-build extension deploy step run?)";
        return false;
    }

    SECURITY_ATTRIBUTES inheritable{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };

    HANDLE stdinRead = nullptr, stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr, stdoutWrite = nullptr;
    if (!CreatePipe(&stdinRead, &stdinWrite, &inheritable, 16 * 1024 * 1024) ||
        !CreatePipe(&stdoutRead, &stdoutWrite, &inheritable, 16 * 1024 * 1024)) {
        error = "CreatePipe failed";
        if (stdinRead) CloseHandle(stdinRead);
        if (stdinWrite) CloseHandle(stdinWrite);
        return false;
    }
    // Only the child-side ends may be inherited by the worker.
    SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);

    // Worker stderr goes to a log file so parse tracebacks are diagnosable.
    HANDLE stderrLog = CreateFileW(StdImportStderrLogPath().c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        &inheritable, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdinRead;
    startup.hStdOutput = stdoutWrite;
    startup.hStdError = (stderrLog != INVALID_HANDLE_VALUE) ? stderrLog : GetStdHandle(STD_ERROR_HANDLE);

    // MVP: plain python.exe from PATH, no sandbox yet. Becomes vk_worker.exe
    // (frozen CPython inside an AppContainer) in the follow-up hardening step.
    std::wstring commandLine = L"python -u main.py";
    const BOOL created = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr,
        TRUE /*inherit handles*/, CREATE_NO_WINDOW, nullptr, extensionDir.c_str(),
        &startup, &worker.process);

    // Child-side handles are duplicated into the worker; release the host copies.
    CloseHandle(stdinRead);
    CloseHandle(stdoutWrite);
    if (stderrLog != INVALID_HANDLE_VALUE) CloseHandle(stderrLog);

    if (!created) {
        error = "Failed to launch the Python worker (is Python 3.12+ on PATH?). Win32 error " +
                std::to_string(GetLastError());
        CloseHandle(stdinWrite);
        CloseHandle(stdoutRead);
        return false;
    }

    worker.stdinWrite = stdinWrite;
    worker.stdoutRead = stdoutRead;
    return true;
}

bool WriteAll(HANDLE pipe, const void* data, size_t size, std::string& error) {
    const uint8_t* cursor = static_cast<const uint8_t*>(data);
    size_t remaining = size;
    while (remaining > 0) {
        DWORD written = 0;
        const DWORD chunk = (DWORD)(std::min<size_t>)(remaining, 4 * 1024 * 1024);
        if (!WriteFile(pipe, cursor, chunk, &written, nullptr) || written == 0) {
            error = "Worker closed the pipe while receiving the file (see stderr log)";
            return false;
        }
        cursor += written;
        remaining -= written;
    }
    return true;
}

// Reads exactly `size` bytes, polling so the wall-clock deadline and worker
// liveness are enforced (anonymous pipes have no native read timeout).
bool ReadExact(const WorkerProcess& worker, uint8_t* buffer, uint32_t size,
    ULONGLONG deadlineTick, std::string& error) {
    uint32_t received = 0;
    while (received < size) {
        DWORD available = 0;
        if (!PeekNamedPipe(worker.stdoutRead, nullptr, 0, nullptr, &available, nullptr)) {
            error = "Worker pipe closed unexpectedly";
            return false;
        }
        if (available == 0) {
            if (GetTickCount64() > deadlineTick) {
                error = "Import timed out";
                return false;
            }
            if (WaitForSingleObject(worker.process.hProcess, 0) != WAIT_TIMEOUT) {
                // Process exited; drain whatever is left before giving up.
                if (!PeekNamedPipe(worker.stdoutRead, nullptr, 0, nullptr, &available, nullptr) || available == 0) {
                    error = "Worker exited before completing the import (see stderr log)";
                    return false;
                }
            } else {
                Sleep(5);
                continue;
            }
        }
        DWORD toRead = (std::min<DWORD>)(available, size - received);
        DWORD readBytes = 0;
        if (!ReadFile(worker.stdoutRead, buffer + received, toRead, &readBytes, nullptr) || readBytes == 0) {
            error = "ReadFile on worker pipe failed";
            return false;
        }
        received += readBytes;
    }
    return true;
}

bool SendImportRequest(const WorkerProcess& worker, const std::wstring& stdFilePath, std::string& error) {
    std::ifstream file(std::filesystem::path(stdFilePath), std::ios::binary | std::ios::ate);
    if (!file) {
        error = "Could not open file: " + Utf8FromWide(stdFilePath);
        return false;
    }
    const std::streamoff fileSize = file.tellg();
    if (fileSize < 0 || (uint64_t)fileSize > kMaxStdFileBytes) {
        error = "File exceeds the " + std::to_string(kMaxStdFileBytes >> 20) + " MiB import limit";
        return false;
    }
    std::string fileBytes(static_cast<size_t>(fileSize), '\0');
    file.seekg(0);
    file.read(fileBytes.data(), fileSize);

    vishwakarma::extension::v1::HostToWorker request;
    auto* import = request.mutable_import_file_request();
    import->set_file_name(Utf8FromWide(std::filesystem::path(stdFilePath).filename().wstring()));
    import->set_file_bytes(std::move(fileBytes));

    std::string payload;
    if (!request.SerializeToString(&payload)) {
        error = "Failed to serialize the import request";
        return false;
    }
    const uint32_t length = (uint32_t)payload.size();
    uint8_t prefix[4] = { (uint8_t)(length), (uint8_t)(length >> 8), (uint8_t)(length >> 16), (uint8_t)(length >> 24) };
    return WriteAll(worker.stdinWrite, prefix, sizeof(prefix), error) &&
           WriteAll(worker.stdinWrite, payload.data(), payload.size(), error);
}

bool AppendValidatedBatch(const vishwakarma::extension::v1::CreateGeometryBatch& batch,
    ImportedStructuralModel& model, std::unordered_set<uint32_t>& seenNodeIds, std::string& error) {
    if (model.nodes.size() + batch.nodes_size() > kMaxImportedNodes) {
        error = "Worker exceeded the node cap";
        return false;
    }
    if (model.members.size() + batch.members_size() > kMaxImportedMembers) {
        error = "Worker exceeded the member cap";
        return false;
    }
    for (const auto& node : batch.nodes()) {
        if (node.node_id() == 0 ||
            !std::isfinite(node.x()) || !std::isfinite(node.y()) || !std::isfinite(node.z())) {
            error = "Worker sent an invalid node (zero id or non-finite coordinate)";
            return false;
        }
        if (!seenNodeIds.insert(node.node_id()).second) continue; // Ignore duplicate ids.
        model.nodes.push_back({ node.node_id(), (float)node.x(), (float)node.y(), (float)node.z() });
    }
    for (const auto& member : batch.members()) {
        if (member.member_id() == 0 || member.start_node_id() == 0 || member.end_node_id() == 0) {
            error = "Worker sent an invalid member (zero id)";
            return false;
        }
        model.members.push_back({ member.member_id(), member.start_node_id(), member.end_node_id() });
    }
    return true;
}

} // anonymous namespace

bool QueueImportStdCommand(DATASETTAB* tab) {
    if (!tab || !tab->todoCPUQueue) return false;

    const std::wstring path = ShowOpenStdDialog();
    if (path.empty()) return false;
    return QueueImportStdFile(tab, path);
}

bool QueueImportStdFile(DATASETTAB* tab, const std::wstring& stdFilePath) {
    if (!tab || !tab->todoCPUQueue || stdFilePath.empty()) return false;

    ACTION_DETAILS request{};
    request.actionType = ACTION_TYPE::IMPORT_STD_FILE;
    request.source = INPUT_SOURCE::SYSTEM;
    request.objectId = reinterpret_cast<uint64_t>(new std::wstring(stdFilePath));
    request.timestamp = GetTickCount64();
    tab->todoCPUQueue->push(request);
    return true;
}

ImportedStructuralModel* RunQueuedStdImport(uint64_t payloadId, std::string& error) {
    std::unique_ptr<std::wstring> path(reinterpret_cast<std::wstring*>(payloadId));
    if (!path || path->empty()) {
        error = "Import request carried no file path";
        return nullptr;
    }

    WorkerProcess worker;
    if (!SpawnImportWorker(worker, error)) return nullptr;
    if (!SendImportRequest(worker, *path, error)) return nullptr;

    auto model = std::make_unique<ImportedStructuralModel>();
    model->sourceFile = *path;
    std::unordered_set<uint32_t> seenNodeIds;
    const ULONGLONG deadlineTick = GetTickCount64() + kImportTimeoutMs;

    bool resultReceived = false;
    std::vector<uint8_t> payload;
    while (!resultReceived) {
        uint8_t prefix[4] = {};
        if (!ReadExact(worker, prefix, sizeof(prefix), deadlineTick, error)) break;
        const uint32_t length = (uint32_t)prefix[0] | ((uint32_t)prefix[1] << 8) |
                                ((uint32_t)prefix[2] << 16) | ((uint32_t)prefix[3] << 24);
        if (length == 0 || length > kMaxMessageBytes) {
            error = "Worker sent an out-of-range message length";
            break;
        }
        payload.resize(length);
        if (!ReadExact(worker, payload.data(), length, deadlineTick, error)) break;

        vishwakarma::extension::v1::WorkerToHost message;
        if (!message.ParseFromArray(payload.data(), (int)length)) {
            error = "Worker sent an unparseable message";
            break;
        }

        switch (message.msg_case()) {
        case vishwakarma::extension::v1::WorkerToHost::kCreateGeometryBatch:
            if (!AppendValidatedBatch(message.create_geometry_batch(), *model, seenNodeIds, error)) {
                resultReceived = false;
            } else {
                continue;
            }
            break;
        case vishwakarma::extension::v1::WorkerToHost::kLog:
            std::cout << "[std-importer] "
                      << message.log().text().substr(0, 2000) << "\n";
            continue;
        case vishwakarma::extension::v1::WorkerToHost::kResult:
            if (!message.result().success()) {
                error = "Import failed: " + message.result().error().substr(0, 2000);
            } else {
                resultReceived = true;
            }
            break;
        default:
            error = "Worker sent an unknown message type";
            break;
        }
        if (!resultReceived) break; // Any validation failure or worker-reported error.
    }

    if (!resultReceived) {
        const std::string stderrTail = TailOfStderrLog(StdImportStderrLogPath());
        if (!stderrTail.empty()) error += "\n\nWorker stderr:\n" + stderrTail;
        WriteImportResultMarker("FAILED: " + error.substr(0, 500));
        return nullptr; // WorkerProcess destructor terminates the worker.
    }

    // The sandbox boundary ends at this validation: members must reference
    // nodes that actually arrived, no matter what the worker claimed.
    std::vector<ImportedMember> validMembers;
    validMembers.reserve(model->members.size());
    for (const ImportedMember& member : model->members) {
        if (seenNodeIds.count(member.startNodeId) && seenNodeIds.count(member.endNodeId)) {
            validMembers.push_back(member);
        }
    }
    model->members = std::move(validMembers);

    WriteImportResultMarker("OK: " + std::to_string(model->nodes.size()) + " nodes, " +
                            std::to_string(model->members.size()) + " members from " + Utf8FromWide(*path));
    std::cout << "[std-importer] Validated " << model->nodes.size() << " nodes and "
              << model->members.size() << " members from " << Utf8FromWide(*path) << std::endl;
    return model.release();
}

} // namespace ExtensionCommunications
