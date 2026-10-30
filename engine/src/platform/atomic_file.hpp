#pragma once

#include <SDL3/SDL.h>
#include <atomic>
#include <filesystem>
#include <string>

namespace crstorage {
// Stage on the destination filesystem and replace only after the complete file
// has been written and closed. Readers keep seeing the previous complete save
// if staging or replacement fails. This is not a power-loss durability promise.
inline bool replaceFile(const std::string& path, const std::string& text, std::string* error = nullptr) {
    namespace fs = std::filesystem;
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    const fs::path destination = fs::u8path(path);
    fs::path parent = destination.parent_path();
    if (parent.empty()) parent = ".";
    static std::atomic<unsigned long long> serial{0};
    fs::path stage;
    bool owned = false;
    for (int attempt = 0; attempt < 16; ++attempt) {
        stage = parent / fs::u8path(".cr-save-" + std::to_string(SDL_GetTicksNS()) + "-" + std::to_string(serial.fetch_add(1)));
        std::error_code ec;
        if (fs::create_directory(stage, ec)) { owned = true; break; }
        if (ec && ec != std::errc::file_exists) return fail(ec.message());
    }
    if (!owned) return fail("Could not create a save staging directory");
    const fs::path payload = stage / "data";
    const std::string temporary = payload.u8string();
    SDL_IOStream* io = SDL_IOFromFile(temporary.c_str(), "wb");
    bool written = io && SDL_WriteIO(io, text.data(), text.size()) == text.size();
    std::string why = written ? std::string() : SDL_GetError();
    if (io) {
        // Buffered writes can succeed before the OS reports a full disk. Never
        // publish the stage until both flushing and closing have succeeded.
        if (written && !SDL_FlushIO(io)) written = false, why = SDL_GetError();
        if (!SDL_CloseIO(io) && written) written = false, why = SDL_GetError();
    }
    const bool committed = written && SDL_RenamePath(temporary.c_str(), path.c_str());
    if (written && !committed) why = SDL_GetError();
    std::error_code ignored;
    fs::remove(payload, ignored);
    fs::remove(stage, ignored); // remove only our empty directory, never another writer's files
    if (!committed) return fail(why);
    if (error) error->clear();
    return true;
}
} // namespace crstorage
