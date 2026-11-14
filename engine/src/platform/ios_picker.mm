// iOS: the open dialogs SDL has none for (platform/dialogs.hpp) - UIDocumentPickerViewController, over
// the game's window or, before it has one (the first start's setup), a window of its own. Files of any
// kind or a folder, opened in place (asCopy NO: a 2 GB dump isn't copied into the app's temporary folder
// first); each URL's security-scoped access is started and kept for the app's life, so the setup and the
// game can read it - a folder's whole tree - with plain file calls.

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <SDL3/SDL.h>

#include <string>
#include <vector>

@interface CRDocumentPicker : NSObject <UIDocumentPickerDelegate>
@property(nonatomic) SDL_DialogFileCallback callback;
@property(nonatomic) void* userdata;
@property(nonatomic, strong) UIWindow* window; // (ours, while the game has none)
@end

static NSMutableArray<NSURL*>* gAccessed; // (kept open: read by path afterwards)
static CRDocumentPicker* gActive;         // (the picker holds its delegate weakly)

@implementation CRDocumentPicker
- (void)answer:(NSArray<NSURL*>*)urls {
    CRDocumentPicker* keep = self; // (alive to the end, gActive cleared below)
    std::vector<std::string> paths;
    for (NSURL* url in urls) {
        if ([url startAccessingSecurityScopedResource]) [gAccessed addObject:url];
        if (const char* p = url.fileSystemRepresentation) paths.push_back(p);
    }
    std::vector<const char*> list;
    for (const std::string& p : paths) list.push_back(p.c_str());
    list.push_back(nullptr);
    if (keep.window) {
        keep.window.hidden = YES;
        keep.window = nil;
    }
    const SDL_DialogFileCallback callback = keep.callback;
    void* const userdata = keep.userdata;
    gActive = nil;
    if (callback) callback(userdata, list.data(), -1);
}
- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    (void)controller;
    [self answer:urls];
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
    (void)controller;
    [self answer:@[]];
}
@end

static UIWindowScene* activeScene() {
    for (UIScene* s in UIApplication.sharedApplication.connectedScenes)
        if ([s isKindOfClass:UIWindowScene.class] && s.activationState == UISceneActivationStateForegroundActive) return (UIWindowScene*)s;
    for (UIScene* s in UIApplication.sharedApplication.connectedScenes)
        if ([s isKindOfClass:UIWindowScene.class]) return (UIWindowScene*)s;
    return nil;
}

// The view controller on top: the key window's (the game's), what it presents, and what that presents
static UIViewController* topController() {
    UIWindow* key = nil;
    if (UIWindowScene* scene = activeScene())
        for (UIWindow* w in scene.windows)
            if (w.isKeyWindow || !key) key = w.isHidden ? key : w;
    UIViewController* top = key.rootViewController;
    while (top.presentedViewController && !top.presentedViewController.isBeingDismissed) top = top.presentedViewController;
    return top;
}

extern "C" void CR_IosPickDocuments(SDL_DialogFileCallback callback, void* userdata, bool folder, bool allowMany) {
    @autoreleasepool {
        if (!gAccessed) gAccessed = [NSMutableArray array];
        if (gActive) { // (one at a time, as SDL's: the second refused)
            SDL_SetError("a document picker is open already");
            if (callback) callback(userdata, nullptr, -1);
            return;
        }
        UIDocumentPickerViewController* picker =
            [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ folder ? UTTypeFolder : UTTypeItem ] asCopy:NO];
        picker.allowsMultipleSelection = allowMany && !folder;
        picker.modalPresentationStyle = UIModalPresentationFormSheet;
        CRDocumentPicker* d = [CRDocumentPicker new];
        d.callback = callback;
        d.userdata = userdata;
        picker.delegate = d;
        gActive = d;
        UIViewController* top = topController();
        if (!top) { // (the first start's setup: before the game's window - one to present it from)
            UIWindowScene* scene = activeScene();
            UIWindow* w = scene ? [[UIWindow alloc] initWithWindowScene:scene] : [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
            w.rootViewController = [UIViewController new];
            w.windowLevel = UIWindowLevelAlert;
            [w makeKeyAndVisible];
            d.window = w;
            top = w.rootViewController;
        }
        [top presentViewController:picker animated:YES completion:nil];
    }
}
