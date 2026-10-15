#pragma once
// Field music: bgmNN.caf (AAC-LC in CAF, 44.1 kHz stereo, ~128 kb/s) decoded with FAAD2 and
// streamed through SDL audio, looping from the game's _BGM_REPEAT packet. _bgmgo semantics.
// With a Vita source (setVitaSource), the Vita's cr_mNN.at9 instead (ATRAC9, 48 kHz stereo,
// 144 kb/s: the same tracks but bgm31, looping sample-exactly between its 'smpl' points - the
// files run on past the loop's end into an ending). Without either decoder everything is a
// silent no-op. See docs/FORMATS.md ("Music").

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cr {

// The AAC side of a CAF file: decoder config and packets.
struct CafAac {
    int rate = 44100, channels = 2, framesPerPacket = 1024;
    std::vector<uint8_t> config;      // AudioSpecificConfig (from the 'kuki' ESDS)
    std::vector<uint32_t> offset, size; // packets within `data`
    int64_t validFrames = 0;          // 'pakt': frames after priming and remainder are removed
    int priming = 0, remainder = 0;
    std::vector<uint8_t> data;
};
bool parseCafAac(const std::vector<uint8_t>& file, CafAac& out, std::string* error = nullptr);
// Decodes music track `t` (0..29, _BGM_FILE) whole: its frames and an FNV-1a hash of the s16 PCM
// (tests: the system and the bundled FAAD2 builds should agree); false without a decoder
bool decodeTrack(const std::string& dataDir, int t, int64_t& frames, uint64_t& hash, std::string* error = nullptr);
constexpr int kBgmTracks = 30;

class MusicPlayer {
public:
    MusicPlayer();
    ~MusicPlayer();
    bool init(const std::string& dataDir); // false: no decoder or no audio device
    // _bgmgo(v): bit 29 stop now; bits 25 / 26 / 27 fade out over 40 / 10 / 20 frames and stop;
    // otherwise play track v & 0xff (0..29, _BGM_FILE), unless it is already playing.
    void command(uint32_t v);
    void update(float dt);   // decode ahead, fades
    int current() const { return track_; }
    void setVolume(float v) { volume_ = v; }
    // Reads a file of the Vita's bgm.psarc by name; empty: the data folder's tracks only
    using VitaReader = std::function<bool(const std::string& name, std::vector<uint8_t>& out)>;
    void setVitaSource(VitaReader read) { vita_ = std::move(read); }
    bool playingVita() const; // the current track is the Vita's

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string dataDir_;
    VitaReader vita_;
    int track_ = -1;
    float volume_ = 0.8f, fade_ = 1.0f, fadeStep_ = 0.0f;
};

} // namespace cr
