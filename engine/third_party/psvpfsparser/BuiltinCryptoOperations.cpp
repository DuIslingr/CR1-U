#include "BuiltinCryptoOperations.h"

#include <cstring>
#include <vector>

namespace {

// ---- AES-128 (FIPS-197), the tables built once: the S-box from GF(2^8)'s inverses, the
// round's four T tables (SubBytes + MixColumns) and their inverses

uint8_t xtime(uint8_t x) { return uint8_t((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
uint8_t gmul(uint8_t a, uint8_t b) {
   uint8_t p = 0;
   for (int i = 0; i < 8; ++i, b >>= 1) {
      if (b & 1) p ^= a;
      a = xtime(a);
   }
   return p;
}

struct Tables {
   uint8_t sbox[256], inv[256];
   uint32_t te[4][256], td[4][256];
   Tables() {
      uint8_t p = 1, q = 1; // (p walks the multiplicative group by 3, q by its inverse)
      do {
         p = uint8_t(p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0));
         q ^= uint8_t(q << 1);
         q ^= uint8_t(q << 2);
         q ^= uint8_t(q << 4);
         if (q & 0x80) q ^= 0x09;
         const uint8_t x = uint8_t(q ^ (q << 1 | q >> 7) ^ (q << 2 | q >> 6) ^ (q << 3 | q >> 5) ^ (q << 4 | q >> 4));
         sbox[p] = x ^ 0x63;
      } while (p != 1);
      sbox[0] = 0x63;
      for (int i = 0; i < 256; ++i) inv[sbox[i]] = uint8_t(i);
      for (int i = 0; i < 256; ++i) {
         const uint8_t s = sbox[i], s2 = xtime(s), s3 = uint8_t(s2 ^ s);
         const uint32_t e = uint32_t(s2) << 24 | uint32_t(s) << 16 | uint32_t(s) << 8 | s3;
         const uint8_t v = inv[i];
         const uint32_t d = uint32_t(gmul(v, 14)) << 24 | uint32_t(gmul(v, 9)) << 16 | uint32_t(gmul(v, 13)) << 8 | gmul(v, 11);
         for (int k = 0; k < 4; ++k) te[k][i] = e >> (8 * k) | e << (32 - 8 * k), td[k][i] = d >> (8 * k) | d << (32 - 8 * k);
      }
   }
};
const Tables& tables() {
   static const Tables t;
   return t;
}

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }

// AES-128 or AES-256 (a SELF's metadata key is 256 bits)
struct Aes {
   int rounds;
   uint32_t ek[60], dk[60];
   explicit Aes(const uint8_t* key, int bits = 128) : rounds(bits == 256 ? 14 : 10) {
      const Tables& t = tables();
      static const uint8_t rcon[10] = {1, 2, 4, 8, 16, 32, 64, 128, 0x1b, 0x36};
      const int nk = bits == 256 ? 8 : 4, words = 4 * (rounds + 1);
      auto sub = [&](uint32_t w) { return uint32_t(t.sbox[w >> 24]) << 24 | uint32_t(t.sbox[(w >> 16) & 0xff]) << 16 | uint32_t(t.sbox[(w >> 8) & 0xff]) << 8 | t.sbox[w & 0xff]; };
      for (int i = 0; i < nk; ++i) ek[i] = be32(key + 4 * i);
      for (int i = nk; i < words; ++i) {
         uint32_t w = ek[i - 1];
         if (i % nk == 0) w = sub(w << 8 | w >> 24) ^ (uint32_t(rcon[i / nk - 1]) << 24);
         else if (nk > 6 && i % nk == 4) w = sub(w);
         ek[i] = ek[i - nk] ^ w;
      }
      // (the equivalent inverse cipher: the round keys in reverse, InvMixColumns over the middle ones)
      for (int r = 0; r <= rounds; ++r)
         for (int c = 0; c < 4; ++c) {
            const uint32_t w = ek[(rounds - r) * 4 + c];
            dk[r * 4 + c] = (r == 0 || r == rounds) ? w
                                                    : t.td[0][t.sbox[w >> 24]] ^ t.td[1][t.sbox[(w >> 16) & 0xff]] ^ t.td[2][t.sbox[(w >> 8) & 0xff]] ^ t.td[3][t.sbox[w & 0xff]];
         }
   }
   void encrypt(const uint8_t* in, uint8_t* out) const {
      const Tables& t = tables();
      uint32_t s0 = be32(in) ^ ek[0], s1 = be32(in + 4) ^ ek[1], s2 = be32(in + 8) ^ ek[2], s3 = be32(in + 12) ^ ek[3];
      for (int r = 1; r < rounds; ++r) {
         const uint32_t* k = ek + 4 * r;
         const uint32_t t0 = t.te[0][s0 >> 24] ^ t.te[1][(s1 >> 16) & 0xff] ^ t.te[2][(s2 >> 8) & 0xff] ^ t.te[3][s3 & 0xff] ^ k[0];
         const uint32_t t1 = t.te[0][s1 >> 24] ^ t.te[1][(s2 >> 16) & 0xff] ^ t.te[2][(s3 >> 8) & 0xff] ^ t.te[3][s0 & 0xff] ^ k[1];
         const uint32_t t2 = t.te[0][s2 >> 24] ^ t.te[1][(s3 >> 16) & 0xff] ^ t.te[2][(s0 >> 8) & 0xff] ^ t.te[3][s1 & 0xff] ^ k[2];
         const uint32_t t3 = t.te[0][s3 >> 24] ^ t.te[1][(s0 >> 16) & 0xff] ^ t.te[2][(s1 >> 8) & 0xff] ^ t.te[3][s2 & 0xff] ^ k[3];
         s0 = t0, s1 = t1, s2 = t2, s3 = t3;
      }
      const uint8_t* S = t.sbox;
      auto last = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t k) {
         return (uint32_t(S[a >> 24]) << 24 | uint32_t(S[(b >> 16) & 0xff]) << 16 | uint32_t(S[(c >> 8) & 0xff]) << 8 | S[d & 0xff]) ^ k;
      };
      const uint32_t* k = ek + 4 * rounds;
      put32(out, last(s0, s1, s2, s3, k[0]));
      put32(out + 4, last(s1, s2, s3, s0, k[1]));
      put32(out + 8, last(s2, s3, s0, s1, k[2]));
      put32(out + 12, last(s3, s0, s1, s2, k[3]));
   }
   void decrypt(const uint8_t* in, uint8_t* out) const {
      const Tables& t = tables();
      uint32_t s0 = be32(in) ^ dk[0], s1 = be32(in + 4) ^ dk[1], s2 = be32(in + 8) ^ dk[2], s3 = be32(in + 12) ^ dk[3];
      for (int r = 1; r < rounds; ++r) {
         const uint32_t* k = dk + 4 * r;
         const uint32_t t0 = t.td[0][s0 >> 24] ^ t.td[1][(s3 >> 16) & 0xff] ^ t.td[2][(s2 >> 8) & 0xff] ^ t.td[3][s1 & 0xff] ^ k[0];
         const uint32_t t1 = t.td[0][s1 >> 24] ^ t.td[1][(s0 >> 16) & 0xff] ^ t.td[2][(s3 >> 8) & 0xff] ^ t.td[3][s2 & 0xff] ^ k[1];
         const uint32_t t2 = t.td[0][s2 >> 24] ^ t.td[1][(s1 >> 16) & 0xff] ^ t.td[2][(s0 >> 8) & 0xff] ^ t.td[3][s3 & 0xff] ^ k[2];
         const uint32_t t3 = t.td[0][s3 >> 24] ^ t.td[1][(s2 >> 16) & 0xff] ^ t.td[2][(s1 >> 8) & 0xff] ^ t.td[3][s0 & 0xff] ^ k[3];
         s0 = t0, s1 = t1, s2 = t2, s3 = t3;
      }
      const uint8_t* I = t.inv;
      auto last = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t k) {
         return (uint32_t(I[a >> 24]) << 24 | uint32_t(I[(b >> 16) & 0xff]) << 16 | uint32_t(I[(c >> 8) & 0xff]) << 8 | I[d & 0xff]) ^ k;
      };
      const uint32_t* k = dk + 4 * rounds;
      put32(out, last(s0, s3, s2, s1, k[0]));
      put32(out + 4, last(s1, s0, s3, s2, k[1]));
      put32(out + 8, last(s2, s1, s0, s3, k[2]));
      put32(out + 12, last(s3, s2, s1, s0, k[3]));
   }
};

// ---- SHA-1 and SHA-256 (FIPS 180-4)

uint32_t rol(uint32_t x, int n) { return x << n | x >> (32 - n); }
uint32_t ror(uint32_t x, int n) { return x >> n | x << (32 - n); }

// the message padded: its bits' count, big-endian, at the end of the last 64-byte block
std::vector<uint8_t> padded(const uint8_t* src, size_t size) {
   std::vector<uint8_t> m(src, src + size);
   m.push_back(0x80);
   while (m.size() % 64 != 56) m.push_back(0);
   const uint64_t bits = uint64_t(size) * 8;
   for (int i = 7; i >= 0; --i) m.push_back(uint8_t(bits >> (8 * i)));
   return m;
}

void sha1Of(const uint8_t* src, size_t size, uint8_t* out) {
   uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
   const std::vector<uint8_t> m = padded(src, size);
   for (size_t b = 0; b < m.size(); b += 64) {
      uint32_t w[80];
      for (int i = 0; i < 16; ++i) w[i] = be32(&m[b + 4 * size_t(i)]);
      for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
      uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
      for (int i = 0; i < 80; ++i) {
         const uint32_t f = i < 20 ? (bb & c) | (~bb & d) : i < 40 ? bb ^ c ^ d : i < 60 ? (bb & c) | (bb & d) | (c & d) : bb ^ c ^ d;
         const uint32_t k = i < 20 ? 0x5a827999 : i < 40 ? 0x6ed9eba1 : i < 60 ? 0x8f1bbcdc : 0xca62c1d6;
         const uint32_t t = rol(a, 5) + f + e + k + w[i];
         e = d, d = c, c = rol(bb, 30), bb = a, a = t;
      }
      h[0] += a, h[1] += bb, h[2] += c, h[3] += d, h[4] += e;
   }
   for (int i = 0; i < 5; ++i) put32(out + 4 * i, h[i]);
}

void sha256Of(const uint8_t* src, size_t size, uint8_t* out) {
   static const uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
      0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
      0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
      0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
      0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
      0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
   uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
   const std::vector<uint8_t> m = padded(src, size);
   for (size_t b = 0; b < m.size(); b += 64) {
      uint32_t w[64];
      for (int i = 0; i < 16; ++i) w[i] = be32(&m[b + 4 * size_t(i)]);
      for (int i = 16; i < 64; ++i) {
         const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
         w[i] = w[i - 16] + s0 + w[i - 7] + s1;
      }
      uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
      for (int i = 0; i < 64; ++i) {
         const uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
         const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & bb) ^ (a & c) ^ (bb & c));
         hh = g, g = f, f = e, e = d + t1, d = c, c = bb, bb = a, a = t1 + t2;
      }
      h[0] += a, h[1] += bb, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
   }
   for (int i = 0; i < 8; ++i) put32(out + 4 * i, h[i]);
}

// HMAC (RFC 2104) over a 64-byte block hash
template <typename Hash>
void hmacOf(Hash hash, size_t outLen, const uint8_t* src, size_t size, const uint8_t* key, size_t keyLen, uint8_t* out) {
   uint8_t k[64] = {};
   if (keyLen > 64) hash(key, keyLen, k);
   else std::memcpy(k, key, keyLen);
   std::vector<uint8_t> inner(64 + size), outer(64 + outLen);
   for (int i = 0; i < 64; ++i) inner[size_t(i)] = k[i] ^ 0x36, outer[size_t(i)] = k[i] ^ 0x5c;
   if (size) std::memcpy(&inner[64], src, size);
   hash(inner.data(), inner.size(), &outer[64]);
   hash(outer.data(), outer.size(), out);
}

} // namespace

int BuiltinCryptoOperations::aes_cbc_encrypt(const unsigned char* src, unsigned char* dst, int size, const unsigned char* key, int key_size, unsigned char* iv) const {
   if ((key_size != 128 && key_size != 256) || size % 16) return -1; // (256: a SELF's metadata)
   const Aes aes(key, key_size);
   uint8_t chain[16];
   std::memcpy(chain, iv, 16);
   for (int b = 0; b < size; b += 16) {
      uint8_t x[16];
      for (int i = 0; i < 16; ++i) x[i] = src[b + i] ^ chain[i];
      aes.encrypt(x, dst + b);
      std::memcpy(chain, dst + b, 16);
   }
   if (size) std::memcpy(iv, chain, 16); // (the new IV: the last block out)
   return 0;
}

int BuiltinCryptoOperations::aes_cbc_decrypt(const unsigned char* src, unsigned char* dst, int size, const unsigned char* key, int key_size, unsigned char* iv) const {
   if ((key_size != 128 && key_size != 256) || size % 16) return -1; // (256: a SELF's metadata)
   const Aes aes(key, key_size);
   uint8_t chain[16];
   std::memcpy(chain, iv, 16);
   for (int b = 0; b < size; b += 16) {
      uint8_t in[16], x[16];
      std::memcpy(in, src + b, 16); // (src and dst may be one buffer)
      aes.decrypt(in, x);
      for (int i = 0; i < 16; ++i) dst[b + i] = x[i] ^ chain[i];
      std::memcpy(chain, in, 16);
   }
   if (size) std::memcpy(iv, chain, 16); // (the new IV: the last block in)
   return 0;
}

int BuiltinCryptoOperations::aes_ctr_encrypt(const unsigned char* src, unsigned char* dst, int size, const unsigned char* key, int key_size, unsigned char* iv) {
   if (key_size != 128) return -1;
   const Aes aes(key);
   uint8_t counter[16], stream[16];
   std::memcpy(counter, iv, 16);
   for (int b = 0; b < size; b += 16) {
      aes.encrypt(counter, stream);
      for (int i = 0; i < 16 && b + i < size; ++i) dst[b + i] = src[b + i] ^ stream[i];
      for (int i = 15; i >= 0 && ++counter[i] == 0; --i) {} // (128-bit big-endian increment)
   }
   // (the IV moved on by the whole blocks done, as OpenSSLCryptoOperations::aes_ctr)
   uint64_t add = uint64_t(size / 16);
   for (int i = 15; i >= 0; --i) {
      add += iv[i];
      iv[i] = uint8_t(add);
      add >>= 8;
   }
   return 0;
}

int BuiltinCryptoOperations::aes_ctr_decrypt(const unsigned char* src, unsigned char* dst, int size, const unsigned char* key, int key_size, unsigned char* iv) {
   return aes_ctr_encrypt(src, dst, size, key, key_size, iv);
}

int BuiltinCryptoOperations::aes_ecb_encrypt(const unsigned char* src, unsigned char* dst, int size, const unsigned char* key, int key_size) const {
   if (key_size != 128 || size % 16) return -1;
   const Aes aes(key);
   for (int b = 0; b < size; b += 16) {
      uint8_t in[16];
      std::memcpy(in, src + b, 16);
      aes.encrypt(in, dst + b);
   }
   return 0;
}

int BuiltinCryptoOperations::aes_ecb_decrypt(const unsigned char* src, unsigned char* dst, int size, const unsigned char* key, int key_size) const {
   if (key_size != 128 || size % 16) return -1;
   const Aes aes(key);
   for (int b = 0; b < size; b += 16) {
      uint8_t in[16];
      std::memcpy(in, src + b, 16);
      aes.decrypt(in, dst + b);
   }
   return 0;
}

// AES-CMAC (RFC 4493)
int BuiltinCryptoOperations::aes_cmac(const unsigned char* src, unsigned char* dst, int size, const unsigned char* key, int key_size) const {
   if (key_size != 128) return -1;
   const Aes aes(key);
   uint8_t l[16] = {}, k1[16], k2[16];
   aes.encrypt(l, l);
   auto dbl = [](const uint8_t* in, uint8_t* out) {
      for (int i = 0; i < 16; ++i) out[i] = uint8_t(in[i] << 1 | (i < 15 ? in[i + 1] >> 7 : 0));
      if (in[0] & 0x80) out[15] ^= 0x87;
   };
   dbl(l, k1);
   dbl(k1, k2);
   const int n = size == 0 ? 1 : (size + 15) / 16;
   const bool whole = size > 0 && size % 16 == 0;
   uint8_t x[16] = {}, last[16] = {};
   for (int i = 0; i < 16; ++i) {
      const int at = (n - 1) * 16 + i;
      if (whole) last[i] = src[at] ^ k1[i];
      else last[i] = uint8_t((at < size ? src[at] : at == size ? 0x80 : 0) ^ k2[i]);
   }
   for (int b = 0; b < n - 1; ++b) {
      for (int i = 0; i < 16; ++i) x[i] ^= src[b * 16 + i];
      aes.encrypt(x, x);
   }
   for (int i = 0; i < 16; ++i) x[i] ^= last[i];
   aes.encrypt(x, dst);
   return 0;
}

int BuiltinCryptoOperations::sha1(const unsigned char* src, unsigned char* dst, int size) const {
   sha1Of(src, size_t(size), dst);
   return 0;
}

int BuiltinCryptoOperations::sha256(const unsigned char* src, unsigned char* dst, int size) const {
   sha256Of(src, size_t(size), dst);
   return 0;
}

int BuiltinCryptoOperations::hmac_sha1(const unsigned char* src, unsigned char* dst, int size, const unsigned char* key, int key_size) const {
   hmacOf(sha1Of, 20, src, size_t(size), key, size_t(key_size), dst);
   return 0;
}

int BuiltinCryptoOperations::hmac_sha256(const unsigned char* src, unsigned char* dst, int size, const unsigned char* key, int key_size) const {
   hmacOf(sha256Of, 32, src, size_t(size), key, size_t(key_size), dst);
   return 0;
}
