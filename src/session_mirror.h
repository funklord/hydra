// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "session_import.h"

#include <QDateTime>
#include <QObject>
#include <QString>

class QTimer;

// Keeping another browser's tabs up to date, by watching the file it writes.
//
// **Polled, not watched.** `QFileSystemWatcher` looks like the right tool and
// is not: Firefox writes its session by creating a temporary file and renaming
// it over the old one, so the inode the watcher holds stops being the file, the
// watch fires exactly once and then never again. Watching the *directory*
// instead trades that for a different problem — every unrelated write in the
// profile wakes us. A timer that stats one path is duller and does not stop
// working.
//
// **A file change is not a tab change**, and this is the whole reason this
// class exists rather than a naked timer. Firefox rewrites that file constantly
// — scroll offsets, form state, which tab is focused — so refreshing on every
// write would rebuild the mirror every few seconds while the set of tabs sat
// completely still. So the parsed result is reduced to a fingerprint and the
// signal is only emitted when *that* changes. Cheap check first (size and
// mtime), expensive one only when it fires, and the answer discarded when it
// turns out to say the same thing.
class session_mirror : public QObject {
	Q_OBJECT
public:
	explicit session_mirror(QObject *parent = nullptr);

	// Off by default. This reads another program's files on a schedule, which
	// is not something to start doing because the feature exists.
	// `source` decides which reader runs: "firefox" or "chromium". One class
	// for both because everything around the reading -- the cheap stat, the
	// fingerprint, emitting only on a real change -- is identical, and two
	// copies of that would drift.
	void start(const QString &source, const QString &session_file,
	            int interval_ms = k_default_interval_ms);

	// **Measured, because the interval was a guess and the guess was wrong in a
	// direction nobody checked.** Both mirrors ran on 15 s while Chromium's
	// source flushes about every 2.5 s, and the note said nothing had measured
	// whether following it more closely was worth the reads. On a real 2.2 MB
	// session file holding 131 tabs:
	//
	//     stat only        0.006 ms per poll
	//     read + replay    1.3   ms per poll
	//
	// So the reads were never the constraint. A poll that finds nothing changed
	// costs six microseconds, and the whole of `replay_snss` over two megabytes
	// costs less than a frame. Chromium therefore polls at 5 s -- not 2.5 s,
	// which would run in lockstep with the writer for no freshness anyone can
	// perceive, and which is the interval most likely to catch a flush
	// half-written.
	//
	// A partial read is survivable rather than impossible: `replay_snss` treats
	// a truncated tail as normal, so a half-written file yields a *prefix* of
	// the tabs, the fingerprint changes, and the mirror briefly shows fewer.
	// The next poll restores it. That risk rises with polling frequency and has
	// not been measured; it is the reason for 5 s rather than something faster.
	static constexpr int k_default_interval_ms  = 15000;
	static constexpr int k_chromium_interval_ms = 5000;
	void stop();
	bool running() const;

	QString path() const { return m_path; }

	// What the timer is set to, exposed so a driver can catch the regression
	// that matters: the interval is passed at one call site, and dropping that
	// argument reverts Chromium to the 15 s default silently -- nothing fails,
	// the mirror is merely six times staler than it was measured to need.
	int interval_ms() const;

	// What the tabs currently reduce to. Exposed for the suite: the interesting
	// property is that two different *files* holding the same tabs produce the
	// same value, and that is otherwise only observable by waiting.
	static QString fingerprint(const QList<session_import::imported_tab> &tabs);

	// One poll, run now rather than on the timer. Returns whether the tab set
	// turned out to be different from the last one seen.
	bool poll_once();

signals:
	// Only when the tabs actually differ from the last set reported.
	void tabs_changed(const QList<session_import::imported_tab> &tabs);
	// Something went wrong reading the file. Emitted once per distinct message,
	// not once per poll: a browser that is closed would otherwise fill the
	// status bar with the same sentence four times a minute.
	void failed(const QString &message);

private:
	QTimer  *m_timer = nullptr;
	QString  m_source;
	QString  m_path;
	QString  m_fingerprint;
	QString  m_last_error;
	qint64   m_size = -1;
	QDateTime m_mtime;
};
