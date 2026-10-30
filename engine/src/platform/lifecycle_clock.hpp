#pragma once
#include <SDL3/SDL.h>
#include <mutex>
#include <algorithm>

namespace crtime {
// SDL can block event pumping until resume. Its event watch records the lifecycle
// times when they happen, including notifications from a mobile lifecycle thread.
// Only clock state is touched here; gameplay and audio devices stay on their threads.
class LifecycleClock {
public:
    LifecycleClock() = default;
    LifecycleClock(const LifecycleClock&) = delete;
    LifecycleClock& operator=(const LifecycleClock&) = delete;
    ~LifecycleClock() { stop(); }
    bool start() {
        stop();
        reset();
        watching_ = SDL_AddEventWatch(watch, this);
        return watching_;
    }
    void stop() {
        if (watching_) SDL_RemoveEventWatch(watch, this);
        watching_ = false;
    }
    // Explicit timestamps allow deterministic tests without waiting through a long pause.
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        excluded_ = pausedAt_ = last_ = 0;
        paused_ = haveLast_ = false;
        ++generation_;
    }
    void note(const SDL_Event& event, Uint64 wall = 0) {
        switch (event.type) {
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
        case SDL_EVENT_DID_ENTER_BACKGROUND: {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!paused_) {
                pausedAt_ = wall ? wall : SDL_GetTicksNS();
                paused_ = true;
                ++generation_;
            }
            break;
        }
        case SDL_EVENT_DID_ENTER_FOREGROUND: {
            std::lock_guard<std::mutex> lock(mutex_);
            if (paused_) {
                if (!wall) wall = SDL_GetTicksNS();
                if (wall >= pausedAt_) excluded_ += wall - pausedAt_;
                paused_ = false;
                ++generation_;
            }
            break;
        }
        default: break;
        }
    }
    Uint64 ticksNS(Uint64 wall = 0) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ticksLocked(wall ? wall : SDL_GetTicksNS());
    }
    Uint64 ticksMS() const { return ticksNS() / SDL_NS_PER_MS; }
    float step(Uint64 wall = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        // Sample under the lock: a lifecycle callback can otherwise overtake a
        // caller that was preempted after sampling wall time but before locking.
        const Uint64 active = ticksLocked(wall ? wall : SDL_GetTicksNS());
        const float dt = haveLast_ && !paused_ && seen_ == generation_ && active >= last_
                             ? float(double(active - last_) / double(SDL_NS_PER_SECOND)) : 0;
        last_ = active;
        seen_ = generation_;
        haveLast_ = true;
        return dt;
    }
private:
    Uint64 ticksLocked(Uint64 wall) const {
        const Uint64 end = paused_ ? std::min(wall, pausedAt_) : wall;
        return end >= excluded_ ? end - excluded_ : 0;
    }
    static bool SDLCALL watch(void* data, SDL_Event* event) {
        static_cast<LifecycleClock*>(data)->note(*event);
        return true;
    }
    mutable std::mutex mutex_;
    bool watching_ = false, paused_ = false, haveLast_ = false;
    Uint64 excluded_ = 0, pausedAt_ = 0, last_ = 0, generation_ = 0, seen_ = 0;
};
} // namespace crtime
