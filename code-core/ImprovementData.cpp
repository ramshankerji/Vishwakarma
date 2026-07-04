// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

// Local usage statistics + system metrics collection. See ImprovementData.h for the design.
// Everything here runs on the dedicated ImprovementDataThread except the interaction
// counters (atomics incremented by the UI thread) and RecordRibbonAction.

#include "ImprovementData.h"
#include "AccountManager.h"

#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <winhttp.h>
#include <winioctl.h>
#include <dxgi1_6.h>
#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "sqlite3.h"
#include <openssl/evp.h>

#pragma comment(lib, "Winhttp.lib")
#pragma comment(lib, "Dxgi.lib")

namespace fs = std::filesystem;

extern std::atomic<bool> shutdownSignal; // Defined in Main.cpp

// Interaction counters incremented by the main UI thread (WndProc).
std::atomic<uint64_t> g_statLeftClicks = 0;
std::atomic<uint64_t> g_statMiddleClicks = 0;
std::atomic<uint64_t> g_statRightClicks = 0;
std::atomic<uint64_t> g_statKeyPresses = 0;

// ------------------------------------------------------------------ Constants
static const int kUsageIntervalSeconds = 5 * 60;        // One UsageLog row per 5 minutes.
static const int kUsageUploadIntervalSeconds = 24 * 3600; // Usage log upload cadence.
static const int kPendingUploadRetrySeconds = 60;       // Hardware/first-contact retry cadence.

// Telemetry endpoint. Debug builds talk to a local Django dev server, release builds to
// the production server (Raspberry Pi behind a Cloudflare tunnel).
#ifdef _DEBUG
static const wchar_t* kTelemetryUrl = L"http://127.0.0.1:8000/api/logs";
#else
static const wchar_t* kTelemetryUrl = L"https://mv-server.ramshanker.in/api/logs";
#endif

// ------------------------------------------------------------------ Small helpers
static fs::path DataDir() {
    PWSTR p = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) result = p;
    if (p) CoTaskMemFree(p);
    return result / L"Mission Vishwakarma";
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::string EpochToIso8601Utc(long long epoch) {
    tm t{};
    _gmtime64_s(&t, &epoch);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return buf;
}

static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
            else out += c;
        }
    }
    return out;
}

static std::string Sha256Hex(const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_Digest(data.data(), data.size(), digest, &len, EVP_sha256(), nullptr) != 1) return "";
    char hex[2 * EVP_MAX_MD_SIZE + 1];
    for (unsigned int i = 0; i < len; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return std::string(hex, 2 * len);
}

// Version of this installation, read from the release json next to the exe (0 = dev build).
static long long InstalledAppVersion() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    fs::path jsonPath = fs::path(buf).parent_path() / L"Vishwakarma_release_details.json";
    std::ifstream f(jsonPath, std::ios::binary);
    if (!f) return 0;
    std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t k = json.find("\"version\"");
    if (k == std::string::npos) return 0;
    size_t colon = json.find(':', k);
    if (colon == std::string::npos) return 0;
    return _strtoi64(json.c_str() + colon + 1, nullptr, 10);
}

// ------------------------------------------------------------------ Ribbon action recording
namespace {
std::mutex g_ribbonMutex;
std::map<uint32_t, uint32_t> g_ribbonActionCounts; // commandId -> clicks in current interval.
}

namespace ImprovementData {
void RecordRibbonAction(uint32_t commandId) {
    std::lock_guard<std::mutex> lock(g_ribbonMutex);
    g_ribbonActionCounts[commandId]++;
}
} // namespace ImprovementData

static std::string DrainRibbonActionsJson() {
    std::map<uint32_t, uint32_t> counts;
    {
        std::lock_guard<std::mutex> lock(g_ribbonMutex);
        counts.swap(g_ribbonActionCounts);
    }
    std::string json = "{";
    for (const auto& [id, count] : counts) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s\"%u\":%u", json.size() > 1 ? "," : "", id, count);
        json += buf;
    }
    json += "}";
    return json;
}

// =================================================================================
// SYSTEM METRICS COLLECTION (no personally identifiable information)
// =================================================================================

static std::string RegistryString(HKEY root, const wchar_t* subKey, const wchar_t* value) {
    wchar_t buf[256]{};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(root, subKey, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return "";
    return WideToUtf8(buf);
}

static DWORD RegistryDword(HKEY root, const wchar_t* subKey, const wchar_t* value) {
    DWORD data = 0, size = sizeof(data);
    RegGetValueW(root, subKey, value, RRF_RT_REG_DWORD, nullptr, &data, &size);
    return data;
}

static int PhysicalCoreCount() {
    DWORD size = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size);
    if (size == 0) return 0;
    std::vector<char> buffer(size);
    if (!GetLogicalProcessorInformationEx(
            RelationProcessorCore, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)buffer.data(), &size))
        return 0;
    int cores = 0;
    for (DWORD offset = 0; offset < size;) {
        auto* info = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)(buffer.data() + offset);
        if (info->Relationship == RelationProcessorCore) cores++;
        offset += info->Size;
    }
    return cores;
}

static std::string InstructionSets() {
    int regs[4]{};
    std::string isa = "x64 SSE2"; // Baseline for every x64 CPU.
    __cpuid(regs, 1);
    const bool sse42 = (regs[2] & (1 << 20)) != 0;
    const bool osxsave = (regs[2] & (1 << 27)) != 0;
    const bool avxBit = (regs[2] & (1 << 28)) != 0;
    const bool fma3 = (regs[2] & (1 << 12)) != 0;
    bool avxOsEnabled = false;
    if (osxsave) avxOsEnabled = (_xgetbv(0) & 0x6) == 0x6; // XMM + YMM state saved by OS.
    if (sse42) isa += " SSE4.2";
    if (avxBit && avxOsEnabled) {
        isa += " AVX";
        if (fma3) isa += " FMA3";
        __cpuidex(regs, 7, 0);
        if (regs[1] & (1 << 5)) isa += " AVX2";
        if ((regs[1] & (1 << 16)) && (_xgetbv(0) & 0xE0) == 0xE0) isa += " AVX512F";
    }
    return isa;
}

// RAM module type / speed from the SMBIOS firmware table (type 17: Memory Device).
static void SmbiosMemoryInfo(std::string& type, int& speedMTps) {
    type = "Unknown";
    speedMTps = 0;
    UINT size = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
    if (size == 0) return;
    std::vector<unsigned char> buffer(size);
    if (GetSystemFirmwareTable('RSMB', 0, buffer.data(), size) != size) return;
    if (size < 8) return;
    const unsigned char* p = buffer.data() + 8; // Skip RawSMBIOSData header.
    const unsigned char* end = buffer.data() + size;
    while (p + 4 <= end) {
        unsigned char structType = p[0], structLen = p[1];
        if (structType == 127 || structLen < 4) break;
        if (structType == 17 && p + structLen <= end && structLen >= 0x13) {
            switch (p[0x12]) { // SMBIOS MemoryType enumeration.
            case 20: type = "DDR"; break;
            case 21: type = "DDR2"; break;
            case 24: type = "DDR3"; break;
            case 26: type = "DDR4"; break;
            case 27: type = "LPDDR"; break;
            case 28: type = "LPDDR2"; break;
            case 29: type = "LPDDR3"; break;
            case 30: type = "LPDDR4"; break;
            case 34: type = "DDR5"; break;
            case 35: type = "LPDDR5"; break;
            default: break;
            }
            if (structLen >= 0x17) {
                int speed = p[0x15] | (p[0x16] << 8);
                if (speed > speedMTps) speedMTps = speed;
            }
            if (type != "Unknown") return; // First populated module is representative.
        }
        // Advance past the formatted area and the trailing string-set (double null).
        const unsigned char* next = p + structLen;
        while (next + 1 < end && (next[0] != 0 || next[1] != 0)) next++;
        p = next + 2;
    }
}

struct PhysicalDriveInfo {
    std::string bus;   // NVMe / SATA / USB / ...
    std::string kind;  // SSD / HDD / Unknown
    long long sizeGB = 0;
};

static std::vector<PhysicalDriveInfo> EnumeratePhysicalDrives() {
    std::vector<PhysicalDriveInfo> drives;
    for (int i = 0; i < 32; i++) {
        wchar_t path[32];
        swprintf_s(path, L"\\\\.\\PhysicalDrive%d", i);
        HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;

        PhysicalDriveInfo drive;
        DWORD bytes = 0;

        STORAGE_PROPERTY_QUERY query{};
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;
        char descriptorBuf[1024]{};
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                            descriptorBuf, sizeof(descriptorBuf), &bytes, nullptr)) {
            auto* desc = (STORAGE_DEVICE_DESCRIPTOR*)descriptorBuf;
            switch (desc->BusType) {
            case BusTypeNvme: drive.bus = "NVMe"; break;
            case BusTypeSata: drive.bus = "SATA"; break;
            case BusTypeUsb: drive.bus = "USB"; break;
            case BusTypeSCM: drive.bus = "SCM"; break;
            case BusTypeRAID: drive.bus = "RAID"; break;
            case BusTypeSd: drive.bus = "SD"; break;
            default: drive.bus = "Other"; break;
            }
        }

        query.PropertyId = StorageDeviceSeekPenaltyProperty;
        DEVICE_SEEK_PENALTY_DESCRIPTOR seek{};
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                            &seek, sizeof(seek), &bytes, nullptr))
            drive.kind = seek.IncursSeekPenalty ? "HDD" : "SSD";
        else
            drive.kind = (drive.bus == "NVMe") ? "SSD" : "Unknown";

        GET_LENGTH_INFORMATION length{};
        if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
                            &length, sizeof(length), &bytes, nullptr))
            drive.sizeGB = length.Length.QuadPart / (1000LL * 1000 * 1000);

        CloseHandle(h);
        drives.push_back(drive);
    }
    return drives;
}

// Sum of total / free bytes over all fixed logical volumes ("empty capacity").
static void LogicalDiskTotals(long long& totalGB, long long& freeGB) {
    totalGB = freeGB = 0;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        ULARGE_INTEGER freeToCaller{}, total{}, freeTotal{};
        if (GetDiskFreeSpaceExW(root, &freeToCaller, &total, &freeTotal)) {
            totalGB += (long long)(total.QuadPart / (1000ULL * 1000 * 1000));
            freeGB += (long long)(freeTotal.QuadPart / (1000ULL * 1000 * 1000));
        }
    }
}

static void OsVersionInfo(std::string& name, std::string& version, long long& build, long long& ubr) {
    name = RegistryString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                          L"ProductName");
    version = RegistryString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                             L"DisplayVersion");
    std::string buildStr = RegistryString(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber");
    build = buildStr.empty() ? 0 : atoll(buildStr.c_str());
    ubr = RegistryDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                        L"UBR");
    // ProductName still reports "Windows 10 ..." on Windows 11; build number disambiguates.
    if (build >= 22000 && name.find("Windows 10") != std::string::npos)
        name.replace(name.find("Windows 10"), 10, "Windows 11");
}

static void GpuInfo(std::string& name, long long& vramMB, std::string& driverVersion, bool& discrete) {
    name = "";
    vramMB = 0;
    driverVersion = "";
    discrete = false;
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
    IDXGIAdapter1* adapter = nullptr;
    IDXGIAdapter1* best = nullptr;
    DXGI_ADAPTER_DESC1 bestDesc{};
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            (!best || desc.DedicatedVideoMemory > bestDesc.DedicatedVideoMemory)) {
            if (best) best->Release();
            best = adapter;
            bestDesc = desc;
        } else {
            adapter->Release();
        }
    }
    if (best) {
        name = WideToUtf8(bestDesc.Description);
        vramMB = (long long)(bestDesc.DedicatedVideoMemory / (1024ULL * 1024));
        discrete = bestDesc.DedicatedVideoMemory >= 512ULL * 1024 * 1024;
        LARGE_INTEGER umd{};
        if (SUCCEEDED(best->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                     HIWORD(umd.HighPart), LOWORD(umd.HighPart),
                     HIWORD(umd.LowPart), LOWORD(umd.LowPart));
            driverVersion = buf;
        }
        best->Release();
    }
    factory->Release();
}

// Collects all system metrics. Returns the payload json and a fingerprint hash computed
// over the stable fields only (free disk space excluded: it changes constantly and must
// not re-trigger a "hardware changed" notification).
static void CollectHardwareStatistics(std::string& payloadJson, std::string& fingerprint) {
    std::string cpuName = RegistryString(HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
    long long cpuBaseMHz = RegistryDword(HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz");
    int physicalCores = PhysicalCoreCount();
    int logicalCores = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    std::string isa = InstructionSets();

    MEMORYSTATUSEX mem{ sizeof(mem) };
    GlobalMemoryStatusEx(&mem);
    long long ramTotalMB = (long long)(mem.ullTotalPhys / (1024ULL * 1024));
    std::string ramType;
    int ramSpeed = 0;
    SmbiosMemoryInfo(ramType, ramSpeed);

    std::vector<PhysicalDriveInfo> drives = EnumeratePhysicalDrives();
    long long diskTotalGB = 0, diskFreeGB = 0;
    LogicalDiskTotals(diskTotalGB, diskFreeGB);

    std::string osName, osVersion;
    long long osBuild = 0, osUbr = 0;
    OsVersionInfo(osName, osVersion, osBuild, osUbr);

    std::string gpuName, gpuDriver;
    long long gpuVramMB = 0;
    bool gpuDiscrete = false;
    GpuInfo(gpuName, gpuVramMB, gpuDriver, gpuDiscrete);

    std::string drivesJson = "[";
    for (size_t i = 0; i < drives.size(); i++) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s{\"bus\":\"%s\",\"kind\":\"%s\",\"sizeGB\":%lld}",
                 i ? "," : "", drives[i].bus.c_str(), drives[i].kind.c_str(), drives[i].sizeGB);
        drivesJson += buf;
    }
    drivesJson += "]";

    char buf[512];
    payloadJson = "{";
    snprintf(buf, sizeof(buf), "\"cpuName\":\"%s\",\"cpuPhysicalCores\":%d,\"cpuLogicalCores\":%d,"
             "\"cpuBaseMHz\":%lld,\"instructionSets\":\"%s\",",
             JsonEscape(cpuName).c_str(), physicalCores, logicalCores, cpuBaseMHz, isa.c_str());
    payloadJson += buf;
    snprintf(buf, sizeof(buf), "\"ramTotalMB\":%lld,\"ramType\":\"%s\",\"ramSpeedMTps\":%d,",
             ramTotalMB, ramType.c_str(), ramSpeed);
    payloadJson += buf;
    payloadJson += "\"drives\":" + drivesJson + ",";
    snprintf(buf, sizeof(buf), "\"diskTotalGB\":%lld,\"diskFreeGB\":%lld,", diskTotalGB, diskFreeGB);
    payloadJson += buf;
    snprintf(buf, sizeof(buf), "\"osName\":\"%s\",\"osVersion\":\"%s\",\"osBuild\":%lld,\"osUpdateBuildRevision\":%lld,",
             JsonEscape(osName).c_str(), JsonEscape(osVersion).c_str(), osBuild, osUbr);
    payloadJson += buf;
    snprintf(buf, sizeof(buf), "\"gpuName\":\"%s\",\"gpuVramMB\":%lld,\"gpuDriverVersion\":\"%s\",\"gpuDiscrete\":%s",
             JsonEscape(gpuName).c_str(), gpuVramMB, gpuDriver.c_str(), gpuDiscrete ? "true" : "false");
    payloadJson += buf;
    payloadJson += "}";

    // Stable fields only: intentionally no free-space, so daily use never looks like new hardware.
    std::string stable = cpuName + "|" + std::to_string(physicalCores) + "|" +
        std::to_string(logicalCores) + "|" + std::to_string(cpuBaseMHz) + "|" + isa + "|" +
        std::to_string(ramTotalMB) + "|" + ramType + "|" + std::to_string(ramSpeed) + "|" +
        drivesJson + "|" + std::to_string(diskTotalGB) + "|" + osName + "|" + osVersion + "|" +
        std::to_string(osBuild) + "|" + std::to_string(osUbr) + "|" + gpuName + "|" +
        std::to_string(gpuVramMB) + "|" + gpuDriver;
    fingerprint = Sha256Hex(stable);
}

// =================================================================================
// LOCAL SQLITE STORAGE : %LOCALAPPDATA%\Mission Vishwakarma\ImprovementStatistics.db
// =================================================================================

static bool ExecSql(sqlite3* db, const char* sql) {
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

static sqlite3* OpenStatisticsDb() {
    std::error_code ec;
    fs::create_directories(DataDir(), ec);
    std::string path = WideToUtf8((DataDir() / L"ImprovementStatistics.db").wstring());
    sqlite3* db = nullptr;
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return nullptr;
    }
    ExecSql(db, "PRAGMA journal_mode=WAL;");
    bool ok =
        ExecSql(db,
            "CREATE TABLE IF NOT EXISTS Meta ("
            " key TEXT PRIMARY KEY, value TEXT);") &&
        ExecSql(db,
            "CREATE TABLE IF NOT EXISTS HardwareStatistics ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " collectedUtc TEXT NOT NULL,"
            " fingerprint TEXT NOT NULL,"
            " payloadJson TEXT NOT NULL,"
            " uploadedUtc TEXT);") &&
        ExecSql(db,
            "CREATE TABLE IF NOT EXISTS UsageLog ("
            " id INTEGER PRIMARY KEY AUTOINCREMENT,"
            " intervalStartUtc TEXT NOT NULL,"
            " intervalSeconds INTEGER NOT NULL,"
            " openSeconds INTEGER NOT NULL,"
            " focusSeconds INTEGER NOT NULL,"
            " leftClicks INTEGER NOT NULL,"
            " middleClicks INTEGER NOT NULL,"
            " rightClicks INTEGER NOT NULL,"
            " keyPresses INTEGER NOT NULL,"
            " ribbonActionsJson TEXT NOT NULL);");
    if (!ok) {
        sqlite3_close(db);
        return nullptr;
    }
    return db;
}

static std::string MetaGet(sqlite3* db, const char* key) {
    std::string value;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT value FROM Meta WHERE key=?1;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            value = (const char*)sqlite3_column_text(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return value;
}

static void MetaSet(sqlite3* db, const char* key, const std::string& value) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "INSERT INTO Meta(key,value) VALUES(?1,?2)"
                               " ON CONFLICT(key) DO UPDATE SET value=?2;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
}

// Inserts a new HardwareStatistics row when the fingerprint differs from the latest row.
static void StoreHardwareIfChanged(sqlite3* db, const std::string& payloadJson,
                                   const std::string& fingerprint) {
    std::string lastFingerprint;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT fingerprint FROM HardwareStatistics"
                               " ORDER BY id DESC LIMIT 1;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            lastFingerprint = (const char*)sqlite3_column_text(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (lastFingerprint == fingerprint && !fingerprint.empty()) return;

    if (sqlite3_prepare_v2(db, "INSERT INTO HardwareStatistics"
                               " (collectedUtc, fingerprint, payloadJson) VALUES (?1,?2,?3);",
                               -1, &stmt, nullptr) == SQLITE_OK) {
        std::string now = EpochToIso8601Utc(_time64(nullptr));
        sqlite3_bind_text(stmt, 1, now.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, fingerprint.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, payloadJson.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
}

struct UsageDelta {
    long long intervalStartEpoch = 0;
    int intervalSeconds = 0;
    int openSeconds = 0;
    int focusSeconds = 0;
    uint64_t leftClicks = 0, middleClicks = 0, rightClicks = 0, keyPresses = 0;
    std::string ribbonActionsJson;
};

static void StoreUsageRow(sqlite3* db, const UsageDelta& d) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "INSERT INTO UsageLog (intervalStartUtc, intervalSeconds,"
                               " openSeconds, focusSeconds, leftClicks, middleClicks, rightClicks,"
                               " keyPresses, ribbonActionsJson) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9);",
                               -1, &stmt, nullptr) == SQLITE_OK) {
        std::string start = EpochToIso8601Utc(d.intervalStartEpoch);
        sqlite3_bind_text(stmt, 1, start.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, d.intervalSeconds);
        sqlite3_bind_int(stmt, 3, d.openSeconds);
        sqlite3_bind_int(stmt, 4, d.focusSeconds);
        sqlite3_bind_int64(stmt, 5, (sqlite3_int64)d.leftClicks);
        sqlite3_bind_int64(stmt, 6, (sqlite3_int64)d.middleClicks);
        sqlite3_bind_int64(stmt, 7, (sqlite3_int64)d.rightClicks);
        sqlite3_bind_int64(stmt, 8, (sqlite3_int64)d.keyPresses);
        sqlite3_bind_text(stmt, 9, d.ribbonActionsJson.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
}

// =================================================================================
// UPLOAD TO TELEMETRY SERVER
// =================================================================================

// HTTPS/HTTP POST of a json body. Returns true on HTTP 200 and fills the response body.
static bool HttpPostJson(const std::wstring& url, const std::string& body,
                         const std::string& signatureB64, std::string& response) {
    response.clear();
    URL_COMPONENTS uc{ sizeof(uc) };
    wchar_t host[256]{}, path[2048]{};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;

    bool ok = false;
    HINTERNET session = WinHttpOpen(L"VishwakarmaTelemetry/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = session ? WinHttpConnect(session, host, uc.nPort, 0) : nullptr;
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"POST", path, nullptr,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    if (request) {
        std::wstring headers = L"Content-Type: application/json\r\n";
        if (!signatureB64.empty()) {
            headers += L"X-MV-Signature: ";
            headers += std::wstring(signatureB64.begin(), signatureB64.end()); // Base64 is ASCII.
            headers += L"\r\n";
        }
        if (WinHttpSendRequest(request, headers.c_str(), (DWORD)headers.size(),
                               (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0, statusSize = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                WINHTTP_NO_HEADER_INDEX);
            if (status == 200) {
                char buf[8192];
                DWORD read = 0;
                ok = true;
                while (WinHttpReadData(request, buf, sizeof(buf), &read) && read > 0)
                    response.append(buf, read);
            }
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return ok;
}

// One upload attempt: pending hardware row (if any) + usage rows (only when includeUsage).
// On server acknowledgement, acked usage rows are deleted and the hardware row is marked
// uploaded. Returns true when the server accepted the request.
static bool TryUpload(sqlite3* db, bool includeUsage) {
    std::string installationKey = AccountManager::InstallationPublicKeyBase64();
    std::string sessionKey = AccountManager::SessionPublicKeyBase64();
    if (installationKey.empty() || sessionKey.empty()) return false;

    // Pending (not yet acknowledged) hardware row: newest one wins.
    long long hardwareRowId = 0;
    std::string hardwareJson, hardwareCollectedUtc;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT id, payloadJson, collectedUtc FROM HardwareStatistics"
                               " WHERE uploadedUtc IS NULL ORDER BY id DESC LIMIT 1;",
                               -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            hardwareRowId = sqlite3_column_int64(stmt, 0);
            hardwareJson = (const char*)sqlite3_column_text(stmt, 1);
            hardwareCollectedUtc = (const char*)sqlite3_column_text(stmt, 2);
        }
    }
    sqlite3_finalize(stmt);

    std::vector<long long> usageIds;
    std::string usageJson = "[";
    if (includeUsage &&
        sqlite3_prepare_v2(db, "SELECT id, intervalStartUtc, intervalSeconds, openSeconds,"
                               " focusSeconds, leftClicks, middleClicks, rightClicks, keyPresses,"
                               " ribbonActionsJson FROM UsageLog ORDER BY id LIMIT 4096;",
                               -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            long long id = sqlite3_column_int64(stmt, 0);
            usageIds.push_back(id);
            char buf[256];
            snprintf(buf, sizeof(buf),
                "%s{\"id\":%lld,\"start\":\"%s\",\"interval\":%d,\"open\":%d,\"focus\":%d,"
                "\"left\":%lld,\"middle\":%lld,\"right\":%lld,\"keys\":%lld,\"actions\":",
                usageJson.size() > 1 ? "," : "", id,
                (const char*)sqlite3_column_text(stmt, 1),
                sqlite3_column_int(stmt, 2), sqlite3_column_int(stmt, 3), sqlite3_column_int(stmt, 4),
                (long long)sqlite3_column_int64(stmt, 5), (long long)sqlite3_column_int64(stmt, 6),
                (long long)sqlite3_column_int64(stmt, 7), (long long)sqlite3_column_int64(stmt, 8));
            usageJson += buf;
            usageJson += (const char*)sqlite3_column_text(stmt, 9);
            usageJson += "}";
        }
        sqlite3_finalize(stmt);
    }
    usageJson += "]";

    if (hardwareRowId == 0 && usageIds.empty()) return true; // Nothing to send.

    std::string body = "{";
    body += "\"installationPublicKey\":\"" + installationKey + "\",";
    body += "\"sessionPublicKey\":\"" + sessionKey + "\",";
    body += "\"sessionKeySignature\":\"" + AccountManager::SessionKeySignatureBase64() + "\",";
    body += "\"appVersion\":" + std::to_string(InstalledAppVersion()) + ",";
    if (hardwareRowId != 0) {
        body += "\"hardware\":" + hardwareJson + ",";
        body += "\"hardwareCollectedUtc\":\"" + hardwareCollectedUtc + "\",";
    }
    body += "\"usage\":" + usageJson + "}";

    std::string signature = AccountManager::SignWithSessionKey(body);
    std::string response;
    if (!HttpPostJson(kTelemetryUrl, body, signature, response)) return false;
    if (response.find("\"status\":\"ok\"") == std::string::npos) return false;

    // Server acknowledged receipt: delete the uploaded usage rows, mark hardware uploaded.
    if (hardwareRowId != 0) {
        if (sqlite3_prepare_v2(db, "UPDATE HardwareStatistics SET uploadedUtc=?1 WHERE id<=?2;",
                               -1, &stmt, nullptr) == SQLITE_OK) {
            std::string now = EpochToIso8601Utc(_time64(nullptr));
            sqlite3_bind_text(stmt, 1, now.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 2, hardwareRowId);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    }
    if (!usageIds.empty()) {
        if (sqlite3_prepare_v2(db, "DELETE FROM UsageLog WHERE id<=?1;", -1, &stmt, nullptr)
            == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, usageIds.back());
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    }
    return true;
}

// =================================================================================
// DEDICATED STATISTICS THREAD
// =================================================================================

static bool ApplicationHasFocus() {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    return pid == GetCurrentProcessId();
}

void ImprovementDataThread() {
    sqlite3* db = OpenStatisticsDb();
    if (!db) return; // Statistics are strictly best-effort: never disturb the application.

    // System metrics at startup; new row only when something actually changed.
    std::string hardwareJson, fingerprint;
    CollectHardwareStatistics(hardwareJson, fingerprint);
    StoreHardwareIfChanged(db, hardwareJson, fingerprint);

    std::string lastUploadStr = MetaGet(db, "lastUsageUploadEpoch");
    long long lastUsageUploadEpoch = lastUploadStr.empty() ? 0 : atoll(lastUploadStr.c_str());
    long long lastAttemptEpoch = 0;

    UsageDelta interval{};
    interval.intervalStartEpoch = _time64(nullptr);

    while (!shutdownSignal.load(std::memory_order_relaxed)) {
        Sleep(1000);
        interval.openSeconds++;
        if (ApplicationHasFocus()) interval.focusSeconds++;

        long long now = _time64(nullptr);
        bool intervalComplete = now - interval.intervalStartEpoch >= kUsageIntervalSeconds;
        if (intervalComplete) {
            interval.intervalSeconds = (int)(now - interval.intervalStartEpoch);
            interval.leftClicks = g_statLeftClicks.exchange(0);
            interval.middleClicks = g_statMiddleClicks.exchange(0);
            interval.rightClicks = g_statRightClicks.exchange(0);
            interval.keyPresses = g_statKeyPresses.exchange(0);
            interval.ribbonActionsJson = DrainRibbonActionsJson();
            StoreUsageRow(db, interval);
            interval = UsageDelta{};
            interval.intervalStartEpoch = now;
        }

        // Upload policy: anything pending its first contact (hardware + installation public
        // key) is retried every minute; the accumulated usage logs go once per 24 hours.
        bool hardwarePending = false;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT 1 FROM HardwareStatistics WHERE uploadedUtc IS NULL"
                                   " LIMIT 1;", -1, &stmt, nullptr) == SQLITE_OK)
            hardwarePending = sqlite3_step(stmt) == SQLITE_ROW;
        sqlite3_finalize(stmt);

        bool usageDue = now - lastUsageUploadEpoch >= kUsageUploadIntervalSeconds;
        bool attemptDue = now - lastAttemptEpoch >= kPendingUploadRetrySeconds;
        if ((hardwarePending && attemptDue) || (usageDue && attemptDue)) {
            lastAttemptEpoch = now;
            if (TryUpload(db, usageDue) && usageDue) {
                lastUsageUploadEpoch = now;
                MetaSet(db, "lastUsageUploadEpoch", std::to_string(now));
            }
        }
    }

    // Final partial interval so the last few minutes of a session are not lost.
    long long now = _time64(nullptr);
    interval.intervalSeconds = (int)(now - interval.intervalStartEpoch);
    if (interval.intervalSeconds > 0) {
        interval.leftClicks = g_statLeftClicks.exchange(0);
        interval.middleClicks = g_statMiddleClicks.exchange(0);
        interval.rightClicks = g_statRightClicks.exchange(0);
        interval.keyPresses = g_statKeyPresses.exchange(0);
        interval.ribbonActionsJson = DrainRibbonActionsJson();
        StoreUsageRow(db, interval);
    }
    sqlite3_close(db);
}
