// Copyright (c) 2026-Present : Ram Shanker: All rights reserved.

// Installation + session Ed25519 identity keys. See AccountManager.h for the design.
// Compiled into both Vishwakarma.exe and VishwakarmaSetup.exe (VISHWAKARMA_INSTALLER).

#include "AccountManager.h"

#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>

namespace fs = std::filesystem;

namespace {

// %LOCALAPPDATA%\Mission Vishwakarma\Credentials : same base folder as SoftwareUpdate.cpp.
fs::path CredentialsDir() {
    PWSTR p = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) result = p;
    if (p) CoTaskMemFree(p);
    return result / L"Mission Vishwakarma" / L"Credentials";
}

fs::path InstallationKeyPath() { return CredentialsDir() / L"InstallationKey.pem"; }
fs::path InstallationPubPath() { return CredentialsDir() / L"InstallationKey.pub"; }

std::string Base64Encode(const unsigned char* data, size_t size) {
    std::string out(4 * ((size + 2) / 3) + 1, '\0');
    int n = EVP_EncodeBlock((unsigned char*)out.data(), data, (int)size);
    if (n <= 0) return "";
    out.resize(n);
    return out;
}

// In-memory state, initialized once per process by InitializeOnLaunch().
std::mutex g_mutex;
EVP_PKEY* g_installationKey = nullptr; // Loaded private key.
EVP_PKEY* g_sessionKey = nullptr;      // Per-launch private key, never persisted.
std::string g_installationPublicB64;
std::string g_sessionPublicB64;
std::string g_sessionSignatureB64;

EVP_PKEY* GenerateEd25519() {
    EVP_PKEY* key = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (!ctx) return nullptr;
    if (EVP_PKEY_keygen_init(ctx) != 1 || EVP_PKEY_keygen(ctx, &key) != 1) key = nullptr;
    EVP_PKEY_CTX_free(ctx);
    return key;
}

bool RawPublicKeyBase64(EVP_PKEY* key, std::string& out) {
    unsigned char raw[32];
    size_t rawLen = sizeof(raw);
    if (EVP_PKEY_get_raw_public_key(key, raw, &rawLen) != 1) return false;
    out = Base64Encode(raw, rawLen);
    return !out.empty();
}

// Pure Ed25519 signature over message, base64 encoded. Empty on failure.
std::string SignBase64(EVP_PKEY* key, const unsigned char* msg, size_t msgLen) {
    std::string result;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return result;
    unsigned char sig[64];
    size_t sigLen = sizeof(sig);
    if (EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) == 1 &&
        EVP_DigestSign(ctx, sig, &sigLen, msg, msgLen) == 1)
        result = Base64Encode(sig, sigLen);
    EVP_MD_CTX_free(ctx);
    return result;
}

EVP_PKEY* LoadInstallationKeyFromDisk() {
    std::ifstream f(InstallationKeyPath(), std::ios::binary);
    if (!f) return nullptr;
    std::string pem((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    BIO* bio = BIO_new_mem_buf(pem.data(), (int)pem.size());
    if (!bio) return nullptr;
    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (key && EVP_PKEY_id(key) != EVP_PKEY_ED25519) { EVP_PKEY_free(key); key = nullptr; }
    return key;
}

bool WritePemToFile(const fs::path& path, bool isPrivate, EVP_PKEY* key) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) return false;
    bool ok = isPrivate
        ? PEM_write_bio_PKCS8PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr) == 1
        : PEM_write_bio_PUBKEY(bio, key) == 1;
    if (ok) {
        char* data = nullptr;
        long size = BIO_get_mem_data(bio, &data);
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        ok = f && size > 0;
        if (ok) { f.write(data, size); ok = f.good(); }
    }
    BIO_free(bio);
    return ok;
}

// Loads the installation key, generating and persisting a new pair when missing/corrupt.
// Caller must hold g_mutex. Returns the loaded key (owned by g_installationKey).
EVP_PKEY* EnsureInstallationKeyLocked() {
    if (g_installationKey) return g_installationKey;
    g_installationKey = LoadInstallationKeyFromDisk();
    if (!g_installationKey) {
        EVP_PKEY* key = GenerateEd25519();
        if (!key) return nullptr;
        std::error_code ec;
        fs::create_directories(CredentialsDir(), ec);
        if (!WritePemToFile(InstallationKeyPath(), true, key)) {
            EVP_PKEY_free(key);
            return nullptr;
        }
        WritePemToFile(InstallationPubPath(), false, key); // Convenience copy, best effort.
        g_installationKey = key;
    }
    if (g_installationPublicB64.empty())
        RawPublicKeyBase64(g_installationKey, g_installationPublicB64);
    return g_installationKey;
}

} // namespace

namespace AccountManager {

bool EnsureInstallationKey() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return EnsureInstallationKeyLocked() != nullptr;
}

bool InitializeOnLaunch() {
    std::lock_guard<std::mutex> lock(g_mutex);
    EVP_PKEY* installation = EnsureInstallationKeyLocked();
    if (!installation) return false;
    if (g_sessionKey) return true; // Already initialized this launch.

    EVP_PKEY* session = GenerateEd25519();
    if (!session) return false;

    unsigned char sessionRaw[32];
    size_t sessionRawLen = sizeof(sessionRaw);
    if (EVP_PKEY_get_raw_public_key(session, sessionRaw, &sessionRawLen) != 1) {
        EVP_PKEY_free(session);
        return false;
    }
    std::string signature = SignBase64(installation, sessionRaw, sessionRawLen);
    if (signature.empty()) {
        EVP_PKEY_free(session);
        return false;
    }
    g_sessionKey = session;
    g_sessionPublicB64 = Base64Encode(sessionRaw, sessionRawLen);
    g_sessionSignatureB64 = signature;
    return true;
}

std::string InstallationPublicKeyBase64() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_installationPublicB64;
}

std::string SessionPublicKeyBase64() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_sessionPublicB64;
}

std::string SessionKeySignatureBase64() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_sessionSignatureB64;
}

std::string SignWithSessionKey(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_sessionKey) return "";
    return SignBase64(g_sessionKey, (const unsigned char*)message.data(), message.size());
}

} // namespace AccountManager
