#pragma once
// The crypto psvpfsparser needs (ICryptoOperations), built in - no OpenSSL: AES-128 (ECB, CBC, CTR,
// CMAC), SHA-1, SHA-256 and their HMACs. Written for the Chaos Rings ports (GPL-2.0), the same
// behaviour as OpenSSLCryptoOperations (a CBC / CTR call leaves the IV where the next one goes on)

#include <cstdint>

#include "ICryptoOperations.h"

class BuiltinCryptoOperations : public ICryptoOperations
{
public:
   int aes_cbc_encrypt(const unsigned char* src, unsigned char* dst, int size, const unsigned char* key, int key_size, unsigned char* iv) const override;
   int aes_cbc_decrypt(const unsigned char* src, unsigned char* dst, int size, const unsigned char* key, int key_size, unsigned char* iv) const override;
   int aes_ctr_encrypt(const unsigned char* src, unsigned char* dst, int size, const unsigned char* key, int key_size, unsigned char* iv) override;
   int aes_ctr_decrypt(const unsigned char* src, unsigned char* dst, int size, const unsigned char* key, int key_size, unsigned char* iv) override;
   int aes_ecb_encrypt(const unsigned char* src, unsigned char* dst, int size, const unsigned char* key, int key_size) const override;
   int aes_ecb_decrypt(const unsigned char* src, unsigned char* dst, int size, const unsigned char* key, int key_size) const override;
   int aes_cmac(const unsigned char* src, unsigned char* dst, int size, const unsigned char* key, int key_size) const override;
   int sha1(const unsigned char* src, unsigned char* dst, int size) const override;
   int sha256(const unsigned char* src, unsigned char* dst, int size) const override;
   int hmac_sha1(const unsigned char* src, unsigned char* dst, int size, const unsigned char* key, int key_size) const override;
   int hmac_sha256(const unsigned char* src, unsigned char* dst, int size, const unsigned char* key, int key_size) const override;
};
