#pragma once

#ifdef __OBJC__
#include "input.h"

@class NSEvent;
@class NSMenu;
@class NSString;
#endif

namespace stl {
    class ObjPool;
}

namespace plt {
    struct Platform;

    Platform* createCocoaPlatform(stl::ObjPool& owner);

#ifdef __OBJC__
    // The pure NSEvent -> KeyInput translation behind the window's key
    // handler, exposed so unit tests can drive it with synthesized events.
    KeyInput keyInputFromEvent(NSEvent* event, bool pressed);
    unsigned long cocoaWindowStyleMask(bool decorations);

#ifdef __OBJC__
    // The application menu. A Regular-policy app is given a menu bar
    // whether or not it supplies one, so without this macOS shows an
    // empty menu named after the app - and Cmd+Q, which is a menu key
    // equivalent rather than a key event, has no item to fire.
    NSMenu* cocoaBuildMainMenu(NSString* appName);
#endif
    bool cocoaResizeUsesExactProposal(bool fullscreen, bool viewAvailable, bool liveResize);
#endif
}
