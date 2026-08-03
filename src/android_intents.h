// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QUrl>

// Handing something to another app on Android (architecture doc §19).
//
// The desktop names a player and starts a process. Android has neither: apps are
// reached through intents, and which one answers is the system's business and the
// user's. That difference is why this is a separate file rather than a branch
// inside `player_launcher` — the *decision* about what to play is shared, and
// only the handoff is platform work.
namespace android_intents {

// `ACTION_VIEW` with the url and a media type, so the chooser offers video
// players rather than everything that claims http.
//
// Returns false and fills `error` when nothing on the device can open it, which
// is a real case — an emulator image with no media app has nothing to offer, and
// silently doing nothing would look like the player failing to start.
bool open_media(const QUrl &url, const QString &mime, QString *error);

}  // namespace android_intents
