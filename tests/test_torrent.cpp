// SPDX-License-Identifier: GPL-3.0-or-later
// End-to-end test of torrent_download_source against a real libtorrent swarm:
// a seeder session inside this process, and the source downloading from it over
// loopback. No tracker, no DHT — the seeder connects to us directly.
#include "download_manager.h"
#include "torrent_download_source.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTimer>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/hex.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_info.hpp>

#include <cstdio>
#include <fstream>
#include <memory>

namespace lt = libtorrent;

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &what) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(what)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(what)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }

static void spin(int ms) {
	QEventLoop loop;
	QTimer::singleShot(ms, &loop, &QEventLoop::quit);
	loop.exec();
}

static const download_job *job_by_id(const download_manager &m, int id) {
	for (const download_job &j : m.jobs())
		if (j.id == id)
			return &j;
	return nullptr;
}

static QByteArray sha(const QString &path) {
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return {};
	return QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256);
}

// --- build a real .torrent for files already on disk -----------------------
struct made_torrent {
	QString torrent_path;
	QString magnet;
	std::shared_ptr<lt::torrent_info> ti;
};

static made_torrent make_torrent(const QString &root, const QString &name,
                                  const QStringList &rel_files,
                                  const QString &out_torrent) {
	lt::file_storage fs;
	for (const QString &rel : rel_files) {
		// The files live under root/<name>/, which is also what the torrent
		// records — using root/<rel> here silently yields size 0 for every
		// file and libtorrent rejects the result as "invalid length".
		const QString full = QDir(root).filePath(name + "/" + rel);
		fs.add_file((name + "/" + rel).toStdString(),
		             QFileInfo(full).size());
	}
	lt::create_torrent ct(fs, 16 * 1024);
	ct.set_creator("hydra-test");
	// set_piece_hashes wants the directory that *contains* the torrent's name.
	lt::set_piece_hashes(ct, QDir(root).absolutePath().toStdString(),
	                      [](lt::piece_index_t) {});
	const lt::entry e = ct.generate();
	std::vector<char> buf;
	lt::bencode(std::back_inserter(buf), e);
	{
		std::ofstream out(out_torrent.toStdString(), std::ios::binary);
		out.write(buf.data(), std::streamsize(buf.size()));
	}
	made_torrent mt;
	mt.torrent_path = out_torrent;
	mt.ti = std::make_shared<lt::torrent_info>(buf, lt::from_span);
	mt.magnet = QString::fromStdString(lt::make_magnet_uri(*mt.ti));
	return mt;
}

// A seeder session that already has the data, listening on a known port.
static std::unique_ptr<lt::session> make_seeder(int port, const QString &data_root,
                                                 const std::shared_ptr<lt::torrent_info> &ti,
                                                 lt::torrent_handle *out) {
	lt::settings_pack sp;
	sp.set_str(lt::settings_pack::listen_interfaces,
	            ("127.0.0.1:" + QString::number(port)).toStdString());
	sp.set_bool(lt::settings_pack::enable_dht, false);
	sp.set_bool(lt::settings_pack::enable_lsd, false);
	sp.set_bool(lt::settings_pack::enable_upnp, false);
	sp.set_bool(lt::settings_pack::enable_natpmp, false);
	sp.set_int(lt::settings_pack::alert_mask, lt::alert_category::error);
	auto ses = std::make_unique<lt::session>(lt::session_params(sp));

	lt::add_torrent_params atp;
	atp.ti        = ti;
	atp.save_path = QDir(data_root).absolutePath().toStdString();
	atp.flags |= lt::torrent_flags::seed_mode;
	*out = ses->add_torrent(atp);
	return ses;
}

// ---------------------------------------------------------------------------

static QString g_tmp;

struct fixture {
	made_torrent mt;
	std::unique_ptr<lt::session> seeder;
	lt::torrent_handle seed_handle;
	QString data_root, dest;
};

static fixture build_fixture(const QString &tag, const QStringList &rel_files,
                              int seed_port, int payload_bytes) {
	fixture fx;
	fx.data_root = QDir(g_tmp).filePath(tag + "-src");
	fx.dest      = QDir(g_tmp).filePath(tag + "-dst");
	const QString name = tag + "-payload";
	QDir().mkpath(QDir(fx.data_root).filePath(name));
	QDir().mkpath(fx.dest);

	for (const QString &rel : rel_files) {
		const QString full = QDir(fx.data_root).filePath(name + "/" + rel);
		QDir().mkpath(QFileInfo(full).path());
		QFile f(full);
		f.open(QIODevice::WriteOnly);
		QByteArray blob(payload_bytes, Qt::Uninitialized);
		for (int i = 0; i < payload_bytes; ++i)
			blob[i] = char((i * 31 + rel.size() * 7) & 0xff);
		f.write(blob);
		f.close();
	}

	fx.mt = make_torrent(fx.data_root, name, rel_files,
	                      QDir(g_tmp).filePath(tag + ".torrent"));
	fx.seeder = make_seeder(seed_port, fx.data_root, fx.mt.ti, &fx.seed_handle);
	return fx;
}

// Drive both event loops until `done` or timeout. The seeder is told to connect
// to us each round: no tracker and no DHT, so somebody has to make the introduction.
static bool pump_until(const std::function<bool()> &done, fixture &fx,
                        int our_port, int timeout_ms) {
	QElapsedTimer t;
	t.start();
	while (!done() && t.elapsed() < timeout_ms) {
		if (fx.seed_handle.is_valid())
			fx.seed_handle.connect_peer(lt::tcp::endpoint(
				lt::make_address_v4("127.0.0.1"), std::uint16_t(our_port)));
		std::vector<lt::alert *> junk;
		fx.seeder->pop_alerts(&junk);
		spin(250);
	}
	return done();
}

// ---------------------------------------------------------------------------

static void test_single_file_download() {
	section("a real torrent downloads end to end");

	const int our_port = 6899, seed_port = 6898;
	fixture fx = build_fixture("single", {"movie.bin"}, seed_port, 512 * 1024);

	download_manager m;
	auto *tor = new torrent_download_source;
	tor->set_listen_interfaces("127.0.0.1:" + QString::number(our_port));
	tor->set_state_directory(QDir(g_tmp).filePath("state"));
	tor->set_seed_ratio(0.0);          // stop as soon as it completes
	m.add_source(tor);
	m.set_directory(fx.dest);
	m.set_consent("torrent", true);

	// Record every state the job passes through.
	QList<download_state> seen;
	QObject::connect(&m, &download_manager::changed, &m, [&] {
		for (const download_job &j : m.jobs())
			if (seen.isEmpty() || seen.last() != j.status)
				seen << j.status;
	});

	QString err;
	const int id = m.enqueue(QUrl::fromLocalFile(fx.mt.torrent_path), "node-9", &err);
	check(id != 0, QString("a .torrent file is accepted (%1)").arg(err));

	const bool ok = pump_until([&] {
		const download_job *j = job_by_id(m, id);
		return j && j->terminal();
	}, fx, our_port, 60000);

	const download_job *j = job_by_id(m, id);
	check(ok && j->status == download_state::done,
	      QString("it completes (state=%1, detail=%2)")
	          .arg(int(j->status)).arg(j->detail));
	check(seen.contains(download_state::resolving),
	      "it passed through resolving while checking/fetching metadata");
	check(seen.contains(download_state::running), "and through running");
	check(j->received == 512 * 1024,
	      QString("all bytes accounted for (%1)").arg(j->received));
	check(j->total == 512 * 1024, "with the right total");
	check(j->node_id == "node-9", "and it remembers the node it came from");
	check(j->source_id == "torrent", "attributed to the torrent source");

	const QString got = QDir(fx.dest).filePath("single-payload/movie.bin");
	const QString orig = QDir(fx.data_root).filePath("single-payload/movie.bin");
	check(QFile::exists(got), "the file landed in the download directory");
	check(!sha(got).isEmpty() && sha(got) == sha(orig),
	      "and is byte-identical to what the seeder had");

	check(!j->path.isEmpty() && j->path.contains("single-payload"),
	      QString("the job reports a usable path (%1)").arg(j->path));
}

static void test_multi_file_and_seeding() {
	section("multi-file, and completion that is not the end");

	const int our_port = 6901, seed_port = 6900;
	fixture fx = build_fixture("multi", {"a.bin", "b.bin", "sub/c.bin"},
	                            seed_port, 64 * 1024);

	download_manager m;
	auto *tor = new torrent_download_source;
	tor->set_listen_interfaces("127.0.0.1:" + QString::number(our_port));
	tor->set_state_directory(QDir(g_tmp).filePath("state"));
	tor->set_seed_ratio(1.0);          // seed after completing
	m.add_source(tor);
	m.set_directory(fx.dest);
	m.set_consent("torrent", true);

	QString err;
	const int id = m.enqueue(QUrl::fromLocalFile(fx.mt.torrent_path), QString(), &err);

	const bool reached = pump_until([&] {
		const download_job *j = job_by_id(m, id);
		return j && j->status == download_state::seeding;
	}, fx, our_port, 60000);

	const download_job *j = job_by_id(m, id);
	check(reached, QString("it reaches seeding rather than done (state=%1)")
	                   .arg(int(j->status)));
	check(j->complete(), "seeding counts as complete — the files are usable");
	check(!j->terminal(), "but not terminal — the source has not let go");
	check(j->files.size() == 3,
	      QString("all three files are reported (%1)").arg(j->files.size()));
	// The state already reads "Complete — seeding"; the detail carries only
	// the specifics, so repeating the word there would be noise.
	check(!j->detail.contains("seeding"),
	      QString("and a detail that does not repeat the state (%1)").arg(j->detail));

	for (const QString &rel : QStringList{"a.bin", "b.bin", "sub/c.bin"}) {
		const QString got  = QDir(fx.dest).filePath("multi-payload/" + rel);
		const QString orig = QDir(fx.data_root).filePath("multi-payload/" + rel);
		check(!sha(got).isEmpty() && sha(got) == sha(orig),
		      "byte-identical: " + rel);
	}

	// Resume data is the torrent equivalent of the HTTP Range resume. It is
	// requested on completion and written when the alert comes back, so give
	// the poll a cycle or two rather than racing it.
	spin(2000);
	// Named by info-hash, not job id -- job ids restart every session.
	const QString rp = QDir(g_tmp).filePath(
		"state/" + QString::fromStdString(lt::aux::to_hex(
			fx.mt.ti->info_hashes().get_best())) + ".resume");
	check(QFileInfo(rp).size() > 0,
	      QString("resume data was written, keyed by info-hash (%1)").arg(rp));

	// And cancelling a seeding job releases it.
	m.cancel(id);
	spin(300);
	check(job_by_id(m, id)->status == download_state::cancelled,
	      "cancelling a seeding job stops it");
}

static void test_magnet_and_errors() {
	section("magnet links and refusals");

	download_manager m;
	auto *tor = new torrent_download_source;
	tor->set_listen_interfaces("127.0.0.1:6903");
	tor->set_state_directory(QDir(g_tmp).filePath("state"));
	m.add_source(tor);
	m.set_directory(QDir(g_tmp).filePath("magnet-dst"));
	m.set_consent("torrent", true);

	const int our_port = 6903, seed_port = 6902;
	fixture fx = build_fixture("magnet", {"m.bin"}, seed_port, 64 * 1024);

	QString err;
	const int id = m.enqueue(QUrl(fx.mt.magnet), QString(), &err);
	check(id != 0, QString("a magnet link is accepted (%1)").arg(err));
	spin(1200);
	const download_job *j = job_by_id(m, id);
	check(j->status == download_state::resolving,
	      QString("a magnet starts in resolving (state=%1)").arg(int(j->status)));
	check(j->detail.contains("metadata"),
	      QString("fetching metadata (%1)").arg(j->detail));
	check(j->total == -1, "with no size yet — a magnet has no metadata");
	// The name comes from the magnet's dn= parameter, reported by the source
	// so the downloads list never has to parse a magnet link itself.
	check(j->path.contains("magnet-payload"),
	      QString("a name is reported before metadata arrives (%1)").arg(j->path));

	// It should get metadata from the seeder, since that is the ut_metadata
	// extension doing exactly what a magnet link relies on.
	const bool got_meta = pump_until([&] {
		const download_job *k = job_by_id(m, id);
		return k && k->total > 0;
	}, fx, our_port, 60000);
	check(got_meta, QString("metadata arrives from a peer (total=%1)")
	                    .arg(job_by_id(m, id)->total));

	QString e2;
	const int bad = m.enqueue(QUrl("magnet:?xt=urn:btih:nonsense"), QString(), &e2);
	check(bad == 0 || job_by_id(m, bad)->status == download_state::failed,
	      QString("a malformed magnet is rejected (%1)").arg(e2));

	QString e3;
	check(m.enqueue(QUrl("https://example.invalid/page.html"), QString(), &e3) == 0,
	      "a plain web page is not offered to the torrent source");
}

static void test_consent_with_real_source() {
	section("the consent gate holds a real torrent");

	download_manager m;
	auto *tor = new torrent_download_source;
	tor->set_state_directory(QDir(g_tmp).filePath("state"));
	m.add_source(tor);
	m.set_directory(QDir(g_tmp).filePath("consent-dst"));

	QSignalSpy asked(&m, &download_manager::consent_required);
	QString err;
	const int id = m.enqueue(QUrl("magnet:?xt=urn:btih:"
	                               "0123456789abcdef0123456789abcdef01234567"),
	                          QString(), &err);
	check(id != 0, "the job is accepted");
	check(asked.count() == 1, "consent is demanded before anything starts");
	check(asked.value(0).value(1).toString().contains("announced to"),
	      "the note says what leaks");
	check(asked.value(0).value(1).toString().contains("VPN"),
	      "and points at the system layer, since we ship no tunnel");
	check(job_by_id(m, id)->status == download_state::queued,
	      "the torrent waits");
	check(job_by_id(m, id)->public_participation,
	      "and the row is marked for the UI");
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	check(torrent_download_source::available(),
	      "built with libtorrent support");

	g_tmp = QDir::temp().filePath("hydra-torrent-test");
	QDir(g_tmp).removeRecursively();
	QDir().mkpath(g_tmp);

	test_consent_with_real_source();
	test_single_file_download();
	test_multi_file_and_seeding();
	test_magnet_and_errors();

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
