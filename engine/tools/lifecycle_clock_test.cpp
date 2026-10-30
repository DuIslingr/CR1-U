#include "platform/lifecycle_clock.hpp"
#include <cmath>
#include <cstdio>
#include <thread>
int main() {
    if (!SDL_Init(SDL_INIT_EVENTS)) return 1;
    int problems = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) { ++problems; std::printf("lifecycle clock FAILED: %s\n", what); }
    };
    auto near = [](float a, float b) { return std::fabs(a-b)<1e-6f; };
    const Uint64 s = SDL_NS_PER_SECOND;
    {
        crtime::LifecycleClock clock;
        check(clock.step(s) == 0, "first sample is zero");
        check(near(clock.step(s+s/20), 0.05f), "foreground frame keeps its duration");
        SDL_Event event{};
        event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
        clock.note(event, s+s/10);
        check(near(clock.step(s+s/10), 0.05f), "desktop focus change alone does not pause gameplay");
        event.type = SDL_EVENT_WILL_ENTER_BACKGROUND;
        clock.note(event, 2*s);
        event.type = SDL_EVENT_DID_ENTER_BACKGROUND;
        clock.note(event, 3*s); // duplicate lifecycle phase must not move the pause start
        check(clock.ticksNS(3602*s) == 2*s, "an hour in the background does not advance active time");
        check(clock.step(3602*s) == 0, "no suspended frame duration");
        event.type = SDL_EVENT_WILL_ENTER_FOREGROUND;
        clock.note(event, 3603*s);
        check(clock.ticksNS(3603*s) == 2*s, "still paused during foreground preparation");
        event.type = SDL_EVENT_DID_ENTER_FOREGROUND;
        clock.note(event, 3604*s);
        clock.note(event, 3604*s+1); // duplicate cannot subtract a second interval
        check(clock.ticksNS(3604*s) == 2*s, "excluded time is subtracted exactly once");
        check(clock.step(3604*s) == 0, "first resumed frame rebases its delta");
        check(near(clock.step(3604*s+s/20), 0.05f), "normal updates resume immediately");
        check(near(clock.step(3605*s+s/20), 1.0f), "ordinary foreground stalls keep prior timing policy");
        event.type = SDL_EVENT_DID_ENTER_BACKGROUND;
        clock.note(event, 3606*s); // platforms may first send the did phase
        event.type = SDL_EVENT_DID_ENTER_FOREGROUND;
        clock.note(event, 3616*s);
        check(clock.ticksNS(3616*s) == 4*s, "repeated pause intervals accumulate");
        clock.reset();
        check(clock.ticksNS(s) == s && clock.step(s) == 0, "reset clears prior lifecycle state");
        clock.step(2*s);
        event.type = SDL_EVENT_WILL_ENTER_BACKGROUND;
        clock.note(event, 3*s);
        event.type = SDL_EVENT_DID_ENTER_FOREGROUND;
        clock.note(event, 1003*s); // no samples while SDL blocks
        check(clock.step(1003*s) == 0, "blocked event pump also rebases on resume");
    }
    {
        crtime::LifecycleClock clock;
        check(clock.start(), "watch attaches");
        std::thread worker([] {
            SDL_Event event{};
            event.type = SDL_EVENT_WILL_ENTER_BACKGROUND;
            SDL_PushEvent(&event);
        });
        worker.join();
        const Uint64 frozen = clock.ticksNS();
        SDL_Delay(2);
        check(clock.ticksNS() == frozen, "lifecycle-thread notification freezes the clock");
        SDL_Event event{};
        event.type = SDL_EVENT_DID_ENTER_FOREGROUND;
        SDL_PushEvent(&event);
        check(clock.step() == 0, "real queued foreground event rebases the frame");
        clock.stop();
        event.type = SDL_EVENT_WILL_ENTER_BACKGROUND;
        SDL_PushEvent(&event);
        const Uint64 at = clock.ticksNS();
        SDL_Delay(2);
        check(clock.ticksNS() > at, "watch detaches before destruction");
    }
    SDL_Quit();
    std::printf("lifecycle clock: %d problems\n", problems);
    return problems ? 1 : 0;
}
