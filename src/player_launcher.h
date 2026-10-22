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
// mplayer is weak at HLS/DASH. That is no longer merely *reported* -- both
// kinds are assembled into one progressive file first (`hls_assembler`, served
// through the local proxy, sec 10/sec 11.3), so "only mplayer installed" still
// yields a seekable stream.
//
// **DASH was the one case still reported rather than compensated for, and is
// not any more.** What is left of it is narrower and is not reported from here:
// an MPD carrying its audio separately is refused by the assembler rather than
// turned into a silent file, and that cannot be known at the moment a warning
// would be written, because nothing has fetched the manifest yet.
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

	// **A player whose controls are its terminal is started in one.**
	// mplayer reads its keys -- pause, seek, quit, volume -- from the terminal
	// it was started from, and a browser has none to give it, so a detached
	// mplayer played and could not be paused. Reported 2026-10-22.
	//
	// `terminal_command()` is the terminal and the option that runs a
	// program, with the player's command line appended after it as separate
	// arguments: `xterm -e`, `gnome-terminal --`, `kitty`. Empty means the
	// first that is installed, `x-terminal-emulator -e` first, which Debian
	// requires to take exactly that form.
	QString terminal_command() const { return m_terminal; }
	void set_terminal_command(const QString &cmd) { m_terminal = cmd.trimmed(); }
	// Custom... is somebody's own command, so whether it wants a terminal is
	// theirs to say.
	bool custom_in_terminal() const { return m_custom_in_terminal; }
	void set_custom_in_terminal(bool on) { m_custom_in_terminal = on; }

	// The players that are controlled from their terminal.
	static bool controlled_from_terminal(const QString &id) {
		return id == "mplayer" || id == "mplayer2";
	}
	// The terminal as a program and its leading arguments, resolved: the
	// configured command when its program is found, otherwise the first
	// known terminal installed. Empty when there is none.
	static QStringList resolve_terminal(const QString &configured);
	// What is started: `program args` alone, or inside `terminal` when one is
	// given.
	static QStringList command_line(const QStringList &terminal,
	                                 const QString &program, const QStringList &args);

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
	// context instead of a naked request (sec 11.3). Referer and User-Agent
	// always; cookies only when a learned extractor named them, because
	// nothing observes the engine's jar -- see `media_dialog::watch`.
	// `via_audio` is `item.audio` as the player should fetch it, the same way.
	// `chapters_file`: a tracklist in ffmpeg's metadata format, for a player
	// that reads one (mpv); empty for none.
	bool play(const media_item &item, QString *error,
	           const QUrl &via = QUrl(), const QUrl &via_audio = QUrl(),
	           const QString &chapters_file = QString()) const;

	// The arguments a player is started with, apart from the program: the
	// stream, and the sound beside it where the player can take a second
	// input. `custom` is the custom command line, used for that player only.
	static QStringList arguments(const QString &id, const QString &custom,
	                              const QString &target, const QString &audio,
	                              const QString &chapters_file = QString());
	// Whether that player can be given a set's tracklist as chapters.
	static bool takes_chapters(const QString &id) { return id == "mpv"; }
	// Whether that player can play `audio` beside the picture.
	static bool takes_audio_beside(const QString &id);

private:
	const player_entry *entry(const QString &id) const;

	QList<player_entry> m_players;
	QString             m_selected;
	QString             m_custom;
	QString             m_terminal;
	bool                m_custom_in_terminal = false;
};
