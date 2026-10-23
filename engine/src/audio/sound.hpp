#pragma once
// Sound effects: the sndeff.bin bank (table of {u32 offset, u32 size}, 0x800 bytes; each entry
// a CAF file with linear PCM) played through SDL3 audio streams, as _effgo / _effplay do with
// OpenAL. See docs/FORMATS.md ("Sound").

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct SDL_AudioStream;

namespace cr {

struct PcmSound {
    int rate = 22050, channels = 1, bits = 16;
    bool isFloat = false, bigEndian = false;
    std::vector<uint8_t> data;
};

// Parses a CAF file holding linear PCM ('desc' + 'data' chunks).
bool parseCafPcm(const uint8_t* file, size_t size, PcmSound& out, std::string* error = nullptr);

class SoundSystem {
public:
    ~SoundSystem();
    // Opens the default playback device and loads the bank - the data's sndeff.bin, or the Vita's
    // sndeff.bnk (`vitaBank`, a path) instead when given. False (and silence) if either fails.
    bool init(const std::string& dataDir, const std::string& vitaBank = std::string());
    // Where the voice banks lie when not files of the data folder (the Android release alone:
    // inside its OBB): the file, the bank's offset and size in it. Set before init.
    using FileLocator = std::function<bool(const std::string& name, std::string& path, uint64_t& offset, uint64_t& size)>;
    void setLocator(FileLocator locate) { locate_ = std::move(locate); }
    void shutdown();
    // _effgo(id): low 12 bits = bank entry, bit 23 = loop, bit 29 = stop that sound,
    // bit 22 = the language's variant (+ language offset; English here). Bits 26 / 27 / 25 fade
    // the entry playing out over 10 / 20 / 40 frames (__eff_vol2), bits 18 / 19 / 17 in from
    // nothing (__eff_vol_fadein) - no new sound for either.
    void play(uint32_t id);
    void stop(int entry);
    void stopAll();
    void update(); // refills looping sounds, frees finished voices
    // Gains (0..1) for effects and for voice clips (the language-variant sounds, bit 22), applied
    // to playing sounds too. The mixer's master volume is already multiplied in.
    void setVolumes(float effects, float voice);
    // Bank entries that are voice clips (the game stores them among the effects and plays them
    // the same way; which ones they are is marked by the player in the sound test).
    void setVoiceEntries(const std::vector<int>& entries) { voiceEntries_ = entries; }
    bool isVoice(int entry) const;
    int entries() const { return int(bank_.size()); }

    // Voiced releases (2.2.0 iPhone, Vita): voiceevnt.bin (story lines, 2,900) and voicebtl.bin
    // (battle, 1,220) - the same {u32 offset, u32 size} table, each clip headerless signed 16-bit
    // mono PCM at 20 kHz. Clips are read from disk when played. One line plays at a time (a new
    // one replaces it), on the voice gain.
    bool hasVoices() const { return !lineTable_[0].empty() || !vitaLines_[0].empty(); }
    int voiceClips(bool battle = false) const { return int(std::max(lineTable_[battle].size(), vitaLines_[battle].size())); }
    // The Vita's voices instead (voiceevnt.psarc / voicebtl.psarc: the same clips as ATRAC9, 48 kHz
    // mono, their names sorted in clip order - `_no_silent` first): `read` gets a file by name.
    // Brought to 2.2.0's loudness (the Vita's are 1.67 / 1.51 times louder, RMS).
    using VitaReader = std::function<bool(const std::string& name, std::vector<uint8_t>& out)>;
    void setVitaLines(bool battle, std::vector<std::string> names, VitaReader read);
    bool vitaVoices() const { return !vitaLines_[0].empty(); }
    void playLine(int clip, bool battle = false);
    void stopLine();
    bool linePlaying() const;
    // Battle lines (__voicego(clip | 0x200000, tag = speaker x 1000)): one per tag at a time -
    // the speaker's last line stops first (0x200ffe) - while other speakers' play on
    void playBattleLine(int clip, int tag);
    void stopBattleLines(); // 0x20200fff
    bool battleLinePlaying() const;
    std::vector<int> battleLineLog; // the clips asked for (tests)

private:
    std::string linePath_[2];
    uint64_t lineBase_[2] = {0, 0}; // (the bank's offset in linePath_)
    FileLocator locate_;
    std::vector<std::pair<uint32_t, uint32_t>> lineTable_[2]; // per bank: {offset, size}
    SDL_AudioStream* line_ = nullptr;
    std::vector<std::pair<int, SDL_AudioStream*>> battleLines_; // {tag, stream}
    std::vector<std::string> vitaLines_[2];
    VitaReader vitaRead_[2];
    bool readClip(int bank, int clip, std::vector<uint8_t>& pcm) const;
    // A clip's s16 mono PCM and rate, from the Vita's files when set, else the data's banks
    bool clipAudio(int bank, int clip, std::vector<uint8_t>& pcm, int& rate) const;
    // (a fade, __eff_vol2 / __eff_vol_fadein: over `fadeMs` from `fadeAt`, in or out - out ends it)
    struct Voice { SDL_AudioStream* stream = nullptr; int entry = -1; bool loop = false, voice = false, fadeIn = false; uint64_t fadeAt = 0; int fadeMs = 0; };
    float effects_ = 0.8f, voice_ = 0.8f;
    std::vector<int> voiceEntries_;
    std::vector<PcmSound> bank_;
    std::vector<Voice> voices_; // 16, like the original's OpenAL sources
    void loadLineTables(const std::string& dataDir);
    unsigned device_ = 0;
};

} // namespace cr
