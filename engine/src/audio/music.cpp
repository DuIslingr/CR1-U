#include "audio/music.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "audio/at9.hpp"
#include "core/file.hpp"
#include "core/timing.hpp"

#ifdef CR_HAVE_FAAD2
#include <neaacdec.h>
#endif

#define STB_VORBIS_HEADER_ONLY
#include "stb/stb_vorbis.c"

namespace cr {

namespace {

// _BGM_FILE / _BGM_REPEAT (0xcfa40 / 0xcb4a0): track -> file, loop start in AAC packets
// (setRepeatFrame:; 0: plays once - tracks 28 and 29)
const char* const kBgmFile[kBgmTracks] = {"bgm01", "bgm02", "bgm03", "bgm04", "bgm05", "bgm06", "bgm08", "bgm09", "bgm10",
                                          "bgm11", "bgm12", "bgm13", "bgm14", "bgm15", "bgm16", "bgm17", "bgm18", "bgm19",
                                          "bgm20", "bgm21", "bgm22", "bgm23", "bgm24", "bgm25", "bgm26", "bgm27", "bgm28",
                                          "bgm29", "bgm30", "bgm31"};
const int kBgmRepeat[kBgmTracks] = {957, 391, 296, 200, 120, 551, 15,  1842, 401, 399, 765, 807, 534, 1435, 1119,
                                    270, 613, 525, 689, 390, 447, 654, 356,  310, 310, 334, 570, 482, 0,    0};
// The Android build's loop points (setbgm_caf's table, 0x98f70 of libchaosrings_native.so): the
// sample its ov_pcm_seek returns to at the end, -1 none (its data has no j001m31)
const int kOggLoop[kBgmTracks] = {978746, 398876, 301489, 203550, 121368, 562552, 13738,  1885001, 409423, 406778,
                                  781352, 824487, 545096, 1468347, 1144534, 274661, 626313, 535908,  703488, 398141,
                                  456528, 668051, 363275, 315408, 315408, 340444, 581931, 491807,  -1,     -1};

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

// MPEG-4 descriptor length: up to four 7-bit groups
size_t descLen(const uint8_t* p, const uint8_t* end, const uint8_t*& next) {
    size_t n = 0;
    for (int i = 0; i < 4 && p < end; ++i) {
        n = n << 7 | (*p & 0x7f);
        if (!(*p++ & 0x80)) break;
    }
    next = p;
    return n;
}

// Finds the DecoderSpecificInfo (tag 5) inside an ESDS magic cookie
bool findAsc(const uint8_t* p, const uint8_t* end, std::vector<uint8_t>& out) {
    while (p + 2 <= end) {
        const uint8_t tag = *p++;
        const uint8_t* body;
        const size_t len = descLen(p, end, body);
        if (body + len > end) return false;
        if (tag == 5) { out.assign(body, body + len); return true; }
        if (tag == 3) { // ES_Descriptor: ES_ID(2), flags(1) [+ optional fields], then sub-descriptors
            const uint8_t flags = body[2];
            const uint8_t* q = body + 3;
            if (flags & 0x80) q += 2;
            if (flags & 0x40) q += 1 + *q;
            if (flags & 0x20) q += 2;
            if (findAsc(q, body + len, out)) return true;
        } else if (tag == 4) { // DecoderConfigDescriptor: 13 bytes, then sub-descriptors
            if (findAsc(body + 13, body + len, out)) return true;
        }
        p = body + len;
    }
    return false;
}

} // namespace

bool parseCafAac(const std::vector<uint8_t>& file, CafAac& out, std::string* error) {
    auto fail = [&](const char* why) { if (error) *error = why; return false; };
    const uint8_t* f = file.data();
    const size_t size = file.size();
    if (size < 8 || std::memcmp(f, "caff", 4) != 0) return fail("not a CAF file");
    std::vector<uint64_t> sizes;
    bool desc = false, data = false;
    for (size_t p = 8; p + 12 <= size;) {
        const uint64_t len = be64(f + p + 4);
        const uint8_t* c = f + p + 12;
        if (len > size - p - 12) return fail("chunk past the end");
        if (std::memcmp(f + p, "desc", 4) == 0 && len >= 32) {
            if (std::memcmp(c + 8, "aac ", 4) != 0) return fail("not AAC");
            uint64_t rb = be64(c);
            double rate;
            std::memcpy(&rate, &rb, 8);
            out.rate = int(rate + 0.5);
            out.framesPerPacket = int(be32(c + 20));
            out.channels = int(be32(c + 24));
            desc = true;
        } else if (std::memcmp(f + p, "kuki", 4) == 0) {
            if (!findAsc(c, c + len, out.config) && len <= 5) out.config.assign(c, c + len); // raw ASC
        } else if (std::memcmp(f + p, "pakt", 4) == 0 && len >= 24) {
            const uint64_t n = be64(c);
            out.validFrames = int64_t(be64(c + 8));
            out.priming = int(be32(c + 16));
            out.remainder = int(be32(c + 20));
            const uint8_t* q = c + 24;
            for (uint64_t i = 0; i < n && q < c + len; ++i) { // variable-length big-endian sizes
                uint64_t v = 0;
                while (q < c + len) {
                    const uint8_t b = *q++;
                    v = v << 7 | (b & 0x7f);
                    if (!(b & 0x80)) break;
                }
                sizes.push_back(v);
            }
        } else if (std::memcmp(f + p, "data", 4) == 0 && len >= 4) {
            out.data.assign(c + 4, c + len);
            data = true;
        }
        p += 12 + size_t(len);
    }
    if (!desc || !data || out.config.empty() || sizes.empty()) return fail("missing desc / kuki / pakt / data");
    uint64_t at = 0;
    for (uint64_t s : sizes) {
        if (at + s > out.data.size()) return fail("packet table past the data");
        out.offset.push_back(uint32_t(at));
        out.size.push_back(uint32_t(s));
        at += s;
    }
    return true;
}

struct MusicPlayer::Impl {
    unsigned device = 0;
    SDL_AudioStream* stream = nullptr;
    CafAac track;
    size_t packet = 0;          // next packet to decode
    int64_t framesOut = 0;      // frames sent this pass (for trimming priming / remainder)
    bool firstPass = true;
    int loopPacket = 0;
    bool loops = true;
    bool finished = false;
    // The Vita's track: superframe `sf` next, `pos` the next frame out (after the encoder delay),
    // ending at `end` (the loop's end + 1 while looping)
    bool vita = false;
    std::vector<uint8_t> at9Bytes;
    At9File at9;
    At9Decoder at9Dec;
    size_t sf = 0;
    int64_t pos = 0, end = 0;
    std::vector<int16_t> block;
    // Android's track: Ogg Vorbis from its file's +0x400, looping at `oggLoop` (-1 plays once)
    stb_vorbis* ogg = nullptr;
    std::vector<uint8_t> oggBytes;
    int oggLoop = -1, oggChannels = 2;
    std::vector<int16_t> oggBuf;
    void closeOgg() {
        if (ogg) stb_vorbis_close(ogg);
        ogg = nullptr;
    }
    // Restarts at frame t: the decoder primed with the superframe before (frames overlap)
    bool at9Seek(int64_t t) {
        if (!at9Dec.open(at9)) return false;
        const int64_t raw = t + at9.delay, s = at9Dec.superframeSamples();
        sf = size_t(raw / s);
        if (sf > 0) at9Dec.decode(sf - 1, block);
        pos = t;
        return true;
    }
#ifdef CR_HAVE_FAAD2
    NeAACDecHandle dec = nullptr;
    bool openDecoder() {
        if (dec) NeAACDecClose(dec);
        dec = NeAACDecOpen();
        NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(dec);
        cfg->outputFormat = FAAD_FMT_16BIT;
        NeAACDecSetConfiguration(dec, cfg);
        unsigned long rate = 0;
        unsigned char ch = 0;
        return NeAACDecInit2(dec, track.config.data(), (unsigned long)track.config.size(), &rate, &ch) >= 0;
    }
    // Decodes packet `i`; returns interleaved s16 frames (count in `frames`).
    const int16_t* decode(size_t i, int& frames) {
        frames = 0;
        if (!dec || i >= track.size.size()) return nullptr;
        NeAACDecFrameInfo info;
        void* pcm = NeAACDecDecode(dec, &info, &track.data[track.offset[i]], track.size[i]);
        if (info.error || !pcm) return nullptr;
        frames = int(info.samples / std::max<unsigned char>(info.channels, 1));
        return static_cast<const int16_t*>(pcm);
    }
#endif
    ~Impl() {
        closeOgg();
#ifdef CR_HAVE_FAAD2
        if (dec) NeAACDecClose(dec);
#endif
        if (stream) SDL_DestroyAudioStream(stream);
        if (device) SDL_CloseAudioDevice(device);
    }
};

bool decodeTrack(const std::string& dataDir, int t, int64_t& frames, uint64_t& hash, std::string* error) {
    frames = 0;
    hash = 1469598103934665603ull;
#ifndef CR_HAVE_FAAD2
    (void)dataDir, (void)t;
    if (error) *error = "built without FAAD2";
    return false;
#else
    std::vector<uint8_t> file;
    CafAac caf;
    if (t < 0 || t >= kBgmTracks || !readFile(dataDir + kBgmFile[t] + ".caf", file) || !parseCafAac(file, caf, error)) return false;
    NeAACDecHandle dec = NeAACDecOpen();
    NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(dec);
    cfg->outputFormat = FAAD_FMT_16BIT;
    NeAACDecSetConfiguration(dec, cfg);
    unsigned long rate = 0;
    unsigned char ch = 0;
    if (NeAACDecInit2(dec, caf.config.data(), (unsigned long)caf.config.size(), &rate, &ch) < 0) {
        NeAACDecClose(dec);
        if (error) *error = "decoder init failed";
        return false;
    }
    for (size_t i = 0; i < caf.size.size(); ++i) {
        NeAACDecFrameInfo info;
        void* pcm = NeAACDecDecode(dec, &info, &caf.data[caf.offset[i]], caf.size[i]);
        if (info.error || !pcm) continue;
        const auto* b = static_cast<const uint8_t*>(pcm);
        for (unsigned long k = 0; k < info.samples * 2; ++k) hash = (hash ^ b[k]) * 1099511628211ull;
        if (const char* dump = std::getenv("CR_MUSIC_DUMP")) { // tests: the raw s16 PCM appended to a file
            if (FILE* f = std::fopen(dump, "ab")) { std::fwrite(b, 1, info.samples * 2, f); std::fclose(f); }
        }
        frames += int64_t(info.samples / std::max<unsigned char>(info.channels, 1));
    }
    NeAACDecClose(dec);
    return frames > 0;
#endif
}

bool checkAndroidTrack(const std::function<bool(const std::string&, std::vector<uint8_t>&)>& read, int t, int64_t& samples,
                       bool& loopOk) {
    samples = 0, loopOk = false;
    std::vector<uint8_t> f;
    if (t < 0 || t >= kBgmTracks || !read || !read(std::string("j001m") + (kBgmFile[t] + 3) + ".mca", f) || f.size() <= 0x400)
        return false;
    int e = 0;
    stb_vorbis* v = stb_vorbis_open_memory(f.data() + 0x400, int(f.size() - 0x400), &e, nullptr);
    if (!v) return false;
    samples = stb_vorbis_stream_length_in_samples(v);
    short buf[4096];
    const int ch = stb_vorbis_get_info(v).channels;
    loopOk = kOggLoop[t] < 0 || (kOggLoop[t] < samples && stb_vorbis_seek(v, unsigned(kOggLoop[t])) &&
                                 stb_vorbis_get_samples_short_interleaved(v, ch, buf, 4096) > 0);
    stb_vorbis_close(v);
    return samples > 0;
}

MusicPlayer::MusicPlayer() = default;
MusicPlayer::~MusicPlayer() = default;

bool MusicPlayer::playingVita() const { return impl_ && impl_->stream && impl_->vita; }
bool MusicPlayer::playingAndroid() const { return impl_ && impl_->stream && impl_->ogg; }
bool MusicPlayer::ready() const { return impl_ && impl_->device; }

bool MusicPlayer::init(const std::string& dataDir) {
#if !defined(CR_HAVE_FAAD2)
    SDL_Log("music: built without an AAC decoder (FAAD2): the Vita's or Android's tracks only");
#endif
    dataDir_ = dataDir;
    impl_ = std::make_unique<Impl>();
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
    impl_->device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    return impl_->device != 0;
}

void MusicPlayer::command(uint32_t v) {
    if (!impl_ || !impl_->device) return;
    auto stopNow = [&] {
        if (impl_->stream) SDL_DestroyAudioStream(impl_->stream);
        impl_->stream = nullptr;
        impl_->closeOgg();
        track_ = -1;
    };
    if (v & 0x20000000) { stopNow(); return; }
    const int fadeFrames = (v & 0x02000000) ? 40 : (v & 0x04000000) ? 10 : (v & 0x08000000) ? 20 : 0;
    if (fadeFrames) { fadeStep_ = -1.0f / float(fadeFrames); return; } // update() stops at 0
    const int t = int(v & 0xff);
    if (t >= kBgmTracks || (t == track_ && impl_->stream)) return;
    std::string err;
#ifdef CR_HAVE_ATRAC9
    { // the Vita's cr_mNN.at9 for bgmNN
        std::vector<uint8_t> file;
        At9File at9;
        const std::string name = std::string("cr_m") + (kBgmFile[t] + 3) + ".at9";
        if (vita_ && vita_(name, file) && parseAt9(file, at9, &err)) {
            stopNow();
            Impl& m = *impl_;
            m.at9Bytes = std::move(file);
            if (!parseAt9(m.at9Bytes, m.at9, &err) || !m.at9Seek(0)) return; // (re-parsed: its pointers into the kept bytes)
            SDL_AudioSpec spec{SDL_AUDIO_S16, m.at9.channels, m.at9.rate};
            m.stream = SDL_CreateAudioStream(&spec, nullptr);
            if (!m.stream || !SDL_BindAudioStream(m.device, m.stream)) { stopNow(); return; }
            m.vita = true;
            m.finished = false;
            m.loops = kBgmRepeat[t] > 0 && m.at9.loopStart >= 0;
            m.end = m.loops ? m.at9.loopEnd + 1 : m.at9.samples;
            track_ = t;
            fade_ = 1.0f;
            fadeStep_ = 0.0f;
            update(0);
            SDL_Log("music: %s (Vita, %.1f s, loop %.3f..%.3f s)", name.c_str(), double(m.at9.samples) / m.at9.rate,
                    double(m.at9.loopStart) / m.at9.rate, double(m.at9.loopEnd) / m.at9.rate);
            return;
        }
    }
#endif
    { // Android's j001mNN.mca for bgmNN: Ogg Vorbis after its 1 KB header
        std::vector<uint8_t> file;
        const std::string name = std::string("j001m") + (kBgmFile[t] + 3) + ".mca";
        if (android_ && android_(name, file) && file.size() > 0x400) {
            stopNow();
            Impl& m = *impl_;
            m.oggBytes = std::move(file);
            int e = 0;
            m.ogg = stb_vorbis_open_memory(m.oggBytes.data() + 0x400, int(m.oggBytes.size() - 0x400), &e, nullptr);
            if (m.ogg) {
                const stb_vorbis_info info = stb_vorbis_get_info(m.ogg);
                m.oggChannels = info.channels;
                SDL_AudioSpec spec{SDL_AUDIO_S16, info.channels, int(info.sample_rate)};
                m.stream = SDL_CreateAudioStream(&spec, nullptr);
                if (!m.stream || !SDL_BindAudioStream(m.device, m.stream)) { stopNow(); return; }
                m.vita = false;
                m.finished = false;
                m.oggLoop = kOggLoop[t];
                track_ = t;
                fade_ = 1.0f;
                fadeStep_ = 0.0f;
                update(0);
                SDL_Log("music: %s (Android, %.1f s, loop from %.3f s)", name.c_str(),
                        double(stb_vorbis_stream_length_in_samples(m.ogg)) / info.sample_rate,
                        m.oggLoop < 0 ? -1.0 : double(m.oggLoop) / info.sample_rate);
                return;
            }
            SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "%s: not Ogg Vorbis (stb_vorbis error %d)", name.c_str(), e);
        }
    }
#ifdef CR_HAVE_FAAD2
    std::vector<uint8_t> file;
    CafAac caf;
    if (!readFile(dataDir_ + kBgmFile[t] + ".caf", file) || !parseCafAac(file, caf, &err)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "%s: %s", kBgmFile[t], err.c_str());
        return;
    }
    stopNow();
    impl_->vita = false;
    impl_->track = std::move(caf);
    if (!impl_->openDecoder()) return;
    SDL_AudioSpec spec{SDL_AUDIO_S16, impl_->track.channels, impl_->track.rate};
    impl_->stream = SDL_CreateAudioStream(&spec, nullptr);
    if (!impl_->stream || !SDL_BindAudioStream(impl_->device, impl_->stream)) { stopNow(); return; }
    impl_->packet = 0;
    impl_->framesOut = 0;
    impl_->firstPass = true;
    impl_->finished = false;
    impl_->loopPacket = std::min(kBgmRepeat[t], int(impl_->track.size.size()) - 1);
    impl_->loops = kBgmRepeat[t] > 0;
    track_ = t;
    fade_ = 1.0f;
    fadeStep_ = 0.0f;
    update(0);
    SDL_Log("music: %s (%zu packets, loop from packet %d), %d bytes queued", kBgmFile[t], impl_->track.size.size(),
            impl_->loopPacket, SDL_GetAudioStreamQueued(impl_->stream));
#endif
}

void MusicPlayer::update(float dt) {
    if (!impl_ || !impl_->stream) return;
    if (fadeStep_ != 0) {
        fade_ += fadeStep_ * dt * cr::kFramesPerSecond; // (a fade counts game frames)
        if (fade_ <= 0) { SDL_DestroyAudioStream(impl_->stream); impl_->stream = nullptr; track_ = -1; fade_ = 1; fadeStep_ = 0; return; }
    }
    SDL_SetAudioStreamGain(impl_->stream, volume_ * std::clamp(fade_, 0.0f, 1.0f));
    Impl& m = *impl_;
    if (m.ogg) {
        const int ahead = 44100 * m.oggChannels * 2 / 2; // half a second queued
        m.oggBuf.resize(size_t(4096 * m.oggChannels));
        while (!m.finished && SDL_GetAudioStreamQueued(m.stream) < ahead) {
            const int n = stb_vorbis_get_samples_short_interleaved(m.ogg, m.oggChannels, m.oggBuf.data(), int(m.oggBuf.size()));
            if (n > 0) {
                SDL_PutAudioStreamData(m.stream, m.oggBuf.data(), n * m.oggChannels * 2);
                continue;
            }
            // the end: back to the loop point (SndBgmPlayerCallback's ov_pcm_seek), or done
            if (m.oggLoop < 0 || !stb_vorbis_seek(m.ogg, unsigned(m.oggLoop))) { m.finished = true; SDL_FlushAudioStream(m.stream); break; }
        }
        return;
    }
#ifdef CR_HAVE_ATRAC9
    if (m.vita) {
        const int ahead = m.at9.rate * m.at9.channels * 2 / 2; // half a second queued
        const int64_t s = m.at9Dec.superframeSamples();
        while (!m.finished && SDL_GetAudioStreamQueued(m.stream) < ahead) {
            if (m.pos >= m.end) {
                if (!m.loops || !m.at9Seek(m.at9.loopStart)) { m.finished = true; SDL_FlushAudioStream(m.stream); break; }
                continue;
            }
            if (!m.at9Dec.decode(m.sf, m.block)) { m.pos = m.end; continue; } // (the data ran out: the end)
            const int64_t from = int64_t(m.sf) * s - m.at9.delay; // this superframe's first frame
            ++m.sf;
            const int64_t a = std::max(m.pos, from), b = std::min(from + s, m.end);
            if (b > a)
                SDL_PutAudioStreamData(m.stream, m.block.data() + (a - from) * m.at9.channels, int((b - a) * m.at9.channels * 2));
            m.pos = std::max(m.pos, b);
        }
        return;
    }
#endif
#ifdef CR_HAVE_FAAD2
    const int ahead = m.track.rate * m.track.channels * 2 / 2; // keep half a second queued
    while (!m.finished && SDL_GetAudioStreamQueued(m.stream) < ahead) {
        if (m.packet >= m.track.size.size()) {
            if (!m.loops) { m.finished = true; SDL_FlushAudioStream(m.stream); break; }
            // Loop: restart the decoder one packet early (its output primes the overlap) and
            // resume at the repeat packet
            m.openDecoder();
            int dummy;
            if (m.loopPacket > 0) m.decode(size_t(m.loopPacket - 1), dummy);
            m.packet = size_t(m.loopPacket);
            m.firstPass = false;
            continue;
        }
        int frames = 0;
        const int16_t* pcm = m.decode(m.packet, frames);
        const int64_t packetStart = int64_t(m.packet) * m.track.framesPerPacket;
        ++m.packet;
        if (!pcm || frames <= 0) continue;
        // Trim the encoder priming at the very start and the padding after the last valid frame
        int64_t begin = 0, end = frames;
        if (m.firstPass && packetStart < m.track.priming) begin = std::min<int64_t>(frames, m.track.priming - packetStart);
        const int64_t validEnd = int64_t(m.track.priming) + m.track.validFrames;
        if (m.track.validFrames > 0 && packetStart + end > validEnd) end = std::max<int64_t>(begin, validEnd - packetStart);
        if (end > begin)
            SDL_PutAudioStreamData(m.stream, pcm + begin * m.track.channels, int((end - begin) * m.track.channels * 2));
    }
#endif
}

} // namespace cr
