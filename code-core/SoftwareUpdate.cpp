// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

// Release / install / update machinery of Vishwakarma. Design: website/content/software/release.md
// This single file is compiled into two different binaries:
// 1. Vishwakarma.exe (the application). Provides SoftwareUpdateOnAppLaunch() which applies a
//    previously staged update, and StartSoftwareUpdateThread() which periodically downloads a
//    newer signed setup into a staging folder.
// 2. VishwakarmaSetup.exe (the installer, VISHWAKARMA_INSTALLER defined). Has Vishwakarma.exe
//    and the release json embedded as RCDATA resources (IDs 201 / 202 in VishwakarmaSetup.rc)
//    and provides its own wWinMain. No admin privilege needed for the default per-user install.
//
// Phase 1 scope only: signed manifest, SHA-256 verification, full setup exe, manual/automatic
// check. No chunking, no peer-to-peer, no delta updates yet.

#include "SoftwareUpdate.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifndef VISHWAKARMA_INSTALLER
#include <winhttp.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#pragma comment(lib, "Winhttp.lib")
#endif

namespace fs = std::filesystem;

#pragma comment(lib, "Advapi32.lib") // Registry + scheduled-task-adjacent Win32 calls.

// ------------------------------------------------------------------ Constants
static const wchar_t* kCompanyFolder = L"Mission Vishwakarma";
static const wchar_t* kAppExeName = L"Vishwakarma.exe";
static const wchar_t* kReleaseJsonName = L"Vishwakarma_release_details.json";
static const wchar_t* kScheduledTaskName = L"VishwakarmaUpgrade"; // Every-6-days background updater.
static const wchar_t* kManifestUrl =
    L"https://github.com/ramshankerji/Vishwakarma/releases/download/nightly/Vishwakarma_release_details.json";
static const wchar_t* kManifestSigUrl =
    L"https://github.com/ramshankerji/Vishwakarma/releases/download/nightly/Vishwakarma_release_details.json.sig";

// Ed25519 public key matching code-miscellaneous/ManifestSigner-01.key (signingKeyId ManifestSigner-01).
static const char* kManifestPublicKeyPem =
    "-----BEGIN PUBLIC KEY-----\n"
    "MCowBQYDK2VwAyEAn6g084etWA+9uyagwgiI4sc757rwx2tDMI+RwMXWKzo=\n"
    "-----END PUBLIC KEY-----\n";

// Resource IDs inside VishwakarmaSetup.exe. Must match VishwakarmaSetup.rc.
#define IDR_VW_PAYLOAD_EXE 201
#define IDR_VW_RELEASE_JSON 202
#define IDR_VW_EULA_MD 203
#define IDR_VW_PRIVACY_MD 204
#define IDR_VW_EXT_STD_ZIP 205
#define IDR_VW_EXT_DXF_ZIP 206
#define IDR_VW_EXT_WORKER_EXE 207
#define IDR_VW_LICENSES_ZIP 208

// ------------------------------------------------------------------ Small helpers
static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

static fs::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) result = p;
    if (p) CoTaskMemFree(p);
    return result;
}

// %LOCALAPPDATA%\Mission Vishwakarma : updater state + staging area (not the install folder).
static fs::path DataDir() { return KnownFolder(FOLDERID_LocalAppData) / kCompanyFolder; }
static fs::path StagingDir() { return DataDir() / L"Updates"; }
static fs::path StagedMarkerPath() { return StagingDir() / L"staged.json"; }
static fs::path UpdateStatePath() { return DataDir() / L"UpdateState.json"; }

static fs::path CurrentExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

static bool ReadFileBytes(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

static bool WriteFileBytes(const fs::path& p, const void* data, size_t size) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write((const char*)data, (std::streamsize)size);
    return f.good();
}

// ------------------------------------------------------------------ Minimal JSON reading
// The release json is machine generated with a fixed schema, so simple key scanning suffices.
static std::string JsonString(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t k = json.find(pattern);
    if (k == std::string::npos) return "";
    size_t colon = json.find(':', k + pattern.size());
    if (colon == std::string::npos) return "";
    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return json.substr(q1 + 1, q2 - q1 - 1);
}

static long long JsonInt(const std::string& json, const std::string& key, long long fallback = -1) {
    std::string pattern = "\"" + key + "\"";
    size_t k = json.find(pattern);
    if (k == std::string::npos) return fallback;
    size_t colon = json.find(':', k + pattern.size());
    if (colon == std::string::npos) return fallback;
    size_t i = colon + 1;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) i++;
    if (i >= json.size() || (!isdigit((unsigned char)json[i]) && json[i] != '-')) return fallback;
    return _strtoi64(json.c_str() + i, nullptr, 10);
}

// Version of the installation this exe belongs to: read from the json placed next to the exe
// by the installer. 0 means developer / unpackaged build: updater stays fully inactive.
static long long InstalledVersion() {
    std::string json;
    if (!ReadFileBytes(CurrentExeDir() / kReleaseJsonName, json)) return 0;
    long long v = JsonInt(json, "version", 0);
    return v > 0 ? v : 0;
}

// ------------------------------------------------------------------ Updater state persistence
// Spec: persist lastAcceptedManifestSequence, lastInstalledVersion, lastSuccessfulLaunchVersion,
// lastRejectedBadVersion.
struct UpdateState {
    std::string lastAcceptedManifestSequence;
    long long lastInstalledVersion = 0;
    long long lastSuccessfulLaunchVersion = 0;
    long long lastRejectedBadVersion = 0;
};

static UpdateState LoadUpdateState() {
    UpdateState s;
    std::string json;
    if (ReadFileBytes(UpdateStatePath(), json)) {
        s.lastAcceptedManifestSequence = JsonString(json, "lastAcceptedManifestSequence");
        s.lastInstalledVersion = JsonInt(json, "lastInstalledVersion", 0);
        s.lastSuccessfulLaunchVersion = JsonInt(json, "lastSuccessfulLaunchVersion", 0);
        s.lastRejectedBadVersion = JsonInt(json, "lastRejectedBadVersion", 0);
    }
    return s;
}

static void SaveUpdateState(const UpdateState& s) {
    std::error_code ec;
    fs::create_directories(DataDir(), ec);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\n"
        "  \"lastAcceptedManifestSequence\": \"%s\",\n"
        "  \"lastInstalledVersion\": %lld,\n"
        "  \"lastSuccessfulLaunchVersion\": %lld,\n"
        "  \"lastRejectedBadVersion\": %lld\n"
        "}\n",
        s.lastAcceptedManifestSequence.c_str(), s.lastInstalledVersion,
        s.lastSuccessfulLaunchVersion, s.lastRejectedBadVersion);
    WriteFileBytes(UpdateStatePath(), buf, strlen(buf));
}

static bool LaunchProcess(const fs::path& exe, const std::wstring& args) {
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    if (!args.empty()) cmd += L" " + args;
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        exe.parent_path().c_str(), &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// Absolute path to a Windows system tool, avoiding PATH hijacking (same guard used for tar.exe).
static fs::path System32Path(const wchar_t* exeName) {
    wchar_t sysDir[MAX_PATH]{};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    return fs::path(sysDir) / exeName;
}

// Runs a helper exe hidden, waits for it, and returns its exit code (-1 on launch failure).
static int RunProcessHidden(const fs::path& exe, const std::wstring& cmdLine, DWORD timeoutMs) {
    std::wstring cmd = cmdLine;
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    DWORD code = (DWORD)-1;
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

#ifdef VISHWAKARMA_INSTALLER
// =================================================================================
// INSTALLER : compiled only into VishwakarmaSetup.exe
// =================================================================================

#include "AccountManager.h"

static std::string LoadResourceBytes(int id) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return "";
    HGLOBAL h = LoadResource(nullptr, res);
    if (!h) return "";
    DWORD size = SizeofResource(nullptr, res);
    const char* data = (const char*)LockResource(h);
    if (!data || size == 0) return "";
    return std::string(data, size);
}

// ------------------------------------------------------------------ License agreement dialog
// Shows the EULA + Privacy Policy (embedded from website/content/start) with Accept / Reject.
// Accept proceeds with the installation, Reject aborts it. Skipped in --update mode: the
// user already accepted at first install and updates must stay silent.

// Strips the leading hugo front matter block (--- ... ---) from a markdown file.
static std::string StripFrontMatter(const std::string& md) {
    if (md.rfind("---", 0) != 0) return md;
    size_t end = md.find("\n---", 3);
    if (end == std::string::npos) return md;
    size_t newline = md.find('\n', end + 1);
    return newline == std::string::npos ? "" : md.substr(newline + 1);
}

static std::atomic<int> g_licenseDecision = 0; // 0 = undecided, 1 = accept, -1 = reject.
#define IDC_VW_ACCEPT 301
#define IDC_VW_REJECT 302

static LRESULT CALLBACK LicenseWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_VW_ACCEPT) { g_licenseDecision = 1; DestroyWindow(hWnd); return 0; }
        if (LOWORD(wParam) == IDC_VW_REJECT) { g_licenseDecision = -1; DestroyWindow(hWnd); return 0; }
        break;
    case WM_CLOSE: // Closing the window counts as Reject.
        g_licenseDecision = -1;
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, message, wParam, lParam);
}

static bool ShowLicenseAgreementDialog() {
    std::string text = "END USER LICENSE AGREEMENT\r\n\r\n"
        + StripFrontMatter(LoadResourceBytes(IDR_VW_EULA_MD))
        + "\r\n\r\n========================================\r\n\r\nPRIVACY POLICY\r\n\r\n"
        + StripFrontMatter(LoadResourceBytes(IDR_VW_PRIVACY_MD));
    // EDIT controls need \r\n line breaks; the markdown sources use \n.
    std::string normalized;
    normalized.reserve(text.size() + 256);
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) normalized += '\r';
        normalized += text[i];
    }
    std::wstring wide = Utf8ToWide(normalized);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = LicenseWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"VishwakarmaSetupLicense";
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);

    const int dpi = GetDpiForSystem();
    const int scale = dpi > 0 ? dpi : 96;
    auto px = [scale](int logical) { return logical * scale / 96; };
    const int width = px(720), height = px(640);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    HWND hWnd = CreateWindowExW(0, wc.lpszClassName,
        L"Vishwakarma Setup - License Agreement and Privacy Policy",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        (work.right - width) / 2, (work.bottom - height) / 2, width, height,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hWnd) return false;

    RECT client{};
    GetClientRect(hWnd, &client);
    const int margin = px(12), buttonWidth = px(120), buttonHeight = px(32);
    HWND label = CreateWindowExW(0, L"STATIC",
        L"Please review the End User License Agreement and Privacy Policy below. "
        L"Click Accept to proceed with the installation, or Reject to abort.",
        WS_CHILD | WS_VISIBLE, margin, margin, client.right - 2 * margin, px(34),
        hWnd, nullptr, wc.hInstance, nullptr);
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", wide.c_str(),
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        margin, margin + px(38), client.right - 2 * margin,
        client.bottom - 3 * margin - px(38) - buttonHeight,
        hWnd, nullptr, wc.hInstance, nullptr);
    HWND accept = CreateWindowExW(0, L"BUTTON", L"Accept",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        client.right - margin - 2 * buttonWidth - px(8), client.bottom - margin - buttonHeight,
        buttonWidth, buttonHeight, hWnd, (HMENU)IDC_VW_ACCEPT, wc.hInstance, nullptr);
    HWND reject = CreateWindowExW(0, L"BUTTON", L"Reject",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        client.right - margin - buttonWidth, client.bottom - margin - buttonHeight,
        buttonWidth, buttonHeight, hWnd, (HMENU)IDC_VW_REJECT, wc.hInstance, nullptr);

    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    for (HWND child : { label, edit, accept, reject })
        if (child) SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);

    g_licenseDecision = 0;
    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return g_licenseDecision == 1;
}

// Rules from release.md: default per-user, no admin needed. --allUsers tries Program Files
// and silently falls back to the per-user location when permission is denied.
static fs::path PickInstallDir(bool allUsers) {
    if (allUsers) {
        fs::path pf = KnownFolder(FOLDERID_ProgramFiles) / kCompanyFolder;
        std::error_code ec;
        fs::create_directories(pf, ec);
        if (!ec) {
            // Confirm we can actually write there before committing to it.
            fs::path probe = pf / L"write_probe.tmp";
            if (WriteFileBytes(probe, "x", 1)) {
                fs::remove(probe, ec);
                return pf;
            }
        }
    }
    return KnownFolder(FOLDERID_LocalAppData) / L"Programs" / kCompanyFolder;
}

// Replace targetExe with the payload even while the old exe is running: a running exe cannot
// be overwritten but CAN be renamed. Old file is parked as Vishwakarma.exe.old and removed
// opportunistically on the next run.
static bool InstallPayload(const fs::path& dir, const std::string& payload, const std::string& json,
                           std::wstring& error) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path exe = dir / kAppExeName;
    fs::path exeNew = dir / L"Vishwakarma.exe.new";
    fs::path exeOld = dir / L"Vishwakarma.exe.old";

    fs::remove(exeOld, ec); // Leftover from a previous update; may fail if still running.
    if (!WriteFileBytes(exeNew, payload.data(), payload.size())) {
        error = L"Could not write to installation folder:\n" + dir.wstring();
        return false;
    }
    if (fs::exists(exe)) {
        if (!MoveFileExW(exe.c_str(), exeOld.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            error = L"Could not replace the existing Vishwakarma.exe. Close the application and retry.";
            fs::remove(exeNew, ec);
            return false;
        }
    }
    if (!MoveFileExW(exeNew.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(exeOld.c_str(), exe.c_str(), 0); // Roll the old version back.
        error = L"Could not activate the new Vishwakarma.exe.";
        return false;
    }
    WriteFileBytes(dir / kReleaseJsonName, json.data(), json.size());
    return true;
}

// Writes VishwakarmaExtension.exe (the embedded-CPython extension worker) next to the
// application exe. Same rename dance as InstallPayload: a running worker cannot be
// overwritten but can be renamed aside.
static bool InstallExtensionWorker(const fs::path& dir, std::wstring& error) {
    std::string payload = LoadResourceBytes(IDR_VW_EXT_WORKER_EXE);
    if (payload.empty()) {
        error = L"The extension worker is missing from the setup file.";
        return false;
    }
    std::error_code ec;
    fs::path exe = dir / L"VishwakarmaExtension.exe";
    fs::path exeNew = dir / L"VishwakarmaExtension.exe.new";
    fs::path exeOld = dir / L"VishwakarmaExtension.exe.old";

    fs::remove(exeOld, ec);
    if (!WriteFileBytes(exeNew, payload.data(), payload.size())) {
        error = L"Could not write the extension worker to:\n" + dir.wstring();
        return false;
    }
    if (fs::exists(exe) && !MoveFileExW(exe.c_str(), exeOld.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = L"Could not replace VishwakarmaExtension.exe. Close the application and retry.";
        fs::remove(exeNew, ec);
        return false;
    }
    if (!MoveFileExW(exeNew.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(exeOld.c_str(), exe.c_str(), 0);
        error = L"Could not activate the new VishwakarmaExtension.exe.";
        return false;
    }
    return true;
}

struct EmbeddedExtension { int resourceId; const wchar_t* folderName; };

// The importer extensions bundled as zips by VishwakarmaSetup.rc; resource IDs
// must match it.
static const EmbeddedExtension kEmbeddedExtensions[] = {
    { IDR_VW_EXT_STD_ZIP, L"Interoperability-STD" },
    { IDR_VW_EXT_DXF_ZIP, L"Interoperability-DXF" },
};

// Unpacks a zip held in memory into destDir using the OS-provided bsdtar. bsdtar
// seeks to a zip's central directory, so the archive is staged to a temp file
// rather than piped through stdin.
static bool ExtractZipToDir(const std::string& zipBytes, const fs::path& destDir, std::wstring& error) {
    std::error_code ec;
    fs::create_directories(destDir, ec);

    wchar_t tempDir[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tempDir)) { error = L"Could not locate the temp folder."; return false; }
    wchar_t tempZip[MAX_PATH]{};
    if (!GetTempFileNameW(tempDir, L"vwx", 0, tempZip)) { error = L"Could not create a temp file."; return false; }
    fs::path zipPath = tempZip;
    if (!WriteFileBytes(zipPath, zipBytes.data(), zipBytes.size())) {
        fs::remove(zipPath, ec);
        error = L"Could not stage a bundled extension for extraction.";
        return false;
    }

    // Absolute System32 path avoids PATH hijacking; tar.exe ships with Windows 10 1803+.
    wchar_t sysDir[MAX_PATH]{};
    GetSystemDirectoryW(sysDir, MAX_PATH);
    fs::path tarExe = fs::path(sysDir) / L"tar.exe";
    std::wstring cmd = L"\"" + tarExe.wstring() + L"\" -xf \"" + zipPath.wstring() +
                       L"\" -C \"" + destDir.wstring() + L"\"";

    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    bool ok = false;
    if (CreateProcessW(tarExe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        DWORD wait = WaitForSingleObject(pi.hProcess, 60000);
        DWORD code = 1;
        if (wait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
        else TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ok = (wait == WAIT_OBJECT_0 && code == 0);
        if (!ok) error = L"Could not extract a bundled extension (tar.exe failed).";
    } else {
        error = L"Could not launch tar.exe to extract bundled extensions.";
    }
    fs::remove(zipPath, ec);
    return ok;
}

// Unpacks each bundled importer extension into <dir>\extensions\<name> so the
// application finds it at <exe dir>\extensions\<name>\main.py.
static bool InstallBundledExtensions(const fs::path& dir, std::wstring& error) {
    bool allOk = true;
    for (const EmbeddedExtension& ext : kEmbeddedExtensions) {
        std::string zip = LoadResourceBytes(ext.resourceId);
        if (zip.empty()) {
            error = L"A bundled extension is missing from the setup file.";
            allOk = false;
            continue;
        }
        fs::path dest = dir / L"extensions" / ext.folderName;
        std::error_code ec;
        fs::remove_all(dest, ec); // Replace any previously installed version's files.
        if (!ExtractZipToDir(zip, dest, error)) allOk = false;
    }
    return allOk;
}

// Unpacks the third-party license texts (built from the repo's OpenSourceLicenses folder
// plus LICENSE.md by GenerateRelease.ps1) into <dir>\OpenSourceLicenses, as required by
// the licenses of the bundled libraries.
static bool InstallOpenSourceLicenses(const fs::path& dir, std::wstring& error) {
    std::string zip = LoadResourceBytes(IDR_VW_LICENSES_ZIP);
    if (zip.empty()) {
        error = L"The license texts are missing from the setup file.";
        return false;
    }
    fs::path dest = dir / L"OpenSourceLicenses";
    std::error_code ec;
    fs::remove_all(dest, ec); // Replace any previously installed version's files.
    return ExtractZipToDir(zip, dest, error);
}

static void CreateDesktopShortcut(const fs::path& targetExe) {
    fs::path desktop = KnownFolder(FOLDERID_Desktop);
    if (desktop.empty()) return;
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, (void**)&link))) return;
    link->SetPath(targetExe.c_str());
    link->SetWorkingDirectory(targetExe.parent_path().c_str());
    link->SetIconLocation(targetExe.c_str(), 0);
    link->SetDescription(L"Mission Vishwakarma - CAD / CAM / Engineering");
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&file))) {
        file->Save((desktop / L"Vishwakarma.lnk").c_str(), TRUE);
        file->Release();
    }
    link->Release();
}

// Registers (or updates) the per-user background-update task via schtasks.exe. Idempotent:
// /F updates an existing "VishwakarmaUpgrade" task in place rather than creating a duplicate, which
// also satisfies the "if present, just fix it" rule. /IT runs it only while the user is logged on,
// /RL LIMITED with the user's own (non-elevated) token. /SC DAILY /MO 6 fires every 6 (not 7) days
// so the weekday it lands on keeps rotating, spreading update checks across the whole week instead
// of pinning them to one weekday. Best effort; a failure is non-fatal.
static bool CreateUpgradeScheduledTask(const fs::path& appExe) {
    fs::path schtasks = System32Path(L"schtasks.exe");
    std::wstring cmd = L"\"" + schtasks.wstring() + L"\" /Create /F /TN " + kScheduledTaskName +
        L" /SC DAILY /MO 6 /IT /RL LIMITED /TR \"\\\"" + appExe.wstring() + L"\\\" --background-update\"";
    return RunProcessHidden(schtasks, cmd, 30000) == 0;
}

// Adds the per-user "Apps & features" entry so the user can uninstall from Windows Settings.
// UninstallString points back at the application exe with --uninstall.
static void RegisterUninstallEntry(const fs::path& dir, const fs::path& exe, long long version) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MissionVishwakarma",
        0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    auto setStr = [&](const wchar_t* name, const std::wstring& val) {
        RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)val.c_str(),
                       (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    };
    setStr(L"DisplayName", L"Mission Vishwakarma");
    setStr(L"DisplayIcon", exe.wstring());
    setStr(L"Publisher", L"Ram Shanker");
    setStr(L"InstallLocation", dir.wstring());
    setStr(L"DisplayVersion", std::to_wstring(version));
    setStr(L"UninstallString", L"\"" + exe.wstring() + L"\" --uninstall");
    DWORD one = 1;
    RegSetValueExW(key, L"NoModify", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
    RegSetValueExW(key, L"NoRepair", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
    RegCloseKey(key);
}

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int) {
    bool allUsers = false, updateMode = false, noLaunch = false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        if (_wcsicmp(argv[i], L"--allUsers") == 0) allUsers = true;
        else if (_wcsicmp(argv[i], L"--update") == 0) updateMode = true;
        else if (_wcsicmp(argv[i], L"--no-launch") == 0) noLaunch = true;
    }
    if (argv) LocalFree(argv);

    // Rule: only one installer runs at a time. Global\ covers the all-users case too; fall
    // back to the per-session Local\ namespace if the Global one cannot be created.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Global\\MissionVishwakarmaInstallerLock");
    if (!mutex) mutex = CreateMutexW(nullptr, TRUE, L"Local\\MissionVishwakarmaInstallerLock");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!updateMode)
            MessageBoxW(nullptr, L"Another Vishwakarma installer is already running.",
                        L"Vishwakarma Setup", MB_OK | MB_ICONINFORMATION);
        return 1;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::string payload = LoadResourceBytes(IDR_VW_PAYLOAD_EXE);
    std::string json = LoadResourceBytes(IDR_VW_RELEASE_JSON);
    if (payload.empty() || json.empty()) {
        MessageBoxW(nullptr, L"Setup file is corrupted (embedded payload missing). Please download again.",
                    L"Vishwakarma Setup", MB_OK | MB_ICONERROR);
        return 1;
    }
    long long newVersion = JsonInt(json, "version", 0);

    // First-install (and manual reinstall) runs must present the EULA + Privacy Policy.
    // Reject aborts before anything touches the disk. --update stays silent: the user
    // already accepted when installing the version that staged this update.
    if (!updateMode && !ShowLicenseAgreementDialog()) {
        if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
        CoUninitialize();
        return 1;
    }

    fs::path dir = PickInstallDir(allUsers);
    fs::path exe = dir / kAppExeName;

    // Overwrite only when the embedded version is newer than whatever is already installed.
    std::string existingJson;
    long long existingVersion = 0;
    if (ReadFileBytes(dir / kReleaseJsonName, existingJson))
        existingVersion = JsonInt(existingJson, "version", 0);

    bool installed = true;
    if (newVersion > existingVersion || !fs::exists(exe)) {
        std::wstring error;
        installed = InstallPayload(dir, payload, json, error);
        if (!installed && !updateMode)
            MessageBoxW(nullptr, error.c_str(), L"Vishwakarma Setup", MB_OK | MB_ICONERROR);
        // Bundled Python extensions and their worker travel with the exe; unpack them
        // alongside it. A failure here is non-fatal: the app still runs, only importers
        // are unavailable.
        if (installed) {
            std::wstring extError;
            if (!InstallBundledExtensions(dir, extError) && !updateMode)
                MessageBoxW(nullptr, extError.c_str(), L"Vishwakarma Setup", MB_OK | MB_ICONWARNING);
            std::wstring workerError;
            if (!InstallExtensionWorker(dir, workerError) && !updateMode)
                MessageBoxW(nullptr, workerError.c_str(), L"Vishwakarma Setup", MB_OK | MB_ICONWARNING);
            // License texts travel with the exe too; a failure is equally non-fatal.
            std::wstring licenseError;
            if (!InstallOpenSourceLicenses(dir, licenseError) && !updateMode)
                MessageBoxW(nullptr, licenseError.c_str(), L"Vishwakarma Setup", MB_OK | MB_ICONWARNING);
        }
    }

    if (installed) {
        CreateDesktopShortcut(exe);
        UpdateState st = LoadUpdateState();
        st.lastInstalledVersion = newVersion;
        SaveUpdateState(st);
        // Ed25519 installation identity key pair: created here at install time; the
        // application re-creates it on launch if it ever goes missing.
        AccountManager::EnsureInstallationKey();

        // The background-update task and the uninstall entry are per-user only: the task
        // must run as the logged-in user (see release.md), which an elevated all-users install
        // cannot reliably identify. Both are idempotent, so re-running on every update self-heals.
        fs::path localPrograms = KnownFolder(FOLDERID_LocalAppData) / L"Programs" / kCompanyFolder;
        if (dir == localPrograms) {
            CreateUpgradeScheduledTask(exe);
            RegisterUninstallEntry(dir, exe, newVersion);
        }
    }

    if (updateMode) {
        // Consume the staging marker so the application does not re-trigger this update.
        std::error_code ec;
        fs::remove(StagedMarkerPath(), ec);
    }

    if (!noLaunch && fs::exists(exe)) LaunchProcess(exe, L"");

    CoUninitialize();
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return installed ? 0 : 1;
}

#else
// =================================================================================
// APPLICATION SIDE : compiled only into Vishwakarma.exe
// =================================================================================

extern std::atomic<bool> shutdownSignal; // Defined in Main.cpp

// ------------------------------------------------------------------ Crypto (OpenSSL, static)
static std::string Sha256HexOfFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";
    std::string result;
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1) {
        char buf[65536];
        while (f.read(buf, sizeof(buf)) || f.gcount() > 0)
            EVP_DigestUpdate(ctx, buf, (size_t)f.gcount());
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        if (EVP_DigestFinal_ex(ctx, digest, &len) == 1) {
            char hex[2 * EVP_MAX_MD_SIZE + 1];
            for (unsigned int i = 0; i < len; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
            result.assign(hex, 2 * len);
        }
    }
    EVP_MD_CTX_free(ctx);
    return result;
}

static bool VerifyManifestSignature(const std::string& manifest, const std::string& signature) {
    BIO* bio = BIO_new_mem_buf(kManifestPublicKeyPem, -1);
    if (!bio) return false;
    EVP_PKEY* key = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!key) return false;
    bool ok = false;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx) {
        // Pure Ed25519: no separate digest, EVP_DigestVerify over the whole message.
        if (EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1)
            ok = EVP_DigestVerify(ctx, (const unsigned char*)signature.data(), signature.size(),
                                  (const unsigned char*)manifest.data(), manifest.size()) == 1;
        EVP_MD_CTX_free(ctx);
    }
    EVP_PKEY_free(key);
    return ok;
}

// ------------------------------------------------------------------ HTTPS download (WinHTTP)
// Resolves the proxy to use for `url` strictly from the current user's system (Internet Options /
// GPO) configuration: WPAD auto-detect, an explicit auto-config (PAC) URL, or a static proxy with
// its bypass list. Returns false for a direct connection when none applies. On success the caller
// owns info.lpszProxy / info.lpszProxyBypass and must GlobalFree them (WinHTTP copies them when
// they are handed to WINHTTP_OPTION_PROXY, so freeing straight after that call is correct).
static bool ResolveSystemProxy(HINTERNET session, const std::wstring& url, WINHTTP_PROXY_INFO& info) {
    ZeroMemory(&info, sizeof(info));
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ie{};
    if (!WinHttpGetIEProxyConfigForCurrentUser(&ie)) return false; // No per-user config -> direct.

    bool resolved = false;
    // Auto-detect and/or PAC URL: let WinHTTP evaluate the script for this specific URL.
    if (ie.fAutoDetect || ie.lpszAutoConfigUrl) {
        WINHTTP_AUTOPROXY_OPTIONS opt{};
        opt.dwFlags = (ie.lpszAutoConfigUrl ? WINHTTP_AUTOPROXY_CONFIG_URL : 0) |
                      (ie.fAutoDetect ? WINHTTP_AUTOPROXY_AUTO_DETECT : 0);
        opt.dwAutoDetectFlags = ie.fAutoDetect
            ? (WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A) : 0;
        opt.lpszAutoConfigUrl = ie.lpszAutoConfigUrl;
        opt.fAutoLogonIfChallenged = TRUE;
        if (WinHttpGetProxyForUrl(session, url.c_str(), &opt, &info)) resolved = true;
    }
    // Static proxy (e.g. "proxy.corp:8080" or "http=...;https=...") pushed by GPO.
    if (!resolved && ie.lpszProxy) {
        auto dup = [](LPWSTR s) -> LPWSTR {
            if (!s) return nullptr;
            size_t n = (wcslen(s) + 1) * sizeof(wchar_t);
            LPWSTR d = (LPWSTR)GlobalAlloc(GPTR, n);
            if (d) memcpy(d, s, n);
            return d;
        };
        info.dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        info.lpszProxy = dup(ie.lpszProxy);
        info.lpszProxyBypass = dup(ie.lpszProxyBypass);
        resolved = info.lpszProxy != nullptr;
    }
    if (ie.lpszAutoConfigUrl) GlobalFree(ie.lpszAutoConfigUrl);
    if (ie.lpszProxy) GlobalFree(ie.lpszProxy);
    if (ie.lpszProxyBypass) GlobalFree(ie.lpszProxyBypass);
    return resolved;
}

static bool HttpGet(const std::wstring& url, std::string& out, const fs::path* toFile) {
    out.clear();
    URL_COMPONENTS uc{ sizeof(uc) };
    wchar_t host[256]{}, path[2048]{};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;

    bool ok = false;
    // The session default is direct; the actual proxy is resolved per URL from the user's system
    // configuration and applied to the request below, so a proxy change takes effect immediately.
    HINTERNET session = WinHttpOpen(L"VishwakarmaUpdater/1", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host, uc.nPort, 0) : nullptr;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path, nullptr,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    if (request) {
        // Route through the corporate proxy when the system is configured for one.
        WINHTTP_PROXY_INFO proxyInfo{};
        if (ResolveSystemProxy(session, url, proxyInfo)) {
            WinHttpSetOption(request, WINHTTP_OPTION_PROXY, &proxyInfo, sizeof(proxyInfo));
            if (proxyInfo.lpszProxy) GlobalFree(proxyInfo.lpszProxy);
            if (proxyInfo.lpszProxyBypass) GlobalFree(proxyInfo.lpszProxyBypass);
        }
        // Let WinHTTP answer a 407 with the logged-in user's default credentials (NTLM/Negotiate),
        // covering authenticating proxies transparently without any app-specific setting.
        DWORD autoLogon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_LOW;
        WinHttpSetOption(request, WINHTTP_OPTION_AUTOLOGON_POLICY, &autoLogon, sizeof(autoLogon));
    }
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                      WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                && WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, statusSize = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            std::ofstream file;
            if (toFile) {
                file.open(*toFile, std::ios::binary | std::ios::trunc);
                if (!file) status = 0;
            }
            std::vector<char> buf(65536);
            DWORD read = 0;
            ok = (status == 200);
            while (ok && WinHttpReadData(request, buf.data(), (DWORD)buf.size(), &read) && read > 0) {
                if (toFile) { file.write(buf.data(), read); ok = file.good(); }
                else out.append(buf.data(), read);
            }
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

// ------------------------------------------------------------------ Manifest evaluation
static long long ParseIso8601Utc(const std::string& s) {
    int y, mo, d, h, mi, se;
    if (sscanf_s(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    tm t{};
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
    t.tm_hour = h; t.tm_min = mi; t.tm_sec = se;
    return _mkgmtime64(&t);
}

// Extract the json object of the first release matching windows / x64 / stable channel.
static std::string FindWindowsX64Release(const std::string& manifest) {
    size_t arr = manifest.find("\"releases\"");
    if (arr == std::string::npos) return "";
    size_t pos = manifest.find('[', arr);
    size_t end = manifest.find(']', arr);
    if (pos == std::string::npos || end == std::string::npos) return "";
    while (true) {
        size_t open = manifest.find('{', pos);
        if (open == std::string::npos || open > end) return "";
        size_t close = manifest.find('}', open);
        if (close == std::string::npos) return "";
        std::string obj = manifest.substr(open, close - open + 1);
        if (JsonString(obj, "platform") == "windows" && JsonString(obj, "instructionSet") == "x64" &&
            JsonString(obj, "channel") == "stable")
            return obj;
        pos = close + 1;
    }
}

static bool IsVersionBlocked(const std::string& manifest, long long version) {
    size_t arr = manifest.find("\"blockedVersions\"");
    if (arr == std::string::npos) return false;
    size_t open = manifest.find('[', arr);
    size_t close = manifest.find(']', arr);
    if (open == std::string::npos || close == std::string::npos) return false;
    std::string list = manifest.substr(open + 1, close - open - 1);
    const char* p = list.c_str();
    while (*p) {
        if (isdigit((unsigned char)*p)) {
            char* next = nullptr;
            if (_strtoi64(p, &next, 10) == version) return true;
            p = next;
        } else p++;
    }
    return false;
}

// One full check-and-stage cycle. Every failure silently aborts: next cycle retries.
static void RunUpdateCheckOnce(long long currentVersion) {
    // Rule: only one updater downloads at a time per user.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\MissionVishwakarmaUpdaterLock");
    if (!mutex) return;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(mutex); return; }

    do {
        std::string manifest, signature;
        if (!HttpGet(kManifestUrl, manifest, nullptr)) break;
        if (!HttpGet(kManifestSigUrl, signature, nullptr)) break;
        if (!VerifyManifestSignature(manifest, signature)) break;

        long long now = _time64(nullptr);
        long long validUntil = ParseIso8601Utc(JsonString(manifest, "validUntil"));
        if (validUntil == 0 || now > validUntil) break; // Expired or malformed manifest.
        if (JsonString(manifest, "appId") != "MissionVishwakarma") break;

        UpdateState state = LoadUpdateState();
        std::string sequence = JsonString(manifest, "manifestSequence");
        if (sequence.empty() || sequence < state.lastAcceptedManifestSequence) break; // Rollback guard.

        std::string release = FindWindowsX64Release(manifest);
        if (release.empty()) break;
        long long newVersion = JsonInt(release, "version", 0);
        long long minFrom = JsonInt(release, "minUpdateFromVersion", 1);
        long long size = JsonInt(release, "size", 0);
        std::string sha256 = JsonString(release, "sha256");
        std::string fileName = JsonString(release, "fileName");
        std::string url = JsonString(release, "url");
        if (newVersion <= currentVersion || currentVersion < minFrom) break;
        if (newVersion == state.lastRejectedBadVersion) break;
        if (IsVersionBlocked(manifest, newVersion)) break;
        if (sha256.empty() || url.empty() || fileName.empty() || size <= 0) break;

        state.lastAcceptedManifestSequence = sequence;
        SaveUpdateState(state);

        // Already staged and intact? Nothing to download.
        std::string marker;
        if (ReadFileBytes(StagedMarkerPath(), marker) && JsonInt(marker, "version", 0) == newVersion &&
            fs::exists(StagingDir() / Utf8ToWide(fileName)))
            break;

        std::error_code ec;
        fs::create_directories(StagingDir(), ec);
        fs::path setupPath = StagingDir() / Utf8ToWide(fileName);
        std::string unused;
        if (!HttpGet(Utf8ToWide(url), unused, &setupPath)) break;
        if ((long long)fs::file_size(setupPath, ec) != size || Sha256HexOfFile(setupPath) != sha256) {
            fs::remove(setupPath, ec);
            break;
        }

        char markerJson[512];
        snprintf(markerJson, sizeof(markerJson),
                 "{\n  \"version\": %lld,\n  \"fileName\": \"%s\",\n  \"sha256\": \"%s\"\n}\n",
                 newVersion, fileName.c_str(), sha256.c_str());
        WriteFileBytes(StagedMarkerPath(), markerJson, strlen(markerJson));
    } while (false);

    ReleaseMutex(mutex);
    CloseHandle(mutex);
}

// ------------------------------------------------------------------ Public entry points
bool SoftwareUpdateOnAppLaunch() {
    long long currentVersion = InstalledVersion();
    if (currentVersion <= 0) return false; // Developer / unpackaged build: no update handling.

    UpdateState state = LoadUpdateState();

    std::string marker;
    if (ReadFileBytes(StagedMarkerPath(), marker)) {
        long long stagedVersion = JsonInt(marker, "version", 0);
        fs::path setupPath = StagingDir() / Utf8ToWide(JsonString(marker, "fileName"));
        std::error_code ec;
        if (stagedVersion > currentVersion && fs::exists(setupPath) &&
            Sha256HexOfFile(setupPath) == JsonString(marker, "sha256")) {
            // Rule: no two apply-update operations concurrently.
            HANDLE applyLock = CreateMutexW(nullptr, TRUE, L"Local\\MissionVishwakarmaAppLaunchLock");
            bool alreadyApplying = applyLock && GetLastError() == ERROR_ALREADY_EXISTS;
            if (!alreadyApplying && LaunchProcess(setupPath, L"--update")) {
                // Installer overwrites us, deletes the marker and relaunches the new version.
                if (applyLock) CloseHandle(applyLock);
                return true;
            }
            if (applyLock) CloseHandle(applyLock);
        } else {
            // Stale, superseded or corrupted staging: throw it away.
            fs::remove_all(StagingDir(), ec);
        }
    }

    state.lastSuccessfulLaunchVersion = currentVersion;
    SaveUpdateState(state);
    return false;
}

void StartSoftwareUpdateThread() {
    long long currentVersion = InstalledVersion();
    if (currentVersion <= 0) return; // Developer / unpackaged build.
    std::thread([currentVersion]() {
        std::mt19937 rng{ std::random_device{}() };
        std::uniform_int_distribution<int> minutes(10, 600); // Spec: 10 minutes to 10 hours.
        while (!shutdownSignal.load(std::memory_order_relaxed)) {
            long long remainingSeconds = (long long)minutes(rng) * 60;
            while (remainingSeconds > 0 && !shutdownSignal.load(std::memory_order_relaxed)) {
                Sleep(5000);
                remainingSeconds -= 5;
            }
            if (shutdownSignal.load(std::memory_order_relaxed)) return;
            RunUpdateCheckOnce(currentVersion);
        }
    }).detach();
}

// ------------------------------------------------------------------ Background-update / uninstall
// True if another Vishwakarma.exe is running (any besides this process). Used to avoid replacing
// the exe out from under an active user; a running instance applies staged updates on next launch.
static bool AnotherAppInstanceRunning() {
    DWORD self = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return true; // Cannot tell: assume yes and skip applying.
    PROCESSENTRY32W pe{ sizeof(pe) };
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID != self && _wcsicmp(pe.szExeFile, kAppExeName) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

static void DeleteUpgradeScheduledTask() {
    fs::path schtasks = System32Path(L"schtasks.exe");
    std::wstring cmd = L"\"" + schtasks.wstring() + L"\" /Delete /F /TN " + kScheduledTaskName;
    RunProcessHidden(schtasks, cmd, 30000);
}

int RunBackgroundUpdate() {
    long long currentVersion = InstalledVersion();
    if (currentVersion <= 0) return 0; // Developer / unpackaged build: nothing to do.

    RunUpdateCheckOnce(currentVersion); // Download + verify + stage immediately (no random wait).

    // Apply only when we are the sole instance, so we never overwrite the exe of a running app.
    if (AnotherAppInstanceRunning()) return 0;
    SoftwareUpdateOnAppLaunch(); // Launches the staged setup with --update, which replaces us.
    return 0;
}

void RunUninstall() {
    if (MessageBoxW(nullptr, L"Remove Mission Vishwakarma from this computer?",
                    L"Vishwakarma Uninstall", MB_YESNO | MB_ICONQUESTION) != IDYES) return;

    fs::path exeDir = CurrentExeDir();
    std::error_code ec;

    DeleteUpgradeScheduledTask();

    fs::path desktop = KnownFolder(FOLDERID_Desktop);
    if (!desktop.empty()) fs::remove(desktop / L"Vishwakarma.lnk", ec);

    RegDeleteTreeW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MissionVishwakarma");

    fs::remove_all(DataDir(), ec); // Updater state + staging area under %LOCALAPPDATA%.

    // Acknowledge before spawning the self-deleter and exiting: the dialog must not keep the exe
    // image locked while cmd.exe tries to remove the folder.
    MessageBoxW(nullptr, L"Mission Vishwakarma has been removed.",
                L"Vishwakarma Uninstall", MB_OK | MB_ICONINFORMATION);

    // The install folder holds this running exe, which cannot delete itself. Hand the removal to a
    // detached cmd.exe that waits (~2 s) for this process to exit, then deletes the folder.
    fs::path cmdExe = System32Path(L"cmd.exe");
    std::wstring line = L"\"" + cmdExe.wstring() +
        L"\" /c ping 127.0.0.1 -n 3 >nul & rmdir /s /q \"" + exeDir.wstring() + L"\"";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(cmdExe.c_str(), line.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

#endif // VISHWAKARMA_INSTALLER
