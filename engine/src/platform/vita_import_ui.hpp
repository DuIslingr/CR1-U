#pragma once
#include "platform/vita_import.hpp"
#include <SDL3/SDL.h>
#include <memory>
#include <mutex>

namespace vitaimport {
// Only the picker callback runs on another thread. Conversion and installation
// run on the game's main thread; callbacks retain no pointers to an App or
// Viewer.
struct Session {
    enum State { Idle, Picking, Confirm, Result } state = Idle;
    struct Choice {
        std::mutex mutex;
        bool ready = false;
        std::string path, error;
    };
    std::shared_ptr<Choice> choice;
    Plan plan;
    std::string game, message;
    bool succeeded = false;
    int focus = 1;
    bool active() const { return state != Idle; }
    void cancel() {
        choice.reset();
        state = Idle;
    }
    void prepareFolder(const std::string &path) {
        succeeded = false;
        focus = 1;
        try {
            plan = prepare(path, game);
            message = "Found " + std::to_string(plan.slots) + " save slot(s).\nExisting saves will be backed up.\n";
            if (!plan.warnings.empty())
                message += "Invalid suspend skipped. ";
            message += "Import now?";
            state = Confirm;
        } catch (const std::exception &e) {
            message = std::string("Import failed: ") + e.what();
            state = Result;
            focus = 0;
        }
    }
    void begin(const std::string &which, SDL_Window *window) {
        if (active())
            return;
        game = which;
        plan = Plan();
        message.clear();
        succeeded = false;
        focus = 1;
        if (const char *test = SDL_getenv("CR_TEST_VITA_IMPORT")) {
            prepareFolder(test);
            return;
        }
        choice = std::make_shared<Choice>();
        state = Picking;
        auto *holder = new std::shared_ptr<Choice>(choice);
        SDL_ShowOpenFolderDialog(
            [](void *data, const char *const *list, int) {
                std::unique_ptr<std::shared_ptr<Choice>> keep(static_cast<std::shared_ptr<Choice> *>(data));
                std::lock_guard<std::mutex> lock((*keep)->mutex);
                if (!list)
                    (*keep)->error = SDL_GetError();
                else if (list[0])
                    (*keep)->path = list[0];
                (*keep)->ready = true;
            },
            holder, window, nullptr, false);
    }
    void poll() {
        if (state != Picking || !choice)
            return;
        std::string path, error;
        {
            std::lock_guard<std::mutex> lock(choice->mutex);
            if (!choice->ready)
                return;
            path = choice->path;
            error = choice->error;
        }
        choice.reset();
        if (!error.empty()) {
            state = Result;
            message = "Cannot open the folder picker: " + error;
            focus = 0;
        } else if (path.empty())
            state = Idle;
        else
            prepareFolder(path);
    }
    void answer(bool yes, const std::string &profile) {
        if (state == Confirm) {
            if (!yes) {
                cancel();
                return;
            }
            try {
                const std::string backup = install(plan, profile);
                succeeded = true;
                message = "Imported " + std::to_string(plan.slots) + " save slot(s).\n";
                if (!backup.empty())
                    message += "Previous saves backed up.\n";
                if (!plan.warnings.empty())
                    message += "Invalid suspend skipped. ";
                message += "Load from the title.";
            } catch (const std::exception &e) {
                succeeded = false;
                message = std::string("Import failed: ") + e.what();
            }
            state = Result;
            focus = 0;
        } else if (state == Result)
            cancel();
    }
};
} // namespace vitaimport
