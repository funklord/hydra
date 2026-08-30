#pragma once

// Making the shell's dialogs usable on a phone (architecture doc sec 19).
//
// Every dialog here was laid out for a desktop, where a window takes the size
// its contents ask for and the screen is wider than that. On a phone it is not:
// the media dialog came up 1200 logical pixels wide on a 1080-pixel screen, so
// its list was visible and its buttons were not -- off the right edge, with no
// way to scroll a dialog to reach them. Found by tapping Play and discovering
// there was nothing to tap.
//
// The rule is one line and applies to all of them: on Android a dialog fills the
// available screen. Rather than touching thirty constructors, it is applied
// where dialogs become visible -- an application-wide event filter, installed
// once from `main()` and compiled only for Android.
namespace android_dialogs {

// Installs the filter on the application. Call once, after QApplication exists.
void install();

}  // namespace android_dialogs
