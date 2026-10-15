#pragma once
// The Vita release's sound effects: sound/sndeff.bnk, a Sony SCREAM bank (version 3 header,
// "SBlk" block version 9). Header: u32 version, u32 sections, then {offset, size} of the SBlk
// block (0x08) and of the wave data (0x10). SBlk: u16 counts at +0x16 (sounds), +0x18 (grains),
// +0x1a (waves); offsets (from the block) at +0x1c the sounds (12 bytes: u8 volume, .., u8 grain
// count at +4, u32 first grain's offset at +8), +0x20 the grains (8 bytes: u32 type << 16 |
// wave header offset; 0x0100 = play a wave), +0x34 the wave headers (u8 centre note at +2, fine
// at +3 - 0xc4 + 0 = 48 kHz, a semitone a note, 128 fine steps -, u16 flags at +0x0e, u32 data
// offset at +0x10 and size at +0x14). A wave: u32 subtype (0 mono, 3 stereo), u32 0x18, then
// HE-VAG from +0x20 - 16-byte frames (stereo: one frame each in turn), 28 samples a frame. The
// 253 sounds are sndeff.bin's 253 entries in order (same lengths and rates). See docs/FORMATS.md.

#include <cstdint>
#include <string>
#include <vector>

#include "audio/sound.hpp"

namespace cr {

// The bank's sounds as 16-bit PCM, by sound number (an empty one where a sound has no wave)
bool parseScreamBank(const std::vector<uint8_t>& file, std::vector<PcmSound>& out, std::string* error = nullptr);

// HE-VAG (also plain VAG: its filters are the first 29) to s16, `channels` interleaved by frame
void decodeHevag(const uint8_t* data, size_t size, int channels, std::vector<int16_t>& out);

} // namespace cr
