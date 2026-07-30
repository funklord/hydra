// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "download_source.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

namespace libtorrent {
struct torrent_handle;
struct info_hash_t;
}

class QNetworkAccessManager;
class QTimer;

// BitTorrent as a first-class download source (architecture doc §11.4), on
// libtorrent-rasterbar.
//
// Why rasterbar and not rakshasa's library of the same name is argued in §11.4
// from the shipped headers: rakshasa's is a peer-scaling core with no magnet
// support, no µTP, no port mapping and no streaming piece order, all of which
// are what "works when you click the link" is made of. µTP decides it — without
// LEDBAT backoff a torrent saturates the uplink and makes its own browser
// unusable, which no first-class download may do.
//
// The scaling insight that made rakshasa's famous is respected by configuration
// rather than by library choice: `set_connection_limits()` exists precisely so
// the desktop-GUI defaults can be raised, since swarm throughput comes from
// holding many mostly-poor peers rather than from peak rate over a few good
// ones. Nothing here assumes the shipped defaults are right.
//
// Threading: libtorrent runs its own threads and hands work back as *alerts*.
// Rather than bridge threads, this polls the alert queue from a QTimer on the
// GUI thread, so every signal this class emits is emitted where the download
// manager and the UI already live. Alerts are cheap to drain and the poll is
// idle-friendly; correctness here is worth more than a few milliseconds of
// latency on a progress bar.
//
// Built optional, like libsodium: without HYDRA_HAVE_LIBTORRENT this compiles
// to a stub whose available() is false, and the shell simply does not add it as
// a source. There is no degraded mode — a torrent engine cannot be faked.
class torrent_download_source : public download_source {
	Q_OBJECT
public:
	explicit torrent_download_source(QObject *parent = nullptr);
	~torrent_download_source() override;

	// False when built without libtorrent. Nothing else on this class is
	// meaningful in that case.
	static bool available();

	// Non-web URL schemes this source claims, for the engine to be told about
	// before it starts (see qtwebengine_factory::register_url_schemes).
	//
	// Empty when there is no libtorrent, and deliberately so: registering
	// `magnet` with nothing behind it would make the engine route such links to
	// a handler that cannot serve them, silently swallowing the click. Better
	// to leave them unregistered and let the engine do whatever it does with an
	// unknown scheme.
	static QStringList url_schemes();

	QString id() const override { return "torrent"; }
	QString display_name() const override { return "BitTorrent"; }
	source_capabilities capabilities() const override;

	bool accepts(const QUrl &url, QString *why_not = nullptr) const override;
	bool start(const download_request &req, QString *error) override;
	void cancel(int id) override;
	void pause(int id) override;
	void unpause(int id) override;
	void prioritize_streaming(int id, const QString &file, bool on) override;
	qint64 contiguous_bytes(int id, const QString &file) const override;

	// --- Settings ---------------------------------------------------------
	// The §11.4 caps. `global` is the whole session; `per_torrent` bounds one
	// swarm. Defaults here are already well above a desktop GUI's, because the
	// ceiling that matters is the one you can actually reach.
	void set_connection_limits(int global, int per_torrent);

	// The §11.4 privacy decision made usable: Hydra ships no VPN, but a user
	// who configured one at the OS layer can bind the session to that
	// interface, and announces stop if it disappears. A settings field wired to
	// a libtorrent option — not a VPN implementation.
	void set_listen_interfaces(const QString &interfaces);

	// Seeding is a policy question, not a default (§11.4). Ratio <= 0 means
	// "stop as soon as the download completes".
	void set_seed_ratio(double ratio);

	// Sequential piece order for *every* torrent. Watch uses the per-job
	// prioritize_streaming() instead; this is the global default.
	void set_sequential(bool on);

	// Where resume data and the session state live.
	void set_state_directory(const QString &dir);

	// Ask every live torrent to write resume data now. Called at shutdown;
	// safe to call at any time.
	void save_all_resume_data();

private:
	void poll_alerts();
	void fetch_torrent_file(const download_request &req);
	bool add_params(const download_request &req, void *atp_ptr, QString *error);
	void report(int job_id, download_state state, const QString &detail);
#ifdef HYDRA_HAVE_LIBTORRENT
	download_progress final_progress(int job, const libtorrent::torrent_handle &h);
#endif
	QString resume_path(const QString &info_hash) const;
#ifdef HYDRA_HAVE_LIBTORRENT
	static QString hash_string(const libtorrent::info_hash_t &ih);
#endif

	struct impl;
	impl *m_d = nullptr;

	QNetworkAccessManager *m_net = nullptr;
	QTimer  *m_poll = nullptr;
	QString  m_state_dir;
	QString  m_listen_interfaces;
	double   m_seed_ratio     = 1.0;
	bool     m_sequential     = false;
	int      m_global_limit   = 800;
	int      m_torrent_limit  = 200;
};
