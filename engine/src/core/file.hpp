#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cr {

// Reads a whole file through SDL's IO layer (works for Android storage and desktop paths).
// Paths inside the folder given to setDataOverlay are offered to the overlay first.
bool readFile(const std::string& path, std::vector<uint8_t>& out);

// Files of folder `dir` (with its trailing '/') that come from elsewhere: `read` is asked for the
// file's name and returns false to leave it to the folder (the Vita copy's scripts). An empty
// `read` removes the overlay.
using DataOverlay = std::function<bool(const std::string& name, std::vector<uint8_t>& out)>;
void setDataOverlay(const std::string& dir, DataOverlay read);

} // namespace cr
