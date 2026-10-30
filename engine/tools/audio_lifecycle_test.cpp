#include "audio/sound.hpp"
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

int main() {
    if (!SDL_Init(SDL_INIT_EVENTS)) return 1;
    // One synthetic CAF entry accepted by both engines; no original assets needed.
    std::vector<uint8_t> caf;
    auto tag = [&](const char* s) { caf.insert(caf.end(), s, s+4); };
    auto be = [&](uint64_t n, int bytes) { for (int k=bytes-1;k>=0;--k) caf.push_back(uint8_t(n>>(k*8))); };
    tag("caff"); be(1,2); be(0,2);
    tag("desc"); be(32,8);
    double rate=22050; uint64_t rateBits=0; std::memcpy(&rateBits,&rate,8); be(rateBits,8);
    tag("lpcm"); be(2,4); be(2,4); be(1,4); be(1,4); be(16,4);
    tag("data"); be(4+22050*2,8); be(0,4); caf.resize(caf.size()+22050*2,0);
    std::vector<uint8_t> bank(0x800);
    const uint32_t offset=0x800, size=uint32_t(caf.size());
    std::memcpy(bank.data(),&offset,4); std::memcpy(bank.data()+4,&size,4);
    bank.insert(bank.end(),caf.begin(),caf.end());
    const auto folder=std::filesystem::temp_directory_path()/ ("cr-audio-lifecycle-"+std::to_string(SDL_GetTicksNS()));
    std::filesystem::create_directory(folder);
    const auto file=folder/"sndeff.bin";
    if (!SDL_SaveFile(file.string().c_str(), bank.data(), bank.size())) return 1;
    int problems=0;
    {
        cr::SoundSystem sound;
        if (!sound.init(folder.string()+"/")) return 1;
        sound.play(0xc00000); // looping speech-marked entry, countable through the public API
        problems += sound.dialogueVoiceCount()!=1;
        sound.play(0x4000000); // fade out over ten game frames (500 ms)
        std::thread worker([] {
            SDL_Event event{};
            event.type=SDL_EVENT_WILL_ENTER_BACKGROUND;
            SDL_PushEvent(&event);
        });
        worker.join();
        SDL_Delay(650); // longer than the fade; SDL's dummy driver keeps running
        SDL_Event event{};
        event.type=SDL_EVENT_DID_ENTER_FOREGROUND;
        SDL_PushEvent(&event);
        sound.update(); // refill the loop, but background time must not finish its fade
        const bool survived = sound.dialogueVoiceCount()==1;
        problems += !survived;
        // The same fade must still finish after genuine foreground playback time.
        SDL_Delay(550);
        sound.update();
        const bool completed = sound.dialogueVoiceCount()==0;
        problems += !completed;
        std::printf("audio lifecycle: fade survives background %s, finishes in foreground %s; %d problems\n",survived?"yes":"NO",completed?"yes":"NO",problems);
    }
    std::filesystem::remove(file); std::filesystem::remove(folder);
    SDL_Quit();
    return problems ? 1 : 0;
}
