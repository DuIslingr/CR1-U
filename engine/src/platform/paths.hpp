#pragma once

#include <string>

namespace cr {

// Locates the user's original `dataHD` folder. Search order:
//   1. --data <dir> on the command line
//   2. CR_DATA environment variable
//   3. <app dir>/dataHD (desktop) or <external storage>/dataHD (Android)
// Returns an empty string if none contain the game data. The result ends with a separator.
std::string findDataDir(int argc, char** argv);

// Where the platform's expected data location is, for error messages.
std::string defaultDataDirHint();

// Writable per-user directory for saves and settings (ends with a separator).
std::string saveDir();

} // namespace cr
