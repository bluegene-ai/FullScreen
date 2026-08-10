#pragma once
#include "app_common.h"
#include <string>

// ============================================================
// CryptoLayer - XOR + Base64 password obfuscation
// ============================================================

namespace Crypto {

// Encrypt plain password 鈫?Base64-encoded XOR result
std::wstring Encrypt(std::wstring_view plain);

// Decrypt Base64-encoded XOR result 鈫?plain password
std::wstring Decrypt(std::wstring_view encrypted);

// SHA-256 hex digest (lowercase) of a raw buffer. Empty on failure.
std::string Sha256Hex(const void* data, size_t len);

// SHA-256 hex digest (lowercase) of a file's contents, streamed in chunks.
// Returns false and clears hexOut on failure.
bool Sha256FileHex(const std::wstring& path, std::string& hexOut);

} // namespace Crypto
