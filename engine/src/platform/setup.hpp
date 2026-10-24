#pragma once
// The first start without a copy of the game (the same in CR1:U, CRO-U and CR2-U): instead of
// asking for the files to be put in the game folder, a window asks where they are - the iOS
// release's .ipa (unpacked into ios/ in the game folder, else the profile folder), the Android
// release's APK and OBB, the Vita release's folder or Vita3K's - and the port takes them from there
// (the Android and Vita copies read where they are: settings.ini remembers them).

#include <functional>
#include <string>

namespace cr {

struct SetupGame {
    const char* title = "";      // the window's ("Chaos Rings")
    const char* port = "";       // ("CR1:U")
    const char* vitaId = "";     // the Vita release's title id ("PCSG00497")
    const char* trilogyDir = ""; // its folder in the Vita's trilogy, PCSG00457 ("CR1"; "" none)
    const char* obbHint = "";    // the expansion file's name, for the message ("main.100.com...obb")
    bool oneRelease = false;     // (CR2-U: one copy is the game; the others aren't added to it)
    // whether a folder holds the release (the .app's / its data folder; an Android folder or OBB; a Vita game folder)
    std::function<bool(const std::string&)> iosOk, androidOk, vitaOk;
};

struct SetupResult {
    std::string ios;     // the .app unpacked (or chosen) - found again in ios/
    std::string android; // the folder with the APK and the OBB (or the OBB)
    std::string vita;    // the game's folder (PCSG00497, or the trilogy's CR1)
};

// Asks (message boxes and the system's file / folder dialogs) until one copy at least is given and
// Start chosen; false: Quit. `iosFolder` / `fallbackIos`: where an .ipa's app is unpacked to (the
// game folder's ios/, else the profile folder's)
bool runSetup(const SetupGame& game, const std::string& iosFolder, const std::string& fallbackIos, SetupResult& out);

// An .ipa's Payload/<name>.app unpacked into `folder` (<folder>/<name>.app/); its path in `app`.
// `progress` (0 .. 1) as it goes
bool unpackIpa(const std::string& ipa, const std::string& folder, std::string& app, std::string* error,
               const std::function<void(double)>& progress = {});

// The game's folder from a Vita3K folder (its own, or the pref path its config.yml names: ux0/app/<id>),
// ux0, the app folder, the game's folder itself or the trilogy's; "" none
std::string findVitaGame(const std::string& picked, const SetupGame& game);

} // namespace cr
