// Test doubles for the download_source seam.
#pragma once

#include "download_source.h"

#include <QHash>
#include <QList>
#include <QTimer>

// A source shaped like BitTorrent, with no BitTorrent in it. The point is to
// exercise every part of the seam that exists *for* torrents before libtorrent
// is a dependency: no size until metadata resolves, several files, concurrency,
// completion that is not the end, and publicly-observable participation.
class fake_torrent_source : public download_source {
	Q_OBJECT
public:
	using download_source::download_source;

	QString id() const override { return "torrent"; }
	QString display_name() const override { return "BitTorrent"; }

	source_capabilities capabilities() const override {
		source_capabilities c;
		c.resumable            = true;
		c.max_concurrent       = 3;
		c.seeds                = true;
		c.multi_file           = true;
		c.public_participation = true;
		c.streamable           = true;
		c.participation_note   = "A torrent announces your address to a tracker "
		                          "and to every peer in the swarm.";
		return c;
	}

	bool accepts(const QUrl &url, QString *why_not = nullptr) const override {
		if (url.scheme() == "magnet" || url.path().endsWith(".torrent"))
			return true;
		if (why_not)
			*why_not = "Not a magnet link or torrent file.";
		return false;
	}

	bool start(const download_request &req, QString *error) override {
		if (fail_on_start) {
			if (error)
				*error = "Refused on purpose.";
			return false;
		}
		m_live.insert(req.id, req);
		// A magnet has no metadata yet: no size, no name, no file list.
		download_progress p;
		p.state  = download_state::resolving;
		p.detail = "fetching metadata";
		p.total  = -1;
		emit progressed(req.id, p);
		return true;
	}

	// The real source reports a name as soon as the URI carries one.
	void name_it(int id, const QString &path) {
		download_progress p;
		p.state = download_state::resolving;
		p.total = -1;
		p.path  = path;
		p.detail = "fetching metadata";
		emit progressed(id, p);
	}

	// Drive the lifecycle by hand so tests stay deterministic.
	void resolve(int id, qint64 total, const QList<download_file> &files) {
		download_progress p;
		p.state    = download_state::running;
		p.total    = total;
		p.files    = files;
		p.path     = "/downloads/" + (files.isEmpty() ? QString()
		                                              : files.first().path);
		p.received = 0;
		p.detail   = "connected to 12 peers";
		emit progressed(id, p);
	}
	void advance(int id, qint64 received, qint64 total) {
		download_progress p;
		p.state    = download_state::running;
		p.received = received;
		p.total    = total;
		emit progressed(id, p);
	}
	// Every byte has arrived, but the source has not let go.
	void complete_into_seeding(int id, qint64 total) {
		download_progress p;
		p.state    = download_state::seeding;
		p.received = total;
		p.total    = total;
		p.detail   = "seeding to 4 peers";
		emit progressed(id, p);
	}
	void stop_seeding(int id) {
		m_live.remove(id);
		emit finished(id, true, QString());
	}
	void fail(int id, const QString &why) {
		m_live.remove(id);
		emit finished(id, false, why);
	}

	void cancel(int id) override {
		if (!m_live.contains(id))
			return;
		m_live.remove(id);
		emit finished(id, false, "Cancelled.");
	}

	void pause(int id) override {
		download_progress p;
		p.state = download_state::paused;
		emit progressed(id, p);
		++pause_calls;
	}
	void unpause(int id) override {
		download_progress p;
		p.state = download_state::running;
		emit progressed(id, p);
		++unpause_calls;
	}

	bool live(int id) const { return m_live.contains(id); }
	int  pause_calls = 0;
	int  unpause_calls = 0;
	bool fail_on_start = false;

private:
	QHash<int, download_request> m_live;
};

// A source that fails synchronously inside start(), which lands in the
// manager's on_finished() mid-pump. Exists to prove the re-entrancy guard.
class exploding_source : public download_source {
	Q_OBJECT
public:
	using download_source::download_source;
	QString id() const override { return "boom"; }
	QString display_name() const override { return "Exploding"; }
	source_capabilities capabilities() const override { return {}; }
	bool accepts(const QUrl &url, QString *) const override {
		return url.scheme() == "boom";
	}
	bool start(const download_request &req, QString *) override {
		emit finished(req.id, false, "Detonated during start().");
		return true;   // reported failure through the signal, not the return
	}
	void cancel(int) override {}
};

// A plain, non-public source: the "every other download" a torrent row has to
// be visibly different from.
class fake_plain_source : public download_source {
	Q_OBJECT
public:
	using download_source::download_source;
	QString id() const override { return "plain"; }
	QString display_name() const override { return "Direct download"; }
	source_capabilities capabilities() const override {
		source_capabilities c;
		c.resumable = true;
		c.max_concurrent = 4;
		return c;
	}
	bool accepts(const QUrl &url, QString * = nullptr) const override {
		return url.scheme().startsWith("http");
	}
	bool start(const download_request &req, QString *) override {
		m_live.insert(req.id, req);
		return true;
	}
	void cancel(int id) override { m_live.remove(id); emit finished(id, false, "Cancelled."); }
	void pause(int id) override {
		download_progress p; p.state = download_state::paused; emit progressed(id, p);
	}
	void set(int id, download_state st, qint64 got, qint64 total,
	          const QString &path, const QString &detail) {
		download_progress p;
		p.state = st; p.received = got; p.total = total; p.path = path; p.detail = detail;
		emit progressed(id, p);
	}
	void finish(int id, bool ok, const QString &msg) {
		m_live.remove(id); emit finished(id, ok, msg);
	}
private:
	QHash<int, download_request> m_live;
};
