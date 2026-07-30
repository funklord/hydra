// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "media_detector.h"

#include <QList>
#include <QString>
#include <QUrl>

// One known external player.
struct player_entry {
	QString id;          // "mpv"
	QString label;       // "mpv"
	QString path;        // resolved absolute path, empty if not installed
	bool    installed = false;
	bool    native_streams = false;   // handles HLS/DASH manifests itself
};

// Hands a stream to the user's own player (architecture doc §11.3).
//
// Two design points from §11.3 that are easy to get wrong:
//
//  * Never assume a player exists. The set is probed from PATH, the settings UI
//    shows what is supported with the missing ones greyed out, and the default
//    is resolved from what is actually present — on a machine with only
//    mplayer, mplayer is chosen.
//  * Hand the player a URL, never a stdin pipe. A pipe has no random access, so
//    `mplayer -` cannot seek; a URL lets the player issue its own range or
//    segment requests and seekability follows the source.
//
// Capability-aware routing: mpv and VLC take a manifest directly, classic
// mplayer is weak at HLS/DASH and would ideally be handed an assembled
// progressive stream by the local proxy (§10). That proxy does not exist yet,
// so this reports the limitation rather than silently handing mplayer a
// manifest it will stumble on.
class player_launcher {
public:
	player_launcher();

	// Re-probe PATH. Cheap; call on startup and on demand.
	void refresh();

	const QList<player_entry> &players() const { return m_players; }
	QList<player_entry> installed() const;

	// The chosen player id, resolved from what is installed.
	QString selected() const { return m_selected; }
	void set_selected(const QString &id) { m_selected = id; }

	// Empty when the launch is expected to work. Otherwise a reason to show the
	// user — no player, or a player that cannot handle this stream well.
	QString warning_for(const media_item &item) const;

	// True when the selected player takes an HLS/DASH manifest directly.
	// False means the stream must be assembled for it first (§11.3).
	bool selected_handles_streams() const;

	// Launches detached so a player outliving the browser is fine.
	// Returns false and fills `error` if it could not start.
	// `via` overrides the URL handed to the player — the local proxy's
	// localhost URL when one is available, so the CDN sees the page's own
	// Referer and cookies instead of a naked request (§11.3).
	bool play(const media_item &item, QString *error,
	           const QUrl &via = QUrl()) const;

private:
	const player_entry *entry(const QString &id) const;

	QList<player_entry> m_players;
	QString             m_selected;
};
