#pragma once
// The first start without a copy of the game (the same in all four ports, each in its own
// namespace): a window asks where the copy is - the iOS release's .ipa, the Android release's APK
// and OBB (or its .apks), Vita3K's folder with the game installed (or the game's own folder) - and
// the files the port needs are copied into the game folder, the same layout in every port:
//
//   ios/<name>.app/      the .ipa's app, unpacked
//   android/             the APK and its OBB (or the .apks)
//   vita/<title id>/     the Vita game's folder as Vita3K installs it (ux0/app/<id>: eboot.bin, sce_sys/,
//                        the game's own folder); the Prequel Trilogy's PCSG00457/ with this game's folder in it
//   translation/<id>/    (CR3-U) the Vita release's fan translation, ux0/rePatch/<id>
//
// - where the game folder can't be written, the same in the profile folder. Every start finds them
// there as it would files put there by hand; nothing is read where it was chosen.
//
// Tests: CRU_SETUP_ANSWERS answers it without windows - ';' between the answers, each a button's id
// (1 iOS, 2 Android, 3 Vita, 4 Start, 0 Quit; Yes 1 / No 0 to a question) or a path chosen ('|'
// between files); what it would tell is logged. Answers run out: Quit

#include <functional>
#include <string>

namespace cr {

struct SetupGame {
    const char* title = "";      // the window's ("Chaos Rings")
    const char* port = "";       // ("CR1:U")
    const char* vitaId = "";     // the Vita release's title id ("PCSG00497")
    const char* trilogyDir = ""; // its folder in the Vita's trilogy, PCSG00457 ("CR1"; "" none)
    const char* obbHint = "";    // the expansion file's name ("main.100.com...obb"; or a hint, for the message)
    const char* package = "";    // the Android package ("com.square_enix.chaosrings"; "" none): APKs beside a pick that carry it go along
    bool oneRelease = false;     // (CR2-U, CR3-U: one copy is the game; the others aren't added to it)
    // whether a folder holds the release (the .app's / its data folder; an Android folder or OBB; a Vita game folder)
    std::function<bool(const std::string&)> iosOk, androidOk, vitaOk;
    // (CR3-U: the Vita release's fan translation - a folder that reads over it; none: not asked)
    std::function<bool(const std::string&)> patchOk;
};

struct SetupResult {
    std::string ios;     // where they were copied to: ios/<name>.app/
    std::string android; // android/
    std::string vita;    // vita/<id>/ (or vita/PCSG00457/<dir>/)
    std::string patch;   // translation/<id>/
};

// Asks (message boxes and the system's file / folder dialogs) until one copy at least is in place
// and Start chosen; false: Quit. `folder`: the game folder; `fallback`: the profile folder, where
// the game folder can't be written
bool runSetup(const SetupGame& game, const std::string& folder, const std::string& fallback, SetupResult& out);

// whether CRU_SETUP_ANSWERS answers it (a test's run: no windows)
bool setupScripted();

// An .ipa's Payload/<name>.app unpacked into `folder` (<folder>/<name>.app/); its path in `app`.
// `progress` (0 .. 1) as it goes
bool unpackIpa(const std::string& ipa, const std::string& folder, std::string& app, std::string* error,
               const std::function<void(double)>& progress = {});

// The game's folder from a Vita3K folder (its own, or the pref path its config.yml names: ux0/app/<id>),
// ux0, the app folder, the game's folder itself or the trilogy's; "" none
std::string findVitaGame(const std::string& picked, const SetupGame& game);

} // namespace cr
