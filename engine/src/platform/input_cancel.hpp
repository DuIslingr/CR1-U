#pragma once
#include <SDL3/SDL.h>
#include <atomic>

namespace crinput {
// SDL can deliver mobile background notifications on its lifecycle thread. The watch
// only records a request; the game thread cancels gestures before handling more input.
class CancelWatch {
public:
    CancelWatch() = default;
    CancelWatch(const CancelWatch&) = delete;
    CancelWatch& operator=(const CancelWatch&) = delete;
    ~CancelWatch() { stop(); }
    bool start(SDL_WindowID window) {
        stop();
        window_.store(window, std::memory_order_relaxed);
        pending_.store(false, std::memory_order_release);
        watching_ = SDL_AddEventWatch(watch, this);
        return watching_;
    }
    void stop() {
        if (watching_) SDL_RemoveEventWatch(watch, this);
        watching_ = false;
    }
    bool note(const SDL_Event& event) {
        bool cancel = false;
        switch (event.type) {
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
        case SDL_EVENT_DID_ENTER_BACKGROUND:
            cancel = true;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        case SDL_EVENT_WINDOW_MINIMIZED:
            cancel = event.window.windowID == window_.load(std::memory_order_relaxed);
            break;
        case SDL_EVENT_FINGER_CANCELED:
            cancel = event.tfinger.windowID == window_.load(std::memory_order_relaxed);
            break;
        default: break;
        }
        if (cancel) pending_.store(true, std::memory_order_release);
        return cancel;
    }
    bool consume() { return pending_.exchange(false, std::memory_order_acq_rel); }
private:
    static bool SDLCALL watch(void* data, SDL_Event* event) {
        static_cast<CancelWatch*>(data)->note(*event);
        return true;
    }
    std::atomic<SDL_WindowID> window_{0};
    bool watching_ = false;
    std::atomic<bool> pending_{false};
};
// A controller held through a cancel isn't a new press: read each frame, it would carry on walking
// and repeating, and out of focus SDL keeps its state as it was. Dead until it's let go.
class HeldOver {
public:
    void cancel() { waiting_ = true; }
    bool live(bool held) {
        if (!held) waiting_ = false;
        return !waiting_;
    }
private:
    bool waiting_ = false;
};
} // namespace crinput
