// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>
#include <QList>
#include <QStringList>
#include <QUrl>

// The transport seam for downloads (architecture doc §11.4).
//
// §11.2 already describes the manager as "one queue fed by multiple sources".
// Until now that was a description of where jobs *came from* while a single
// hard-wired QNetworkAccessManager moved the bytes. This interface makes it a
// description of how they *move*: the manager owns the queue, the destination
// directory, consent and the job records, and a source owns the bytes.
//
// It exists because BitTorrent is a first-class download source (§11.4) rather
// than a side feature, and a torrent is a badly-behaved guest in a model built
// for HTTP: it has no size until metadata resolves, writes several files at
// once, completes out of order, keeps running after it is "done", and is
// publicly observable in a way a GET is not. Every one of those is represented
// here rather than special-cased later. The same seam carries the Android
// handoff shape (§19.6), where the transport is another application entirely.
//
// The rule that keeps this honest: nothing above this interface may name a
// transport. If the manager or the UI has to ask "is this a torrent?", the
// abstraction has failed and the answer belongs in `source_capabilities`.

// Where a job is in its life. Wider than HTTP needs, deliberately.
//
// `resolving` is the magnet-link gap — the job exists and is working but has no
// size, no file list and no name yet. `seeding` is the opposite end: every byte
// has arrived and the file is usable, but the source has not let go. Both are
// states an HTTP download simply never enters, and both are real enough to a
// user watching a list that pretending otherwise would show a stalled row or a
// finished one that keeps using the network.
enum class download_state {
	queued,      // accepted, waiting for a slot
	resolving,   // working, but size/name/files not known yet
	running,     // transferring
	paused,      // stopped by the user, resumable
	seeding,     // complete and usable, source still active
	done,        // complete, source has let go
	failed,
	cancelled,
};

// True once the bytes are all on disk, whether or not the source has finished
// with the job. `seeding` is complete: the file can be opened.
inline bool is_complete(download_state s) {
	return s == download_state::done || s == download_state::seeding;
}

// True when nothing further will happen without the user asking.
inline bool is_terminal(download_state s) {
	return s == download_state::done || s == download_state::failed ||
	       s == download_state::cancelled;
}

// What a source can do, so the manager and the UI never have to test for a
// specific transport to behave correctly.
struct source_capabilities {
	// Can continue a partially-transferred job across restarts.
	bool resumable = false;

	// How many jobs this source runs at once. HTTP keeps the existing serial
	// behaviour (1); a torrent source is useless serialized, because a swarm
	// that is not connected is a swarm that is not downloading.
	int max_concurrent = 1;

	// Keeps working after the transfer completes. The manager must not treat
	// completion as permission to reclaim the job.
	bool seeds = false;

	// A job produces several files rather than one.
	bool multi_file = false;

	// Participation is visible to third parties. This is the §11.4 privacy
	// decision made structural: Hydra ships no VPN, so the obligation that
	// replaces it — explain before the first one, mark the rows, never start
	// from a page's initiative — has to be enforced somewhere that cannot be
	// forgotten. Making it a capability means the manager refuses to start
	// such a job without consent, rather than relying on a UI author knowing
	// that this particular transport announces the user's address to strangers.
	bool public_participation = false;

	// One line, shown when asking for that consent.
	QString participation_note;

	// The partial file is usable before the job finishes, so it can be played
	// while it downloads (§11.3). Not torrent-specific: an HTTP download is
	// written strictly front-to-back and is streamable for the same reason.
	bool streamable = false;
};

// One file inside a job. A bare path was enough while the list was only ever
// displayed, but choosing *which* file to play needs sizes: the feature in a
// multi-file torrent is the largest video, and a release that ships a sample
// clip first would otherwise be played instead of the film.
struct download_file {
	QString path;         // relative to the download directory
	qint64  size = -1;    // -1 when the source does not know
};

struct download_request {
	int     id = 0;
	QUrl    url;
	QString directory;   // where to write; the source chooses names within it
	QString node_id;     // the tree node it came from (§11.2), may be empty
};

// A source's report on a job. Fields left at their defaults mean "unchanged",
// except `state`, which is always meaningful.
struct download_progress {
	qint64      received = 0;
	qint64      total    = -1;   // -1 while unknown, and it may stay unknown
	QString     path;            // primary file, or the directory if multi-file
	QList<download_file> files;  // multi-file jobs only
	QString     detail;          // "fetching metadata", "seeding to 4 peers"…
	download_state state = download_state::running;
};

class download_source : public QObject {
	Q_OBJECT
public:
	explicit download_source(QObject *parent = nullptr) : QObject(parent) {}

	// Stable identifier used in job records and consent storage: "http",
	// "torrent", "handoff". Not shown to users.
	virtual QString id() const = 0;
	virtual QString display_name() const = 0;
	virtual source_capabilities capabilities() const = 0;

	// Can this source take this URL? On false, `why_not` may be set to a
	// user-facing reason. The reason lives here rather than in the manager
	// because the knowledge does: only the HTTP source knows that saving an
	// HLS manifest would write the playlist text and call it a video.
	virtual bool accepts(const QUrl &url, QString *why_not = nullptr) const = 0;

	// Begin. Returns false and sets `error` if the job could not be started at
	// all; anything that fails later is reported through finished().
	virtual bool start(const download_request &req, QString *error) = 0;

	virtual void cancel(int id) = 0;

	// Optional. A source that does not override these is one whose jobs cannot
	// be paused, which the manager learns from `resumable` rather than by
	// calling and watching nothing happen.
	virtual void pause(int id) { Q_UNUSED(id) }
	virtual void unpause(int id) { Q_UNUSED(id) }

	// Ask the source to fetch the front of the file first, so playback can
	// start before the job finishes. A source that already transfers in order
	// has nothing to do and ignores it.
	// `file` is one of the job's `files` entries, or empty for a single-file
	// job. Named explicitly rather than remembered, so two callers cannot
	// silently disagree about which file the answer below refers to.
	virtual void prioritize_streaming(int id, const QString &file, bool on) {
		Q_UNUSED(id) Q_UNUSED(file) Q_UNUSED(on)
	}

	// Bytes at the *start* of the job's primary file that are genuinely on disk
	// and readable, or -1 to mean "the file's own size is the truth".
	//
	// This exists because a file's size is not always a statement about what is
	// in it. A source that appends can use the size and does (-1). A torrent
	// allocates its files sparse and full-size from the outset, so the size is
	// available immediately while most of the content is holes that read as
	// **zeros** — handing that to a player would produce silence or garbage
	// rather than an honest short read.
	virtual qint64 contiguous_bytes(int id, const QString &file) const {
		Q_UNUSED(id) Q_UNUSED(file)
		return -1;
	}

signals:
	// Any movement, including state changes. Sources may emit this freely; the
	// manager coalesces into its own changed() signal.
	void progressed(int id, const download_progress &p);

	// The source has let go of the job entirely — for a seeding source this is
	// well after the bytes arrived. Terminal: no further signals for this id.
	void finished(int id, bool ok, const QString &message);
};
