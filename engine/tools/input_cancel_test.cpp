#include "platform/input_cancel.hpp"
#include <cstdio>
#include <thread>
int main() {
    if (!SDL_Init(SDL_INIT_EVENTS)) return 1;
    int problems = 0;
    {
        crinput::CancelWatch cancel;
        if (!cancel.start(42)) return 1;
        SDL_Event e{};
        e.type = SDL_EVENT_WINDOW_FOCUS_LOST;
        e.window.windowID = 99;
        SDL_PushEvent(&e);
        problems += cancel.consume(); // another window cannot cancel this one's gesture
        e.window.windowID = 42;
        SDL_PushEvent(&e);
        problems += !cancel.consume() || cancel.consume();
        e.type = SDL_EVENT_FINGER_CANCELED;
        e.tfinger.windowID = 42;
        SDL_PushEvent(&e);
        problems += !cancel.consume();
        // A lifecycle callback does not touch UI/game objects on the worker thread.
        std::thread worker([&] {
            SDL_Event background{};
            background.type = SDL_EVENT_WILL_ENTER_BACKGROUND;
            SDL_PushEvent(&background);
        });
        worker.join();
        problems += !cancel.consume() || cancel.consume();
        cancel.stop();
        SDL_PushEvent(&e);
        problems += cancel.consume(); // the detached callback cannot outlive its owner
    }
    {
        crinput::HeldOver pad; // a press after the cancel, not the one held through it
        problems += !pad.live(true);
        pad.cancel();
        problems += pad.live(true) || pad.live(true) || !pad.live(false) || !pad.live(true);
    }
    SDL_Quit();
    std::printf("input cancellation: window ownership, interrupted touch, lifecycle thread, detach and held controllers; %d problems\n", problems);
    return problems ? 1 : 0;
}
