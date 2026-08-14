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

// Hands a stream to the user's own player (architecture doc sec 11.3).
//
// Two design points from sec 11.3 that are easy to get wrong:
//
//  * Never assume a player exists. The set is probed from PATH, the settings UI
//    shows what is supported with the missing ones greyed out, and the default
//    is resolved from what is actually present -- on a machine with only
//    mplayer, mplayer is chosen.
//  * Hand the player a URL, never a stdin pipe. A pipe has no random access, so
//    `mplayer -` cannot seek; a URL lets the player issue its own range or
//    segment requests and seekability follows the source.
//
// Capability-aware routing: mpv and VLC take a manifest directly; classic
// mplayer is weak at HLS/DASH. That is no longer merely *reported* -- HLS is
// assembled into one progressive file first (`hls_assembler`, served through
// the local proxy, sec 10/sec 11.3), so "only mplayer installed" still yields a
// seekable stream. DASH has no assembly step yet and is the one case still
// reported rather than compensated for.
//
// A **Custom...** player is deliberately treated as *not* handling manifests,
// because nothing here knows what it is. Assembling first works for every
// player and costs only effort; assuming a capability that turns out to be
// missing fails at playback, where the user has no way to tell why.
class player_launcher {
public:
	player_launcher();

	// Re-probe PATH. Cheap; call on startup and on demand.
	void refresh();

	const QList<player_entry> &players() const { return m_players; }
	QList<player_entry> installed() const;

	// The chosen player id, resolved from what is installed. `custom_id()` is
	// the "Custom..." entry, which is driven by a command template instead of a
	// built-in argument list.
	QString selected() const { return m_selected; }
	void set_selected(const QString &id) { m_selected = id; }

	static const char *custom_id() { return "custom"; }

	// The Android entry: hand the url to whatever app the system offers, over an
	// `ACTION_VIEW` intent. There is no PATH to probe there and no process to
	// start, so this is the *only* entry on that platform -- and it is always
	// "installed", because the chooser is, even if nothing behind it is.
	static const char *system_id() { return "system"; }

	// A command line with `%U` where the stream URL goes. Split on whitespace,
	// which is enough for a player invocation and keeps the field honest about
	// what it supports -- a shell would invite quoting bugs and injection for no
	// benefit here. A template with no `%U` gets the URL appended, since that
	// is what someone typing just a program name means.
	QString custom_command() const { return m_custom; }
	void set_custom_command(const QString &cmd);

	// Empty when the launch is expected to work. Otherwise a reason to show the
	// user -- no player, or a player that cannot handle this stream well.
	QString warning_for(const media_item &item) const;

	// True when the selected player takes an HLS/DASH manifest directly.
	// False means the stream must be assembled for it first (sec 11.3).
	bool selected_handles_streams() const;

	// Launches detached so a player outliving the browser is fine.
	// Returns false and fills `error` if it could not start.
	// `via` overrides the URL handed to the player -- the local proxy's
	// localhost URL when one is available, so the CDN sees the page's own
	// Referer and cookies instead of a naked request (sec 11.3).
	bool play(const media_item &item, QString *error,
	           const QUrl &via = QUrl()) const;

private:
	const player_entry *entry(const QString &id) const;

	QList<player_entry> m_players;
	QString             m_selected;
	QString             m_custom;
};
