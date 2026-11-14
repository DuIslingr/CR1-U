#pragma once
// The open dialogs (the same in all four ports): SDL's - on iOS, which SDL has none for (its dummy
// fails), the system's document picker (platform/ios_picker.mm): files of any kind (an .ipa, an .obb,
// an archive have no type it knows: every file shown) or a folder, read in place - their
// security-scoped access kept while the app runs -, answered on the main thread as the run loop turns
// (the waits here pump SDL's events). The callback as SDL's: the paths, an empty list cancelled.

#include <SDL3/SDL.h>

#ifdef SDL_PLATFORM_IOS
extern "C" void CR_IosPickDocuments(SDL_DialogFileCallback callback, void* userdata, bool folder, bool allowMany);
#endif

inline void crShowOpenFileDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window* window, const SDL_DialogFileFilter* filters,
                                 int nfilters, const char* defaultLocation, bool allowMany) {
#ifdef SDL_PLATFORM_IOS
    (void)window, (void)filters, (void)nfilters, (void)defaultLocation;
    CR_IosPickDocuments(callback, userdata, false, allowMany);
#else
    SDL_ShowOpenFileDialog(callback, userdata, window, filters, nfilters, defaultLocation, allowMany);
#endif
}

inline void crShowOpenFolderDialog(SDL_DialogFileCallback callback, void* userdata, SDL_Window* window, const char* defaultLocation,
                                   bool allowMany) {
#ifdef SDL_PLATFORM_IOS
    (void)window, (void)defaultLocation;
    CR_IosPickDocuments(callback, userdata, true, allowMany);
#else
    SDL_ShowOpenFolderDialog(callback, userdata, window, defaultLocation, allowMany);
#endif
}
