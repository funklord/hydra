#pragma once

#include "tab_history.h"

#include <QByteArray>
#include <QList>
#include <QString>

// Reading the tabs another browser has open (architecture doc sec 4).
//
// Tabs rather than bookmarks, deliberately: a bookmark is something a person
// filed once, and the thing worth bringing across is the working set they
// actually have in front of them.
//
// **Nothing here talks to a running browser.** Both sources are files the
// browser writes for its own crash recovery, so this reads what is on disk and
// never attaches to a process, needs no extension installed, and cannot disturb
// the browser it is reading from. The cost is that it sees the world as of the
// last time that browser flushed, which is stated per source below rather than
// implied.
namespace session_import {

struct imported_tab {
	QString title;
	QString url;
	int     window = 0;   // which of the browser's windows it was in
	bool    pinned = false;

	// Where this tab had been, and which entry it was on. A record, not a Back
	// button: only urls and titles ever cross from another browser -- no scroll
	// position, no form contents, no engine state -- because that is all one
	// browser's session file offers another and all that stays true once it is
	// written down.
	tab_history history;
};

// --- mozlz4 ---------------------------------------------------------------
//
// Firefox does not write plain JSON: the file is `mozLz40\0`, a little-endian
// uint32 of the decompressed size, then a raw **LZ4 block**.
//
// Implemented here rather than linked from liblz4, and that is a real decision
// rather than an oversight. As an optional dependency this feature would be
// absent on any machine without liblz4-dev -- including the one it was written
// on -- so it could not be tested where it was built, and "off unless you
// install something" is a poor answer for reading a file the user already has.
// The block format is small and every read and write here is bounds-checked;
// `test_session` decompresses a real 1.5 MB Firefox file and compares the
// result byte for byte against a reference implementation, which is the only
// reason to trust a decompressor somebody wrote by hand.
QByteArray mozlz4_decompress(const QByteArray &file, QString *error = nullptr);

// The LZ4 block alone, for the test to drive directly. `expected_size` comes
// from the container and bounds the output: a corrupt length cannot make this
// allocate wildly, and a block that wants to write past it is refused rather
// than trusted.
QByteArray lz4_block_decompress(const QByteArray &in, int expected_size,
                                 QString *error = nullptr);

// The built-in decoder, always compiled and always testable, even on a machine
// that has liblz4 and therefore does not use it. Keeping it reachable is the
// point: a fallback nothing exercises is a fallback that has stopped working
// without anyone finding out, which is this project's most-repeated defect.
QByteArray lz4_block_builtin(const QByteArray &in, int expected_size,
                              QString *error = nullptr);

// Which of the two the build is actually using, for the suite to report.
bool using_system_lz4();

// --- Firefox --------------------------------------------------------------

// The profile directory whose session is worth reading, or empty.
//
// **Not simply the one marked `Default=1`.** On the machine this was written on
// that entry names a profile containing four certificate databases and no
// session, no history and no bookmarks, while the profile actually in use is
// named by an `[Install...]` section with `Locked=1`. An importer that trusts
// `Default=1` there imports nothing and reports success.
QString firefox_profile(const QString &root = QString());

// Where that profile keeps the session Firefox would restore after a crash.
// Written periodically rather than on every change, so it lags a live browser;
// how far is a compiled-in Firefox pref this project could not read off disk.
QString firefox_session_path(const QString &profile);

// The open tabs in that file. Empty on any failure, with `error` set.
QList<imported_tab> firefox_tabs(const QString &session_file, QString *error = nullptr);

// Parse an already-decompressed session document. Split out so the JSON shape
// can be tested without a real profile.
QList<imported_tab> parse_firefox_session(const QByteArray &json, QString *error = nullptr);

// --- Chromium -------------------------------------------------------------
//
// Chromium does not write a document; it writes a **command log**. The file is
// `SNSS` plus a version, then a run of records -- a uint16 length, a one-byte
// command id, and a payload -- and the current set of tabs is what you get by
// *replaying* them in order. There is no snapshot to read.
//
// Every constant and every payload layout below was taken from Chromium's own
// source, which is vendored in this tree already because Qt WebEngine bundles
// it (`components/sessions/core/`), and then checked against a live file. That
// matters more here than for Firefox: this is versioned internal API with no
// stability promise, so the parser reports the version it saw and refuses one
// it does not know rather than guessing at a layout that has moved.
//
// Unlike the Firefox path there is **no reference implementation to compare
// against** -- nothing else on a normal machine reads these files. So the suite
// checks structure and plausibility, which is weaker, and says so.

QString chromium_profile(const QString &root = QString());
// The most recently written `Sessions/Session_*`, or empty.
QString chromium_session_path(const QString &profile);

// Replay a session file into the tabs it leaves open.
//
// `tab_records`, when given, is how many distinct tabs the log **mentioned**,
// which is not how many it returns. A tab that was opened and later closed is
// counted here and absent from the list, so the two together separate the only
// two things an empty result can mean: zero records is a reader that could not
// read the file, and records with no tabs is a browser that was left with
// nothing open. Nothing else on the machine can tell those apart, and reading
// it off the error message is reading prose.
QList<imported_tab> chromium_tabs(const QString &session_file, QString *error = nullptr,
                                   int *tab_records = nullptr);
// The same, over bytes already in hand, so the replay is testable without a
// Chromium profile.
QList<imported_tab> replay_snss(const QByteArray &file, QString *error = nullptr,
                                 int *tab_records = nullptr);

}  // namespace session_import
