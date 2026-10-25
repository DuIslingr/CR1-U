# psvpfsparser (the Vita's PFS, decrypted)

From Vita3K's fork, https://github.com/Vita3K/psvpfsparser (commit 05d43a06), itself from
motoharu-gosuto's psvpfstools - the part of Vita3K (GPL-2.0) that decrypts a NoNpDrm dump's
files when it installs one. Used by the first start's setup (viewer/setup.cpp) to decrypt a
Vita dump given as a folder or an archive. GPL-2.0, as Vita3K.

Changed here:

- `BuiltinCryptoOperations.{h,cpp}` (new, written for the ports): AES-128 ECB / CBC / CTR /
  CMAC, SHA-1, SHA-256 and their HMACs in place of `OpenSSLCryptoOperations` - no OpenSSL; the same
  results (checked against it on random inputs, and on a whole dump: identical files).
- The parsers keep their title folder's path by value, not a reference (a caller's temporary
  left them reading freed memory).
- `Utils.cpp` upper-cases without Boost.
- Left out: `OpenSSLCryptoOperations`, `CryptoOperationsFactory`, the file-based F00D key
  encryptor and its factory, save data (`PfsSaveData`), the command line's zRIF handling
  (`PsvPfsParserConfig`, `rif2zrif`, `zrif2rif`) - the klicensee is read from the dump's own
  `sce_sys/package/work.bin` (+0x50).
