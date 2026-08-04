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
	void start(const QString &session_file, int interval_ms = 15000);
	void stop();
	bool running() const;

	QString path() const { return m_path; }

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
	QString  m_path;
	QString  m_fingerprint;
	QString  m_last_error;
	qint64   m_size = -1;
	QDateTime m_mtime;
};
