#pragma once
// The crypto the setup needs (a Vita dump's PFS, a Vita program's SELF), built in - no OpenSSL:
// AES-128 / AES-256 (ECB, CBC, CTR, CMAC), SHA-1, SHA-256 and their HMACs. Written for the Chaos
// Rings ports (GPL-2.0-or-later). A CBC or CTR call leaves `iv` where the next call goes on: CBC
// the last ciphertext block, CTR the counter moved on by the whole blocks done.
//
// AES key sizes are in bits (128, or 256 for CBC), HMAC key sizes in bytes. ECB and CBC take whole
// 16-byte blocks only. Each returns false (writing nothing) for a size or key size it can't take.

#include <cstddef>
#include <cstdint>

namespace cr::crypto {

bool aesEcbEncrypt(const uint8_t* src, uint8_t* dst, size_t size, const uint8_t* key, int keyBits = 128);
bool aesEcbDecrypt(const uint8_t* src, uint8_t* dst, size_t size, const uint8_t* key, int keyBits = 128);
bool aesCbcEncrypt(const uint8_t* src, uint8_t* dst, size_t size, const uint8_t* key, int keyBits, uint8_t* iv);
bool aesCbcDecrypt(const uint8_t* src, uint8_t* dst, size_t size, const uint8_t* key, int keyBits, uint8_t* iv);
bool aesCtr(const uint8_t* src, uint8_t* dst, size_t size, const uint8_t* key, int keyBits, uint8_t* iv); // (encrypts and decrypts)
bool aesCmac(const uint8_t* src, size_t size, const uint8_t* key, int keyBits, uint8_t* mac16);

void sha1(const uint8_t* src, size_t size, uint8_t* digest20);
void sha256(const uint8_t* src, size_t size, uint8_t* digest32);
void hmacSha1(const uint8_t* src, size_t size, const uint8_t* key, size_t keyBytes, uint8_t* mac20);
void hmacSha256(const uint8_t* src, size_t size, const uint8_t* key, size_t keyBytes, uint8_t* mac32);

} // namespace cr::crypto
