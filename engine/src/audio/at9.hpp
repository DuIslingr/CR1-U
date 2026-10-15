#pragma once
// ATRAC9 (.at9, the Vita release's voices and music): a RIFF WAVE whose 'fmt ' is
// WAVE_FORMAT_EXTENSIBLE (0xFFFE) with the ATRAC9 sub-format GUID and four bytes of decoder
// config; 'fact' holds the sample count and the encoder delay (samples to drop at the start);
// 'smpl' (music) the loop, start and end inclusive, counted after that delay; 'data' is
// superframes of the fmt's block-align bytes, each several frames. Decoded with LibAtrac9
// (third_party/libatrac9, MIT). See docs/FORMATS.md ("Later releases").

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cr {

struct At9File {
    int rate = 48000, channels = 1;
    int superframeBytes = 0;
    uint8_t config[4] = {};
    int64_t samples = 0;             // 'fact': frames after the delay
    int delay = 0;                   // 'fact': the encoder delay
    int64_t loopStart = -1, loopEnd = -1; // 'smpl', after the delay (end inclusive); -1: none
    const uint8_t* data = nullptr;   // the superframes (inside the file's buffer)
    size_t dataSize = 0;
};
bool parseAt9(const std::vector<uint8_t>& file, At9File& out, std::string* error = nullptr);

// Decodes superframe by superframe (frames overlap, so a seek restarts a superframe early).
class At9Decoder {
public:
    At9Decoder();
    ~At9Decoder();
    bool open(const At9File& f); // false: built without LibAtrac9 or a bad config
    int superframeSamples() const { return superframeSamples_; }
    // Superframe i as interleaved s16 (superframeSamples() frames); false past the end or on an error
    bool decode(size_t i, std::vector<int16_t>& pcm);
    size_t superframes() const;

private:
    void* handle_ = nullptr;
    const At9File* file_ = nullptr;
    int superframeSamples_ = 0, frames_ = 0, frameSamples_ = 0;
};

// A whole file as interleaved s16, the delay dropped and cut to its length (voices)
bool decodeAt9(const std::vector<uint8_t>& file, std::vector<int16_t>& pcm, int& rate, int& channels,
               std::string* error = nullptr);

} // namespace cr
