// SPDX-License-Identifier: GPL-3.0-or-later
#include "torrent_download_source.h"

#include <QDir>
#include <QList>
#include <QSet>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#ifdef HYDRA_HAVE_LIBTORRENT

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/write_resume_data.hpp>

#include <memory>
#include <vector>

namespace lt = libtorrent;

struct torrent_download_source::impl {
	std::unique_ptr<lt::session> session;
	// libtorrent identifies a torrent by handle; the manager identifies a job
	// by int. Handles arrive back on alerts, so both directions are needed.
	QHash<quint32, int>          job_of_handle;
	QHash<int, lt::torrent_handle> handle_of_job;
	QHash<int, QString>          save_path;
	QSet<int>                    completed;   // reached seeding, not yet released
	QSet<int>                    files_sent;  // file list emitted once per job
	QSet<int>                    streaming;   // jobs asked to prioritise playback
	QHash<int, lt::file_index_t> primary_file;// which file Watch is aiming at
	QList<int>                   pending;     // async_add_torrent in flight, FIFO
};

bool torrent_download_source::available() { return true; }

#else   // ------------------------------------------------------------ stub --

struct torrent_download_source::impl {};
bool torrent_download_source::available() { return false; }

#endif

torrent_download_source::torrent_download_source(QObject *parent)
	: download_source(parent) {
	m_d = new impl;
	m_state_dir = QDir(QStandardPaths::writableLocation(
	                        QStandardPaths::AppDataLocation))
	                   .filePath("torrents");

#ifdef HYDRA_HAVE_LIBTORRENT
	lt::settings_pack sp;

	// Only ask for the alerts we act on. The default mask is chatty, and every
	// alert we do not read is work done for nothing.
	sp.set_int(lt::settings_pack::alert_mask,
	            lt::alert_category::status | lt::alert_category::error |
	            lt::alert_category::storage);

	// §11.4: the caps are the whole point of the connection-scaling argument,
	// so set them deliberately rather than inheriting a desktop-GUI default.
	sp.set_int(lt::settings_pack::connections_limit, m_global_limit);
	sp.set_int(lt::settings_pack::active_downloads, -1);
	sp.set_int(lt::settings_pack::active_seeds, -1);
	sp.set_int(lt::settings_pack::active_limit, -1);

	// The reason rasterbar was chosen (§11.4). µTP's LEDBAT congestion control
	// yields to competing traffic, so a torrent does not starve the browser it
	// is running inside. Explicit because it is load-bearing, not incidental.
	sp.set_bool(lt::settings_pack::enable_outgoing_utp, true);
	sp.set_bool(lt::settings_pack::enable_incoming_utp, true);

	// Peer discovery that makes a magnet link work without configuration.
	sp.set_bool(lt::settings_pack::enable_dht, true);
	sp.set_bool(lt::settings_pack::enable_lsd, true);

	// Port mapping is a security posture change — asking the router to open a
	// hole (§11.4). On by default because inbound connectivity is most of what
	// makes a swarm usable, but it is a setting, and it is written down.
	sp.set_bool(lt::settings_pack::enable_upnp, true);
	sp.set_bool(lt::settings_pack::enable_natpmp, true);

	sp.set_str(lt::settings_pack::user_agent, "Hydra/0.1 libtorrent/2.0");

	m_d->session = std::make_unique<lt::session>(lt::session_params(sp));
#endif

	m_net = new QNetworkAccessManager(this);

	// libtorrent works on its own threads and reports through alerts. Draining
	// them here keeps every signal on the GUI thread, where the manager lives.
	m_poll = new QTimer(this);
	m_poll->setInterval(500);
	connect(m_poll, &QTimer::timeout, this, &torrent_download_source::poll_alerts);
	if (available())
		m_poll->start();
}

torrent_download_source::~torrent_download_source() {
#ifdef HYDRA_HAVE_LIBTORRENT
	if (m_d->session) {
		save_all_resume_data();
		// Draining once gives the save_resume_data alerts a chance to land.
		poll_alerts();
	}
#endif
	delete m_d;
}

QStringList torrent_download_source::url_schemes() {
	return available() ? QStringList{ "magnet" } : QStringList{};
}

source_capabilities torrent_download_source::capabilities() const {
	source_capabilities c;
	c.resumable            = true;
	c.seeds                = true;
	c.multi_file           = true;
	c.public_participation = true;
	// Several swarms at once is the normal case; serializing torrents would
	// mean a swarm that is not connected, which is a swarm not downloading.
	c.max_concurrent       = 8;
	c.streamable           = true;   // sequential + deadlines make it playable
	c.participation_note   =
		"A torrent is not a private download. Your IP address is announced to "
		"the tracker and to every peer in the swarm, and peers can see what you "
		"are fetching. Hydra does not tunnel this — if you want it hidden, set "
		"up a VPN or proxy at the system level.";
	return c;
}

bool torrent_download_source::accepts(const QUrl &url, QString *why_not) const {
	if (!available()) {
		if (why_not)
			*why_not = "This build has no BitTorrent support.";
		return false;
	}
	if (url.scheme().compare("magnet", Qt::CaseInsensitive) == 0)
		return true;
	if (url.path().endsWith(".torrent", Qt::CaseInsensitive))
		return true;
	if (why_not)
		*why_not = "Not a magnet link or .torrent file.";
	return false;
}

// Resume data is keyed by **info-hash**, never by job id. Job ids are handed
// out by the manager starting from 1 in each session, so a file named after one
// would be loaded by an unrelated torrent after a restart — resurrecting some
// previous download's piece state onto a new job. The info-hash is the identity
// of the content itself and is the only stable name available.
QString torrent_download_source::resume_path(const QString &info_hash) const {
	return QDir(m_state_dir).filePath(info_hash + ".resume");
}

#ifdef HYDRA_HAVE_LIBTORRENT
QString torrent_download_source::hash_string(const lt::info_hash_t &ih) {
	const lt::sha1_hash h = ih.get_best();
	return QByteArray(h.data(), int(h.size())).toHex();
}
#endif

void torrent_download_source::set_state_directory(const QString &dir) {
	m_state_dir = dir;
}

void torrent_download_source::set_connection_limits(int global, int per_torrent) {
	m_global_limit  = global;
	m_torrent_limit = per_torrent;
#ifdef HYDRA_HAVE_LIBTORRENT
	if (m_d->session) {
		lt::settings_pack sp;
		sp.set_int(lt::settings_pack::connections_limit, global);
		m_d->session->apply_settings(sp);
	}
	for (auto it = m_d->handle_of_job.cbegin(); it != m_d->handle_of_job.cend(); ++it)
		if (it.value().is_valid())
			it.value().set_max_connections(per_torrent);
#endif
}

void torrent_download_source::set_listen_interfaces(const QString &interfaces) {
	m_listen_interfaces = interfaces;
#ifdef HYDRA_HAVE_LIBTORRENT
	if (m_d->session && !interfaces.isEmpty()) {
		lt::settings_pack sp;
		sp.set_str(lt::settings_pack::listen_interfaces,
		            interfaces.toStdString());
		m_d->session->apply_settings(sp);
	}
#endif
}

void torrent_download_source::set_seed_ratio(double ratio) { m_seed_ratio = ratio; }

void torrent_download_source::set_sequential(bool on) {
	m_sequential = on;
#ifdef HYDRA_HAVE_LIBTORRENT
	for (auto it = m_d->handle_of_job.cbegin(); it != m_d->handle_of_job.cend(); ++it) {
		if (!it.value().is_valid())
			continue;
		if (on)
			it.value().set_flags(lt::torrent_flags::sequential_download);
		else
			it.value().unset_flags(lt::torrent_flags::sequential_download);
	}
#endif
}

void torrent_download_source::report(int job_id, download_state state,
                                      const QString &detail) {
	download_progress p;
	p.state  = state;
	p.detail = detail;
	emit progressed(job_id, p);
}

// ---------------------------------------------------------------------------

bool torrent_download_source::start(const download_request &req, QString *error) {
#ifndef HYDRA_HAVE_LIBTORRENT
	Q_UNUSED(req)
	if (error)
		*error = "This build has no BitTorrent support.";
	return false;
#else
	if (!m_d->session) {
		if (error)
			*error = "The BitTorrent session failed to start.";
		return false;
	}
	QDir().mkpath(m_state_dir);
	QDir().mkpath(req.directory);

	// A .torrent behind an http(s) URL has to be fetched before it can be
	// added — libtorrent 2.0 does not fetch them itself. A magnet needs no
	// fetch, which is why it is the fast path.
	if (req.url.scheme().compare("magnet", Qt::CaseInsensitive) != 0 &&
	    (req.url.scheme().startsWith("http"))) {
		fetch_torrent_file(req);
		report(req.id, download_state::resolving, "fetching .torrent");
		return true;
	}

	lt::add_torrent_params atp;
	if (!add_params(req, &atp, error))
		return false;

	m_d->save_path.insert(req.id, req.directory);
	m_d->pending.push_back(req.id);

	// Report a name now if the URI carried one. A magnet's `dn` parameter is
	// exactly that, and without it the list has nothing to show but the raw
	// magnet string until metadata arrives — which can be a long wait on a
	// cold swarm. Naming is the source's job, not the UI's: the window must
	// not have to know what a magnet link looks like.
	download_progress p;
	p.state  = download_state::resolving;
	p.detail = req.url.scheme() == "magnet" ? "fetching metadata" : "checking files";
	if (!atp.name.empty())
		p.path = QDir(req.directory).filePath(QString::fromStdString(atp.name));

	m_d->session->async_add_torrent(std::move(atp));
	emit progressed(req.id, p);
	return true;
#endif
}

// Fills an add_torrent_params for `req`. `atp_ptr` is void* so the header does
// not have to name a libtorrent type — the seam's rule is that nothing above
// download_source knows what transport this is, and that is cheapest to keep
// true if the header stays clean.
bool torrent_download_source::add_params(const download_request &req,
                                          void *atp_ptr, QString *error) {
#ifndef HYDRA_HAVE_LIBTORRENT
	Q_UNUSED(req) Q_UNUSED(atp_ptr) Q_UNUSED(error)
	return false;
#else
	auto *atp = static_cast<lt::add_torrent_params *>(atp_ptr);

	// Parse first. The info-hash the URI yields is what names the resume file,
	// so it has to be known before there is anywhere to look.
	{
		if (req.url.scheme().compare("magnet", Qt::CaseInsensitive) == 0) {
			lt::error_code ec;
			*atp = lt::parse_magnet_uri(req.url.toString().toStdString(), ec);
			if (ec) {
				if (error)
					*error = QString("Bad magnet link: %1")
					              .arg(QString::fromStdString(ec.message()));
				return false;
			}
		} else {
			// A local .torrent file.
			const QString path = req.url.isLocalFile() ? req.url.toLocalFile()
			                                           : req.url.toString();
			if (!QFile::exists(path)) {
				if (error)
					*error = "No such .torrent file: " + path;
				return false;
			}
			try {
				*atp = lt::load_torrent_file(path.toStdString());
			} catch (const std::exception &e) {
				if (error)
					*error = QString("Unreadable .torrent: %1").arg(e.what());
				return false;
			}
		}
	}

	// Now that the identity is known, layer any saved state on top. Resume
	// data carries the info-hash, the file priorities and what is already on
	// disk, so it wins over the freshly parsed URI where they overlap.
	const QString hash = hash_string(atp->info_hashes);
	const QString rp   = resume_path(hash);
	if (!hash.isEmpty() && QFile::exists(rp)) {
		QFile f(rp);
		if (f.open(QIODevice::ReadOnly)) {
			const QByteArray blob = f.readAll();
			lt::error_code ec;
			lt::add_torrent_params resumed =
				lt::read_resume_data({blob.constData(), blob.size()}, ec);
			// Only accept it if it is genuinely the same content. A corrupt or
			// stale file must not silently redirect the download.
			if (!ec && hash_string(resumed.info_hashes) == hash) {
				if (!resumed.ti && atp->ti)
					resumed.ti = atp->ti;
				*atp = std::move(resumed);
			}
		}
	}

	atp->save_path = req.directory.toStdString();
	if (m_sequential)
		atp->flags |= lt::torrent_flags::sequential_download;
	return true;
#endif
}

void torrent_download_source::fetch_torrent_file(const download_request &req) {
#ifdef HYDRA_HAVE_LIBTORRENT
	QNetworkReply *reply = m_net->get(QNetworkRequest(req.url));
	const download_request copy = req;
	connect(reply, &QNetworkReply::finished, this, [this, reply, copy] {
		reply->deleteLater();
		if (reply->error() != QNetworkReply::NoError) {
			emit finished(copy.id, false,
			               "Could not fetch the .torrent: " + reply->errorString());
			return;
		}
		const QByteArray blob = reply->readAll();
		lt::add_torrent_params atp;
		try {
			atp = lt::load_torrent_buffer({blob.constData(), blob.size()});
		} catch (const std::exception &e) {
			emit finished(copy.id, false,
			               QString("That file is not a torrent: %1").arg(e.what()));
			return;
		}
		atp.save_path = copy.directory.toStdString();
		if (m_sequential)
			atp.flags |= lt::torrent_flags::sequential_download;
		m_d->save_path.insert(copy.id, copy.directory);
		m_d->pending.push_back(copy.id);
		m_d->session->async_add_torrent(std::move(atp));
		report(copy.id, download_state::resolving, "checking files");
	});
#else
	Q_UNUSED(req)
#endif
}

void torrent_download_source::cancel(int id) {
#ifdef HYDRA_HAVE_LIBTORRENT
	auto it = m_d->handle_of_job.find(id);
	if (it == m_d->handle_of_job.end()) {
		// Cancelled before the add landed. The pending entry must go too, or
		// every later add_torrent_alert is matched to the wrong job.
		m_d->pending.removeAll(id);
		m_d->save_path.remove(id);
		emit finished(id, false, "Cancelled.");
		return;
	}
	if (it.value().is_valid()) {
		m_d->job_of_handle.remove(it.value().id());
		// Keep the partial data: cancel means "stop", and a half-finished
		// download the user might resume is worth more than a tidy directory.
		m_d->session->remove_torrent(it.value());
	}
	m_d->handle_of_job.erase(it);
	m_d->completed.remove(id);
	emit finished(id, false, "Cancelled.");
#else
	emit finished(id, false, "This build has no BitTorrent support.");
#endif
}

void torrent_download_source::pause(int id) {
#ifdef HYDRA_HAVE_LIBTORRENT
	auto it = m_d->handle_of_job.find(id);
	if (it != m_d->handle_of_job.end() && it.value().is_valid()) {
		it.value().unset_flags(lt::torrent_flags::auto_managed);
		it.value().pause();
		report(id, download_state::paused, "paused");
	}
#else
	Q_UNUSED(id)
#endif
}

void torrent_download_source::unpause(int id) {
#ifdef HYDRA_HAVE_LIBTORRENT
	auto it = m_d->handle_of_job.find(id);
	if (it != m_d->handle_of_job.end() && it.value().is_valid()) {
		it.value().set_flags(lt::torrent_flags::auto_managed);
		it.value().resume();
		report(id, download_state::running, QString());
	}
#else
	Q_UNUSED(id)
#endif
}

#ifdef HYDRA_HAVE_LIBTORRENT
namespace {

// Map one of the job's reported file paths back to its index. The reported
// paths come from file_storage::file_path(), so comparing against the same
// call is exact rather than a guess at path normalisation.
lt::file_index_t file_index_for(const lt::file_storage &fs, const QString &file) {
	if (file.isEmpty())
		return lt::file_index_t(0);
	for (lt::file_index_t i : fs.file_range()) {
		if (QString::fromStdString(fs.file_path(i)) == file)
			return i;
	}
	return lt::file_index_t(0);
}

}  // namespace
#endif

void torrent_download_source::prioritize_streaming(int id, const QString &file,
                                                    bool on) {
#ifdef HYDRA_HAVE_LIBTORRENT
	auto it = m_d->handle_of_job.find(id);
	if (it == m_d->handle_of_job.end() || !it.value().is_valid())
		return;
	lt::torrent_handle h = it.value();
	if (!on) {
		h.unset_flags(lt::torrent_flags::sequential_download);
		m_d->streaming.remove(id);
		return;
	}

	// Sequential order alone gets the front of the file first, but it does not
	// promise *when*. Deadlines do: they tell libtorrent these pieces are
	// wanted soon, which is what turns "downloads in order" into "starts
	// playing quickly" on a swarm that would otherwise trickle.
	h.set_flags(lt::torrent_flags::sequential_download);
	m_d->streaming.insert(id);

	const std::shared_ptr<const lt::torrent_info> ti = h.torrent_file();
	if (!ti)
		return;   // magnet with no metadata yet; the flag applies when it lands
	const lt::file_storage &fs = ti->files();
	const lt::file_index_t idx = file_index_for(fs, file);
	if (idx < lt::file_index_t(0) || idx >= fs.end_file())
		return;
	m_d->primary_file.insert(id, idx);

	// Deadline the first slice of the chosen file, in ascending order, so the
	// player has something to open almost immediately.
	const lt::peer_request first = fs.map_file(idx, 0, 1);
	const int piece_len = fs.piece_length();
	const qint64 lead   = qint64(piece_len) * 24;   // ~a few seconds of video
	const lt::peer_request last = fs.map_file(
		idx, qMin<qint64>(lead, qMax<qint64>(0, fs.file_size(idx) - 1)), 1);
	int deadline = 0;
	for (lt::piece_index_t p = first.piece; p <= last.piece; ++p)
		h.set_piece_deadline(p, (deadline += 200));
#else
	Q_UNUSED(id) Q_UNUSED(on)
#endif
}

qint64 torrent_download_source::contiguous_bytes(int id, const QString &file) const {
#ifdef HYDRA_HAVE_LIBTORRENT
	auto it = m_d->handle_of_job.constFind(id);
	if (it == m_d->handle_of_job.constEnd() || !it.value().is_valid())
		return -1;
	const lt::torrent_handle h = it.value();
	const std::shared_ptr<const lt::torrent_info> ti = h.torrent_file();
	if (!ti)
		return 0;

	const lt::file_storage &fs = ti->files();
	const lt::file_index_t idx = file_index_for(fs, file);
	if (idx < lt::file_index_t(0) || idx >= fs.end_file())
		return -1;

	const qint64 file_off  = fs.file_offset(idx);
	const qint64 file_size = fs.file_size(idx);
	const int    piece_len = fs.piece_length();

	// Walk forward from the piece the file starts in and stop at the first hole.
	// Everything past that point is a sparse gap that reads as zeros, and a
	// player told it exists will render the zeros rather than wait for them.
	const lt::piece_index_t first = fs.map_file(idx, 0, 1).piece;
	lt::piece_index_t p = first;
	while (p < ti->end_piece() && h.have_piece(p))
		++p;

	// Absolute byte just past the last complete piece, converted to an offset
	// within this file. A partial first piece cannot count: the file may start
	// mid-piece, so the usable prefix begins where the file does.
	const qint64 have_to = qint64(static_cast<int>(p)) * qint64(piece_len);
	return qBound<qint64>(0, have_to - file_off, file_size);
#else
	Q_UNUSED(id)
	return -1;
#endif
}

void torrent_download_source::save_all_resume_data() {
#ifdef HYDRA_HAVE_LIBTORRENT
	for (auto it = m_d->handle_of_job.cbegin(); it != m_d->handle_of_job.cend(); ++it) {
		if (!it.value().is_valid())
			continue;
		const lt::torrent_status st = it.value().status();
		if (st.has_metadata)
			it.value().save_resume_data(lt::torrent_handle::save_info_dict);
	}
#endif
}

// ---------------------------------------------------------------------------

#ifdef HYDRA_HAVE_LIBTORRENT
// The last word on a job: every byte accounted for, with the path and file
// list, so a completed row is fully populated no matter when it completed
// relative to the status poll.
download_progress torrent_download_source::final_progress(int job,
                                                           const lt::torrent_handle &h) {
	const lt::torrent_status st = h.status();
	download_progress p;
	p.state    = download_state::done;
	p.received = st.total_wanted_done;
	p.total    = st.total_wanted;
	if (!st.name.empty())
		p.path = QDir(m_d->save_path.value(job))
		              .filePath(QString::fromStdString(st.name));
	if (auto ti = h.torrent_file()) {
		const lt::file_storage &fs = ti->files();
		for (lt::file_index_t i : fs.file_range()) {
			if (fs.pad_file_at(i))
				continue;          // alignment padding, not content
			p.files << QString::fromStdString(fs.file_path(i));
		}
	}
	return p;
}
#endif

void torrent_download_source::poll_alerts() {
#ifdef HYDRA_HAVE_LIBTORRENT
	if (!m_d->session)
		return;

	std::vector<lt::alert *> alerts;
	m_d->session->pop_alerts(&alerts);

	for (lt::alert *a : alerts) {
		if (auto *added = lt::alert_cast<lt::add_torrent_alert>(a)) {
			// add_torrent_alert carries no id of ours, so the job it belongs to
			// has to be recovered. libtorrent posts these in the same order the
			// async_add_torrent calls were made, so a FIFO of pending job ids
			// is exact — and unlike matching on info-hash it still works for a
			// magnet, whose hash we would have to parse twice to know.
			if (m_d->pending.isEmpty())
				continue;
			const int job = m_d->pending.takeFirst();
			if (added->error) {
				emit finished(job, false,
				               QString::fromStdString(added->error.message()));
				m_d->save_path.remove(job);
				continue;
			}
			m_d->handle_of_job.insert(job, added->handle);
			m_d->job_of_handle.insert(added->handle.id(), job);
			added->handle.set_max_connections(m_torrent_limit);
			continue;
		}

		if (auto *upd = lt::alert_cast<lt::state_update_alert>(a)) {
			for (const lt::torrent_status &st : upd->status) {
				if (!st.handle.is_valid())
					continue;
				const int job = m_d->job_of_handle.value(st.handle.id(), 0);
				if (!job)
					continue;

				download_progress p;
				p.received = st.total_wanted_done;
				p.total    = st.has_metadata ? st.total_wanted : -1;
				if (!st.name.empty())
					p.path = QDir(m_d->save_path.value(job))
					              .filePath(QString::fromStdString(st.name));

				// The file list is available as soon as there is metadata,
				// which for a .torrent is immediately and for a magnet is
				// later. Reporting it here rather than off
				// metadata_received_alert covers both, since that alert only
				// fires when metadata arrived from a *peer*. Sent once: the
				// list does not change and walking it every poll is waste.
				if (st.has_metadata && !m_d->files_sent.contains(job)) {
					if (auto ti = st.handle.torrent_file()) {
						const lt::file_storage &fs = ti->files();
						for (lt::file_index_t i : fs.file_range()) {
							// Skip padding. libtorrent inserts zero-content pad
							// files to align pieces in hybrid torrents; they are
							// an encoding detail, and listing them would put
							// entries like ".pad/98304" in front of the user as
							// though they were part of the download.
							if (fs.pad_file_at(i))
								continue;
							p.files << QString::fromStdString(fs.file_path(i));
						}
						m_d->files_sent.insert(job);
					}
				}

				if (st.state == lt::torrent_status::downloading_metadata) {
					p.state  = download_state::resolving;
					p.detail = "fetching metadata";
				} else if (st.state == lt::torrent_status::checking_files ||
				           st.state == lt::torrent_status::checking_resume_data) {
					p.state  = download_state::resolving;
					p.detail = "checking files";
				} else if (m_d->completed.contains(job)) {
					p.state  = download_state::seeding;
					p.detail = QString("seeding to %1 peer(s), ratio %2")
					                .arg(st.num_peers)
					                .arg(st.all_time_download > 0
					                         ? double(st.all_time_upload) /
					                               double(st.all_time_download)
					                         : 0.0, 0, 'f', 2);
				} else if (st.flags & lt::torrent_flags::paused) {
					p.state  = download_state::paused;
					p.detail = "paused";
				} else {
					p.state  = download_state::running;
					p.detail = QString("%1 peer(s), %2 KiB/s")
					                .arg(st.num_peers)
					                .arg(st.download_payload_rate / 1024);
				}
				emit progressed(job, p);

				// Seeding stops when the ratio is met — §11.4 makes this a
				// policy question, and this is where the policy lands.
				if (m_d->completed.contains(job) && m_seed_ratio > 0.0 &&
				    st.all_time_download > 0 &&
				    double(st.all_time_upload) / double(st.all_time_download)
				        >= m_seed_ratio) {
					st.handle.save_resume_data(lt::torrent_handle::save_info_dict);
					emit progressed(job, final_progress(job, st.handle));
					m_d->job_of_handle.remove(st.handle.id());
					m_d->handle_of_job.remove(job);
					m_d->completed.remove(job);
					m_d->files_sent.remove(job);
					m_d->session->remove_torrent(st.handle);
					emit finished(job, true, QString());
				}
			}
			continue;
		}

		if (auto *fin = lt::alert_cast<lt::torrent_finished_alert>(a)) {
			const int job = m_d->job_of_handle.value(fin->handle.id(), 0);
			if (!job)
				continue;
			fin->handle.save_resume_data(lt::torrent_handle::save_info_dict);

			if (m_seed_ratio <= 0.0) {
				// "Do not seed" — the transfer is over the moment it completes.
				// Report the final tally first: finished() carries no numbers,
				// and a torrent that completed between two polls would
				// otherwise be left showing whatever the last poll saw.
				emit progressed(job, final_progress(job, fin->handle));
				m_d->job_of_handle.remove(fin->handle.id());
				m_d->handle_of_job.remove(job);
				m_d->files_sent.remove(job);
				m_d->session->remove_torrent(fin->handle);
				emit finished(job, true, QString());
			} else {
				// Complete, but the source has not let go. This is exactly the
				// state the seam grew `seeding` for (§11.4).
				m_d->completed.insert(job);
				const lt::torrent_status st = fin->handle.status();
				download_progress p;
				p.state    = download_state::seeding;
				p.received = st.total_wanted_done;
				p.total    = st.total_wanted;
				p.detail   = "seeding";
				if (!st.name.empty())
					p.path = QDir(m_d->save_path.value(job))
					              .filePath(QString::fromStdString(st.name));
				emit progressed(job, p);
			}
			continue;
		}

		if (auto *md = lt::alert_cast<lt::metadata_received_alert>(a)) {
			const int job = m_d->job_of_handle.value(md->handle.id(), 0);
			if (!job)
				continue;
			// A magnet has files only now. Report them, so a multi-file job
			// stops looking like a single one.
			const lt::torrent_status st = md->handle.status();
			download_progress p;
			p.state    = download_state::running;
			p.received = st.total_wanted_done;
			p.total    = st.total_wanted;
			p.detail   = "metadata received";
			if (auto ti = md->handle.torrent_file()) {
				const lt::file_storage &fs = ti->files();
				for (lt::file_index_t i : fs.file_range()) {
					if (fs.pad_file_at(i))
						continue;      // alignment padding, not content
					p.files << QString::fromStdString(fs.file_path(i));
				}
				p.path = QDir(m_d->save_path.value(job))
				              .filePath(QString::fromStdString(ti->name()));
			}
			emit progressed(job, p);
			continue;
		}

		if (auto *saved = lt::alert_cast<lt::save_resume_data_alert>(a)) {
			const std::vector<char> buf = lt::write_resume_data_buf(saved->params);
			QFile f(resume_path(hash_string(saved->params.info_hashes)));
			if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
				f.write(buf.data(), qint64(buf.size()));
			continue;
		}

		if (auto *err = lt::alert_cast<lt::torrent_error_alert>(a)) {
			const int job = m_d->job_of_handle.value(err->handle.id(), 0);
			if (!job)
				continue;
			m_d->job_of_handle.remove(err->handle.id());
			m_d->handle_of_job.remove(job);
			m_d->completed.remove(job);
			emit finished(job, false, QString::fromStdString(err->error.message()));
			continue;
		}
	}

	// Ask for the next round of status. Without this there are no
	// state_update_alerts at all, and the UI would sit at zero forever.
	m_d->session->post_torrent_updates();
#endif
}
