// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

// Reading the tabs another browser has open (architecture doc §4).
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

}  // namespace session_import
