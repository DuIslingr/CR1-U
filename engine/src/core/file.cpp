#include "core/file.hpp"

#include <SDL3/SDL.h>

namespace cr {

namespace {
std::string overlayDir;
DataOverlay overlay;
} // namespace

void setDataOverlay(const std::string& dir, DataOverlay read) {
    overlayDir = read ? dir : std::string();
    overlay = std::move(read);
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    if (overlay && !overlayDir.empty() && path.size() > overlayDir.size() && path.compare(0, overlayDir.size(), overlayDir) == 0 &&
        path.find('/', overlayDir.size()) == std::string::npos && overlay(path.substr(overlayDir.size()), out))
        return true;
    size_t size = 0;
    void* data = SDL_LoadFile(path.c_str(), &size);
    if (!data) return false;
    const auto* bytes = static_cast<const uint8_t*>(data);
    out.assign(bytes, bytes + size);
    SDL_free(data);
    return true;
}

} // namespace cr
