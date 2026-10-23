#include "audio/sound.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "audio/at9.hpp"
#include "audio/scream.hpp"
#include "core/file.hpp"

namespace cr {

namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

} // namespace

bool parseCafPcm(const uint8_t* f, size_t size, PcmSound& out, std::string* error) {
    auto fail = [&](const char* why) { if (error) *error = why; return false; };
    if (size < 8 || std::memcmp(f, "caff", 4) != 0) return fail("not a CAF file");
    bool haveDesc = false, haveData = false;
    for (size_t p = 8; p + 12 <= size;) {
        const uint64_t len = be64(f + p + 4);
        const uint8_t* c = f + p + 12;
        if (p + 12 + len > size) return fail("chunk past the end");
        if (std::memcmp(f + p, "desc", 4) == 0 && len >= 32) {
            uint64_t rateBits = be64(c);
            double rate;
            std::memcpy(&rate, &rateBits, 8);
            if (std::memcmp(c + 8, "lpcm", 4) != 0) return fail("not linear PCM");
            const uint32_t flags = be32(c + 12);
            out.rate = int(rate + 0.5);
            out.channels = int(be32(c + 24));
            out.bits = int(be32(c + 28));
            out.isFloat = flags & 1;
            out.bigEndian = !(flags & 2);
            haveDesc = true;
        } else if (std::memcmp(f + p, "data", 4) == 0 && len >= 4) {
            out.data.assign(c + 4, c + len); // after the edit count
            haveData = true;
        }
        p += 12 + size_t(len);
    }
    if (!haveDesc || !haveData) return fail("missing desc or data chunk");
    return true;
}

SoundSystem::~SoundSystem() { shutdown(); }

bool SoundSystem::init(const std::string& dataDir, const std::string& vitaBank) {
    std::vector<uint8_t> f;
    bank_.clear();
    std::string err;
    if (!vitaBank.empty()) {
        if (!readFile(vitaBank, f) || !parseScreamBank(f, bank_, &err)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "%s: %s", vitaBank.c_str(), err.empty() ? "unreadable" : err.c_str());
            return false;
        }
        bank_.resize(std::max<size_t>(bank_.size(), 0x800 / 8)); // (sndeff.bin's 256 slots)
        SDL_Log("sound effects: the Vita's (%s)", vitaBank.c_str());
    } else if (!readFile(dataDir + "sndeff.bin", f) || f.size() < 0x800) return false;
    for (size_t i = 0; vitaBank.empty() && i < 0x800 / 8; ++i) {
        uint32_t off = 0, len = 0;
        std::memcpy(&off, &f[i * 8], 4);
        std::memcpy(&len, &f[i * 8 + 4], 4);
        PcmSound s;
        if (off && len && off + size_t(len) <= f.size()) parseCafPcm(&f[off], len, s);
        bank_.push_back(std::move(s));
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "no audio: %s", SDL_GetError());
        return false;
    }
    device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (!device_) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "no audio device: %s", SDL_GetError());
        return false;
    }
    voices_.resize(16);
    loadLineTables(dataDir);
    return true;
}

void SoundSystem::loadLineTables(const std::string& dataDir) {
    const char* names[2] = {"voiceevnt.bin", "voicebtl.bin"};
    for (int b = 0; b < 2; ++b) {
        lineTable_[b].clear();
        std::string path = dataDir + names[b];
        uint64_t base = 0, located = 0;
        const bool inside = locate_ && locate_(names[b], path, base, located);
        SDL_IOStream* io = SDL_IOFromFile(path.c_str(), "rb");
        if (!io) continue;
        const Sint64 size = inside ? Sint64(located) : SDL_GetIOSize(io);
        uint32_t first[2] = {0, 0};
        if (SDL_SeekIO(io, Sint64(base), SDL_IO_SEEK_SET) >= 0 && SDL_ReadIO(io, first, 8) == 8 && first[0] >= 8 && first[0] % 8 == 0 &&
            first[0] < size) {
            std::vector<uint32_t> t(first[0] / 4);
            SDL_SeekIO(io, Sint64(base), SDL_IO_SEEK_SET);
            if (SDL_ReadIO(io, t.data(), first[0]) == first[0])
                for (size_t i = 0; i + 1 < t.size(); i += 2)
                    lineTable_[b].push_back({t[i], uint64_t(t[i]) + t[i + 1] <= uint64_t(size) ? t[i + 1] : 0});
            linePath_[b] = path, lineBase_[b] = base;
        }
        SDL_CloseIO(io);
    }
    if (hasVoices()) SDL_Log("voices: %zu story clips, %zu battle clips", lineTable_[0].size(), lineTable_[1].size());
}

void SoundSystem::stopLine() {
    if (line_) SDL_DestroyAudioStream(line_);
    line_ = nullptr;
}

bool SoundSystem::linePlaying() const {
    return line_ && (SDL_GetAudioStreamQueued(line_) > 0 || SDL_GetAudioStreamAvailable(line_) > 0);
}

bool SoundSystem::readClip(int bank, int clip, std::vector<uint8_t>& pcm) const {
    const auto& table = lineTable_[bank];
    if (clip < 0 || clip >= int(table.size()) || table[clip].second < 2) return false;
    pcm.resize(table[clip].second & ~1u);
    SDL_IOStream* io = SDL_IOFromFile(linePath_[bank].c_str(), "rb");
    if (!io) return false;
    const bool ok = SDL_SeekIO(io, Sint64(lineBase_[bank] + table[clip].first), SDL_IO_SEEK_SET) >= 0 &&
                    SDL_ReadIO(io, pcm.data(), pcm.size()) == pcm.size();
    SDL_CloseIO(io);
    return ok;
}

void SoundSystem::setVitaLines(bool battle, std::vector<std::string> names, VitaReader read) {
    vitaLines_[battle] = std::move(names);
    vitaRead_[battle] = std::move(read);
    if (!vitaLines_[battle].empty())
        SDL_Log("voices: %zu %s clips from the Vita", vitaLines_[battle].size(), battle ? "battle" : "story");
}

bool SoundSystem::clipAudio(int bank, int clip, std::vector<uint8_t>& pcm, int& rate) const {
    if (!vitaLines_[bank].empty()) {
        std::vector<uint8_t> file;
        std::vector<int16_t> s;
        int channels = 0;
        if (clip < 0 || clip >= int(vitaLines_[bank].size()) || !vitaRead_[bank] || !vitaRead_[bank](vitaLines_[bank][size_t(clip)], file) ||
            !decodeAt9(file, s, rate, channels) || channels != 1)
            return false;
        const float gain = bank ? 1.0f / 1.514f : 1.0f / 1.669f; // to 2.2.0's level (crtool vitaaudio)
        pcm.resize(s.size() * 2);
        for (size_t i = 0; i < s.size(); ++i) {
            const uint16_t v = uint16_t(int16_t(std::lround(float(s[i]) * gain))); // (little-endian, as the banks)
            pcm[i * 2] = uint8_t(v), pcm[i * 2 + 1] = uint8_t(v >> 8);
        }
        return true;
    }
    rate = 20000;
    return readClip(bank, clip, pcm);
}

void SoundSystem::playBattleLine(int clip, int tag) {
    battleLineLog.push_back(clip);
    std::vector<uint8_t> pcm;
    int rate = 0;
    if (!device_ || !clipAudio(1, clip, pcm, rate)) return;
    for (auto it = battleLines_.begin(); it != battleLines_.end();) // the speaker's last line stops
        if (it->first == tag) SDL_DestroyAudioStream(it->second), it = battleLines_.erase(it);
        else ++it;
    const SDL_AudioSpec spec{SDL_AUDIO_S16LE, 1, rate};
    SDL_AudioStream* s = SDL_CreateAudioStream(&spec, nullptr);
    if (!s || !SDL_BindAudioStream(device_, s)) { if (s) SDL_DestroyAudioStream(s); return; }
    SDL_SetAudioStreamGain(s, voice_);
    SDL_PutAudioStreamData(s, pcm.data(), int(pcm.size()));
    SDL_FlushAudioStream(s);
    battleLines_.push_back({tag, s});
}

void SoundSystem::stopBattleLines() {
    for (auto& [tag, s] : battleLines_) SDL_DestroyAudioStream(s);
    battleLines_.clear();
}

bool SoundSystem::battleLinePlaying() const {
    for (const auto& [tag, s] : battleLines_)
        if (SDL_GetAudioStreamQueued(s) > 0 || SDL_GetAudioStreamAvailable(s) > 0) return true;
    return false;
}

void SoundSystem::playLine(int clip, bool battle) {
    std::vector<uint8_t> pcm;
    int rate = 0;
    if (!device_ || !clipAudio(battle ? 1 : 0, clip, pcm, rate)) return;
    stopLine();
    const SDL_AudioSpec spec{SDL_AUDIO_S16LE, 1, rate};
    line_ = SDL_CreateAudioStream(&spec, nullptr);
    if (!line_ || !SDL_BindAudioStream(device_, line_)) { stopLine(); return; }
    SDL_SetAudioStreamGain(line_, voice_);
    SDL_PutAudioStreamData(line_, pcm.data(), int(pcm.size()));
    SDL_FlushAudioStream(line_);
}

void SoundSystem::shutdown() {
    stopLine();
    stopBattleLines();
    for (Voice& v : voices_)
        if (v.stream) SDL_DestroyAudioStream(v.stream);
    voices_.clear();
    if (device_) SDL_CloseAudioDevice(device_);
    device_ = 0;
}

void SoundSystem::stop(int entry) {
    for (Voice& v : voices_)
        if (v.stream && v.entry == entry) {
            SDL_DestroyAudioStream(v.stream);
            v = Voice();
        }
}

void SoundSystem::stopAll() {
    for (Voice& v : voices_)
        if (v.stream) {
            SDL_DestroyAudioStream(v.stream);
            v = Voice();
        }
}

void SoundSystem::play(uint32_t id) {
    const int entry = int(id & 0xfff);
    if (id & 0x20000000) { stop(entry); return; }
    const int fadeOut = id & 0x4000000 ? 10 : id & 0x8000000 ? 20 : id & 0x2000000 ? 40 : 0;
    const int fadeIn = fadeOut ? 0 : id & 0x40000 ? 10 : id & 0x80000 ? 20 : id & 0x20000 ? 40 : 0;
    if (fadeOut || fadeIn) { // (the original's frames, 20 a second)
        for (Voice& v : voices_)
            if (v.stream && v.entry == entry) {
                v.fadeIn = fadeIn > 0, v.fadeAt = SDL_GetTicks(), v.fadeMs = (fadeOut + fadeIn) * 50;
                if (v.fadeIn) SDL_SetAudioStreamGain(v.stream, 0.0f);
            }
        return;
    }
    if (!device_ || entry >= int(bank_.size()) || bank_[entry].data.empty()) return;
    const PcmSound& s = bank_[entry];
    SDL_AudioSpec spec{};
    spec.freq = s.rate;
    spec.channels = s.channels;
    spec.format = s.isFloat ? (s.bigEndian ? SDL_AUDIO_F32BE : SDL_AUDIO_F32LE)
                : s.bits == 8 ? SDL_AUDIO_U8
                : (s.bigEndian ? SDL_AUDIO_S16BE : SDL_AUDIO_S16LE);
    Voice* slot = nullptr;
    for (Voice& v : voices_)
        if (!v.stream) { slot = &v; break; }
    if (!slot) return; // all 16 busy
    slot->stream = SDL_CreateAudioStream(&spec, nullptr);
    if (!slot->stream) return;
    if (!SDL_BindAudioStream(device_, slot->stream)) {
        SDL_DestroyAudioStream(slot->stream);
        *slot = Voice();
        return;
    }
    slot->entry = entry;
    slot->loop = id & 0x800000;
    slot->voice = (id & 0x400000) || isVoice(entry);
    SDL_SetAudioStreamGain(slot->stream, slot->voice ? voice_ : effects_);
    SDL_PutAudioStreamData(slot->stream, s.data.data(), int(s.data.size()));
    if (!slot->loop) SDL_FlushAudioStream(slot->stream);
}

bool SoundSystem::isVoice(int entry) const {
    for (int e : voiceEntries_)
        if (e == entry) return true;
    return false;
}

void SoundSystem::setVolumes(float effects, float voice) {
    effects_ = effects;
    voice_ = voice;
    for (Voice& v : voices_)
        if (v.stream) SDL_SetAudioStreamGain(v.stream, v.voice ? voice_ : effects_);
    if (line_) SDL_SetAudioStreamGain(line_, voice_);
    for (auto& [tag, s] : battleLines_) SDL_SetAudioStreamGain(s, voice_);
}

void SoundSystem::update() {
    for (Voice& v : voices_) {
        if (!v.stream) continue;
        if (v.fadeMs > 0) {
            const float t = std::min(1.0f, float(SDL_GetTicks() - v.fadeAt) / float(v.fadeMs));
            if (!v.fadeIn && t >= 1.0f) {
                SDL_DestroyAudioStream(v.stream);
                v = Voice();
                continue;
            }
            SDL_SetAudioStreamGain(v.stream, (v.voice ? voice_ : effects_) * (v.fadeIn ? t : 1.0f - t));
            if (t >= 1.0f) v.fadeMs = 0;
        }
        const int queued = SDL_GetAudioStreamQueued(v.stream);
        if (v.loop) {
            const PcmSound& s = bank_[v.entry];
            if (queued < int(s.data.size())) SDL_PutAudioStreamData(v.stream, s.data.data(), int(s.data.size()));
        } else if (queued <= 0 && SDL_GetAudioStreamAvailable(v.stream) <= 0) {
            SDL_DestroyAudioStream(v.stream);
            v = Voice();
        }
    }
    for (auto it = battleLines_.begin(); it != battleLines_.end();) // finished battle lines
        if (SDL_GetAudioStreamQueued(it->second) <= 0 && SDL_GetAudioStreamAvailable(it->second) <= 0)
            SDL_DestroyAudioStream(it->second), it = battleLines_.erase(it);
        else ++it;
}

} // namespace cr
