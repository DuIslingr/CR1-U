#pragma once
// MCA_ animations and skeletal posing — see docs/FORMATS.md ("MCA_", "Skinning").

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cr {

struct Key {
    float time = 0;
    uint8_t mode = 0;   // 1 linear, 2 Hermite, 3 step
    uint8_t n = 0;      // values per component group: 1 = value, 3 = value, in, out
    float v[12] = {};   // value[comps], in[comps], out[comps]
};

struct Track {
    uint8_t type = 0;   // 1 translation, 5 Euler XYZ rotation (turns), 11 scale, 12 visibility
    uint8_t comps = 0;  // 0 = unsupported encoding (skipped)
    std::vector<Key> keys;
};

struct Action {
    std::string name;
    float duration = 0; // frames
    uint32_t objClass = 0, objId = 0, objUser = 0; // the record header's class / id / user words
    std::vector<std::vector<Track>> segments; // segment i animates skeleton node i
};

// Parses an MCA_ block at data[pos]. Appends its actions to `out`.
bool parseMca(const uint8_t* data, size_t size, size_t pos, std::vector<Action>& out,
              std::string* error = nullptr);

// Evaluates a track at `time` into out[comps]. Returns false if the result is unusable.
bool evaluateTrack(const Track& track, float time, float* out);

} // namespace cr
