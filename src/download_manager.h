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
	QString     source_id;      // which transport took it ("http", "torrent"...)
	QString     path;           // destination on disk, once the source knows it
	QList<download_file> files; // multi-file jobs; empty for single-file
	QString     node_id;        // the tree node it came from (sec 11.2), may be empty
	qint64      received = 0;
	qint64      total    = -1;  // -1 while unknown, and it may stay unknown
	QString     detail;         // source's own words: "fetching metadata"...
	QMap<QString, QString> headers;   // what this address needs sent with it
	QString     error;
	download_state status = download_state::queued;

	// Copied from the source's capabilities at enqueue time so the UI can mark
	// the row without asking what transport this is (sec 11.4).
	bool public_participation = false;

	bool complete() const { return is_complete(status); }
	bool terminal() const { return is_terminal(status); }
};

// One queue fed by multiple sources (architecture doc sec 11.2, sec 11.4).
//
// The manager owns the queue, the destination directory, consent, and the job
// records. It does not own a transport: bytes are moved by a `download_source`,
// and the manager's job is to decide *which* source, *when* it may start, and
// what the user is told. Nothing here names HTTP or BitTorrent.
//
// Scheduling is per source rather than global, because "one at a time" is the
// right answer for HTTP and the wrong one for torrents -- a swarm that is not
// connected is not downloading. Each source declares its own concurrency.
//
// Consent is enforced here rather than in the UI. A source whose participation
// is publicly observable (sec 11.4) cannot start until consent for that source has
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
	//
	// `headers` travel with the request to whichever source takes it -- what a
	// CDN wants to see before it will serve a stream (sec 11.3).
	int enqueue(const QUrl &url, const QString &node_id, QString *error,
	             const QMap<QString, QString> &headers = {});

	// Register a job whose transport is *already* running.
	//
	// enqueue() schedules: it picks a source, waits for a slot and calls
	// start(). A media capture (sec 11.6) is the other shape -- the page drives the
	// transfer and the manager only tracks it -- so there is nothing to schedule
	// and start() would have nothing to do. Everything after this point is
	// identical: the source reports through progressed()/finished() exactly as
	// any other, and the downloads window cannot tell the difference.
	//
	// Concurrency and consent are deliberately not applied. Nothing is being
	// queued, and the user began this by pressing the control that started it.
	int adopt(download_source *source, const QUrl &url, const QString &node_id);
	void cancel(int id);
	void pause(int id);
	void unpause(int id);
	// Drop a finished, failed or cancelled job from the list. Refuses a job
	// still going -- a running download must not vanish from view. Returns
	// whether a job was removed.
	bool forget(int id);

	// Drop every finished, failed or cancelled job at once, leaving anything
	// still going. Returns how many were removed -- and persists and signals
	// only when that is non-zero, so clearing an already-clean list is silent.
	int forget_finished();

	// Keep finished downloads across a restart. The list is in memory, so a
	// completed transfer vanished on exit; this writes the terminal rows to a
	// small file and reads them back. Set the path once and the manager saves
	// itself whenever the terminal set changes (a finish, a cancel, a remove).
	// Never persisted: the per-request headers, which carry the page's cookies
	// and auth, and any job still going -- only history is kept, never a fake
	// "running" row that cannot resume.
	void set_history_path(const QString &path) { m_history_path = path; }
	// Reports whether the write landed. It used to return void with
	// `commit()`'s answer discarded, so a history that could not be written
	// looked exactly like one that was.
	bool save_history(const QString &path) const;
	// False when the file is there and could not be read or did not parse.
	// An absent file is an ordinary first run and answers true. The caller is
	// expected to stop writing to a file it could not read: a damaged history
	// read as an empty one is overwritten by the next finished download, and
	// the rows that were in it are gone.
	bool load_history(const QString &path);

	const QList<download_job> &jobs() const { return m_jobs; }

signals:
	void changed();   // any job's state or progress moved

	// The automatic write behind `set_history_path` could not be made. This
	// class has no status bar to say so with, and the manager keeps its rows
	// in memory either way -- so the shell is told, and decides.
	void save_failed();

	// A job is held because its source needs consent that has not been given.
	// The UI is expected to explain and then call set_consent().
	//
	// Emitted from inside enqueue(). A handler that opens a dialog should take
	// this on a queued connection, or enqueue() will not return until the user
	// has answered -- which turns every call site into one that must survive a
	// nested event loop.
	void consent_required(const QString &source_id, const QString &note, int job_id);

private:
	void pump();                       // start what is allowed to start
	void sweep();                      // one pass; call pump(), not this
	int  live_count(const QString &source_id) const;
	download_job *find(int id);
	void on_progress(int id, const download_progress &p);
	void on_finished(int id, bool ok, const QString &message);
	void persist_history();

	QList<download_source *> m_sources;
	QList<download_job>      m_jobs;
	QSet<int>                m_live;      // handed to a source, not yet finished
	QSet<QString>            m_consented;
	int     m_next_id = 1;
	QString m_history_path;
	bool    m_pumping    = false;
	bool    m_pump_again = false;
	QString m_dir;
};
