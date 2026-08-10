// ============================================================
// CryptoLayer - XOR + Base64 implementation
// ============================================================
#include "crypto.h"
#include <Windows.h>
#include <bcrypt.h>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "Bcrypt.lib")

namespace Crypto {

// XOR key (fixed, compiled into binary)
static const unsigned char XOR_KEY[] = {
    0x5A, 0x3F, 0x8C, 0x12, 0xE7, 0x4B, 0x9D, 0x26,
    0xF1, 0x08, 0x73, 0xDA, 0x45, 0xBC, 0x2E, 0x91
};

// Base64 encoding table
static const wchar_t B64_TABLE[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// ============================================================
// Base64 encode bytes -> wide string
// ============================================================
static std::wstring Base64Encode(const std::vector<unsigned char>& data)
{
    std::wstring result;
    result.reserve(((data.size() + 2) / 3) * 4);

    for (size_t i = 0; i < data.size(); i += 3) {
        unsigned int val = (unsigned int)(data[i]) << 16;
        if (i + 1 < data.size()) val |= (unsigned int)(data[i + 1]) << 8;
        if (i + 2 < data.size()) val |= (unsigned int)(data[i + 2]);

        result.push_back(B64_TABLE[(val >> 18) & 0x3F]);
        result.push_back(B64_TABLE[(val >> 12) & 0x3F]);

        if (i + 1 < data.size())
            result.push_back(B64_TABLE[(val >> 6) & 0x3F]);
        else
            result.push_back(L'=');

        if (i + 2 < data.size())
            result.push_back(B64_TABLE[val & 0x3F]);
        else
            result.push_back(L'=');
    }
    return result;
}

// ============================================================
// Base64 decode wide string -> bytes
// ============================================================
static std::vector<unsigned char> Base64Decode(const std::wstring& input)
{
    // Build reverse lookup
    static int lookup[256] = {};
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) lookup[i] = -1;
        for (int i = 0; i < 64; ++i) lookup[(unsigned char)B64_TABLE[i]] = i;
        init = true;
    }

    std::vector<unsigned char> result;
    result.reserve((input.size() / 4) * 3);

    int val = 0, valb = -8;
    for (wchar_t ch : input) {
        if (ch == L'=') break;
        int idx = (ch < 256) ? lookup[(unsigned char)ch] : -1;
        if (idx == -1) continue;
        val = (val << 6) + idx;
        valb += 6;
        if (valb >= 0) {
            result.push_back((unsigned char)((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return result;
}

// ============================================================
// XOR obfuscation
// ============================================================
static std::vector<unsigned char> XorData(const std::vector<unsigned char>& data)
{
    std::vector<unsigned char> result = data;
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] ^= XOR_KEY[i % sizeof(XOR_KEY)];
    }
    return result;
}

// ============================================================
// Public API
// ============================================================

std::wstring Encrypt(std::wstring_view plain)
{
    if (plain.empty()) return L"";

    // Convert wstring -> UTF-8 bytes
    int len = WideCharToMultiByte(CP_UTF8, 0, plain.data(), (int)plain.size(),
                                   nullptr, 0, nullptr, nullptr);
    if (len <= 0) return L"";

    std::vector<unsigned char> bytes(len);
    WideCharToMultiByte(CP_UTF8, 0, plain.data(), (int)plain.size(),
                        (LPSTR)bytes.data(), len, nullptr, nullptr);

    // XOR
    bytes = XorData(bytes);

    // Base64 encode
    return Base64Encode(bytes);
}

std::wstring Decrypt(std::wstring_view encrypted)
{
    if (encrypted.empty()) return L"";

    // Base64 decode
    std::vector<unsigned char> bytes = Base64Decode(std::wstring(encrypted));

    if (bytes.empty()) return L"";

    // XOR
    bytes = XorData(bytes);

    // Convert UTF-8 bytes -> wstring
    int len = MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)bytes.data(), (int)bytes.size(),
                                   nullptr, 0);
    if (len <= 0) return L"";

    std::wstring result(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)bytes.data(), (int)bytes.size(),
                        result.data(), len);
    return result;
}

// ============================================================
// SHA-256 (BCrypt) - lowercase hex digest
// ============================================================
std::string Sha256Hex(const void* data, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    std::string out;

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return out;
    }
    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return out;
    }

    if (BCryptHashData(hHash, reinterpret_cast<PUCHAR>(const_cast<void*>(data)),
                       static_cast<ULONG>(len), 0) == 0) {
        BYTE hash[32] = {};
        if (BCryptFinishHash(hHash, hash, sizeof(hash), 0) == 0) {
            out.reserve(64);
            for (BYTE b : hash) {
                out.push_back(hex[(b >> 4) & 0xF]);
                out.push_back(hex[b & 0xF]);
            }
        }
    }

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return out;
}

bool Sha256FileHex(const std::wstring& path, std::string& hexOut)
{
    hexOut.clear();

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    bool ok = false;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) == 0) {
        BYTE buf[64 * 1024] = {};
        DWORD read = 0;
        ok = true;
        while (ReadFile(hFile, buf, sizeof(buf), &read, nullptr) && read > 0) {
            if (BCryptHashData(hHash, buf, read, 0) != 0) { ok = false; break; }
        }
        if (ok) {
            BYTE hash[32] = {};
            if (BCryptFinishHash(hHash, hash, sizeof(hash), 0) == 0) {
                static const char hex[] = "0123456789abcdef";
                hexOut.reserve(64);
                for (BYTE b : hash) {
                    hexOut.push_back(hex[(b >> 4) & 0xF]);
                    hexOut.push_back(hex[b & 0xF]);
                }
            } else {
                ok = false;
            }
        }
        BCryptDestroyHash(hHash);
    }
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    CloseHandle(hFile);
    return ok;
}

} // namespace Crypto
