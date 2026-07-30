// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "download_source.h"

#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QUrl>

struct download_job {
	int         id = 0;
	QUrl        url;
	QString     source_id;      // which transport took it ("http", "torrent"…)
	QString     path;           // destination on disk, once the source knows it
	QList<download_file> files; // multi-file jobs; empty for single-file
	QString     node_id;        // the tree node it came from (§11.2), may be empty
	qint64      received = 0;
	qint64      total    = -1;  // -1 while unknown, and it may stay unknown
	QString     detail;         // source's own words: "fetching metadata"…
	QString     error;
	download_state status = download_state::queued;

	// Copied from the source's capabilities at enqueue time so the UI can mark
	// the row without asking what transport this is (§11.4).
	bool public_participation = false;

	bool complete() const { return is_complete(status); }
	bool terminal() const { return is_terminal(status); }
};

// One queue fed by multiple sources (architecture doc §11.2, §11.4).
//
// The manager owns the queue, the destination directory, consent, and the job
// records. It does not own a transport: bytes are moved by a `download_source`,
// and the manager's job is to decide *which* source, *when* it may start, and
// what the user is told. Nothing here names HTTP or BitTorrent.
//
// Scheduling is per source rather than global, because "one at a time" is the
// right answer for HTTP and the wrong one for torrents — a swarm that is not
// connected is not downloading. Each source declares its own concurrency.
//
// Consent is enforced here rather than in the UI. A source whose participation
// is publicly observable (§11.4) cannot start until consent for that source has
// been given; the job waits and `consent_required` is emitted. That makes the
// privacy obligation structural: since Hydra ships no VPN and torrents are
// deliberately made to look like every other download, the one thing that must
// not be forgettable is telling the user that this one is different.
class download_manager : public QObject {
	Q_OBJECT
public:
	explicit download_manager(QObject *parent = nullptr);

	// Sources are tried in the order added; the first that accepts wins.
	// Takes ownership.
	void add_source(download_source *source);
	QList<download_source *> sources() const { return m_sources; }
	download_source *source_by_id(const QString &id) const;
	// Which source would take this URL, or null. Lets the UI label an action
	// before committing to it, still without naming a transport.
	download_source *source_for(const QUrl &url) const;

	void set_directory(const QString &dir) { m_dir = dir; }
	QString directory() const { return m_dir; }

	// Consent to a source whose participation is publicly observable. Granting
	// it releases any job of that source waiting on it.
	void set_consent(const QString &source_id, bool granted);
	bool has_consent(const QString &source_id) const;

	// Returns the job id, or 0 if no source would take it (see `error`).
	int enqueue(const QUrl &url, const QString &node_id, QString *error);
	void cancel(int id);
	void pause(int id);
	void unpause(int id);

	const QList<download_job> &jobs() const { return m_jobs; }

signals:
	void changed();   // any job's state or progress moved

	// A job is held because its source needs consent that has not been given.
	// The UI is expected to explain and then call set_consent().
	//
	// Emitted from inside enqueue(). A handler that opens a dialog should take
	// this on a queued connection, or enqueue() will not return until the user
	// has answered — which turns every call site into one that must survive a
	// nested event loop.
	void consent_required(const QString &source_id, const QString &note, int job_id);

private:
	void pump();                       // start what is allowed to start
	void sweep();                      // one pass; call pump(), not this
	int  live_count(const QString &source_id) const;
	download_job *find(int id);
	void on_progress(int id, const download_progress &p);
	void on_finished(int id, bool ok, const QString &message);

	QList<download_source *> m_sources;
	QList<download_job>      m_jobs;
	QSet<int>                m_live;      // handed to a source, not yet finished
	QSet<QString>            m_consented;
	int     m_next_id = 1;
	bool    m_pumping    = false;
	bool    m_pump_again = false;
	QString m_dir;
};
