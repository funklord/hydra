#pragma once

#include <QString>
#include <QUrl>

// Handing something to another app on Android (architecture doc sec 19).
//
// The desktop names a player and starts a process. Android has neither: apps are
// reached through intents, and which one answers is the system's business and the
// user's. That difference is why this is a separate file rather than a branch
// inside `player_launcher` -- the *decision* about what to play is shared, and
// only the handoff is platform work.
namespace android_intents {

// `ACTION_VIEW` with the url and a media type, so the chooser offers video
// players rather than everything that claims http.
//
// Returns false and fills `error` when nothing on the device can open it, which
// is a real case -- an emulator image with no media app has nothing to offer, and
// silently doing nothing would look like the player failing to start.
bool open_media(const QUrl &url, const QString &mime, QString *error);

// `ACTION_VIEW` with the address and **no type**, which is the opposite
// decision from `open_media` and is the point of having both.
//
// `open_media` hands over a stream that has already been found, so naming a
// media type keeps browsers out of the chooser. This hands over a *page*, and
// the apps worth reaching are the ones registered for that host -- YouTube
// itself, VLC, NewPipe -- each of which resolves the page on its own. Forcing
// `video/*` would hide all of them behind players expecting a file.
bool open_externally(const QUrl &url, QString *error);

// The address this activity was asked to open, or an empty string.
//
// The other two hand a page *out*; this is the way in, and until the manifest
// grew a `VIEW` filter there was none at all -- hydra could not be opened from
// a link in another app, could not be offered as a browser, and the address
// bar was the only door. On a handset whose on-screen keyboard hides itself
// mid-address that is not a door either.
//
// **Destructive, and the name says so.** A launch intent stays attached to its
// activity for the life of the task, so reading it non-destructively returns
// the same url on every resume, and a browser that reopens the page you
// arrived on each time you come back to it is worse than one with no way in.
// Call it on activation as well as at startup: `singleTop` delivers a second
// request as `onNewIntent`, which Qt sets as the current intent.
QString take_view_url();

}  // namespace android_intents
