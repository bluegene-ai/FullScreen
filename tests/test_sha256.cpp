// ============================================================
// test_sha256.cpp - runnable self-check for Crypto::Sha256Hex /
// Sha256FileHex against known vectors (the update verification core).
//
// Build (VS2022 x64 dev prompt):
//   cl /nologo /W4 /EHsc /std:c++17 test_sha256.cpp /Fe:test_sha256.exe
// ============================================================
#include "../src/crypto.cpp"
#include <cstdio>
#include <string>
#include <fstream>

static int failures = 0;

static void Expect(bool cond, const char* what)
{
    std::printf("%-64s [%s]\n", what, cond ? "PASS" : "FAIL");
    if (!cond) failures++;
}

int main()
{
    std::printf("SHA-256 self-check\n\n");

    // Known vectors.
    Expect(Crypto::Sha256Hex("abc", 3) ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
           "Sha256Hex(\"abc\")");

    Expect(Crypto::Sha256Hex("", 0) ==
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
           "Sha256Hex(\"\")");

    const std::string hello = "hello world";
    Expect(Crypto::Sha256Hex(hello.data(), hello.size()) ==
           "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9",
           "Sha256Hex(\"hello world\")");

    // Buffer vs file-stream hashing must agree.
    {
        const char* path = "sha256_selfcheck.tmp";
        { std::ofstream f(path, std::ios::binary); f.write(hello.data(), (std::streamsize)hello.size()); }
        std::string fileHex;
        bool ok = Crypto::Sha256FileHex(L"sha256_selfcheck.tmp", fileHex);
        std::remove(path);
        Expect(ok && fileHex ==
               "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9",
               "Sha256FileHex(file) matches buffer hash");
    }

    std::printf("\n%s (%d failures)\n", failures == 0 ? "ALL PASS" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
