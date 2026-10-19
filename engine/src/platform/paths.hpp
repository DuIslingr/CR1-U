#pragma once

#include <string>
#include <vector>

namespace cr {

// Where the game looks for your copy and its HD textures, with nothing on the command line - the
// same in all four ports (CR1:U, CRO-U, CR2-U, CR3-U). The game folder is:
//   Windows, Linux   the folder holding the program
//   Linux AppImage   the folder holding the .AppImage (its base path is a read-only mount)
//   macOS            the folder holding the .app (then the .app's Contents/Resources/)
//   Android          Android/data/<package>/files/ on the shared storage (reachable over USB)
// then the working directory. In it:
//   ios/             the iOS release: its .app (or what is in it), or the unpacked .ipa (Payload/...)
//   android/         the Android release: its main.*.obb and its APK
//   vita/            the Vita release: its game folder (PCSG00497: eboot.bin, sce_sys/ ...) or what is in it
//   textures/export/ where --export-textures writes the pictures to upscale (<hash>.crraw)
//   textures/hd/     where the upscaled ones are read from (<hash>.crhd: tools/hd_textures.py)
// A release's folder may hold it one or two folders down (ios/Payload/ChaosRingsHD.app,
// vita/PCSG00497). The older places still work: dataHD/, data/, the .app or Payload/ in the game
// folder, hd-textures/ beside the program or in the profile folder.

// The game folders, in the order they are searched (each ends with a separator)
std::vector<std::string> gameFolders();

// Each place a release of `kind` ("ios", "android", "vita") may be: <game folder>/<kind>/ and the
// folders up to two below it, game folder by game folder (each ends with a separator)
std::vector<std::string> releaseFolders(const char* kind);

// Locates the iOS release's data folder (the one holding sys.evt). Search order:
//   1. --data <dir> on the command line
//   2. CR_DATA environment variable
//   3. ios/ in a game folder (the folder itself, its dataHD/ or data/), then the older places
// Returns an empty string if none contain the game data. The result ends with a separator.
std::string findDataDir(int argc, char** argv);

// Where the platform's expected data location is, for error messages (<game folder>/ios/).
std::string defaultDataDirHint();

// <game folder>/textures/<which>/ ("export", "hd") - the first game folder's (ends with a separator)
std::string texturesFolder(const char* which);

// Writable per-user directory for saves and settings (ends with a separator).
std::string saveDir();

} // namespace cr
