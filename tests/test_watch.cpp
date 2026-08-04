// Watch-while-downloading: does the proxy serve only real bytes?
// A throttled seeder keeps the torrent mid-flight long enough to check.
#include "download_manager.h"
#include "local_proxy.h"
#include "torrent_download_source.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/ip_filter.hpp>
#include <libtorrent/peer_class.hpp>
#include <libtorrent/torrent_info.hpp>

#include <cstdio>
#include <fstream>
#include <memory>

namespace lt = libtorrent;

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void section(const char *n) { std::printf("\n== %s ==\n", n); }
static void spin(int ms) {
	QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec();
}

// GET a range through the proxy; returns body, sets status.
static QByteArray fetch(QNetworkAccessManager &net, const QUrl &url,
                         qint64 from, qint64 to, int *status) {
	QNetworkRequest req(url);
	if (from >= 0)
		req.setRawHeader("Range", "bytes=" + QByteArray::number(from) + "-" +
		                            (to >= 0 ? QByteArray::number(to) : QByteArray()));
	QNetworkReply *r = net.get(req);
	QEventLoop l;
	QObject::connect(r, &QNetworkReply::finished, &l, &QEventLoop::quit);
	QTimer::singleShot(10000, &l, &QEventLoop::quit);
	l.exec();
	*status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	const QByteArray body = r->readAll();
	r->deleteLater();
	return body;
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QCoreApplication app(argc, argv);

	const QString tmp = QDir::temp().filePath("hydra-watch-test");
	QDir(tmp).removeRecursively();
	QDir().mkpath(tmp);

	// --- a 6 MiB "video" in a multi-file torrent ------------------------
	const QString name = "show";
	const QString src_root = QDir(tmp).filePath("src");
	const QString rel = name + "/episode.mkv";
	QDir().mkpath(QDir(src_root).filePath(name));
	const int payload = 6 * 1024 * 1024;
	QByteArray blob(payload, Qt::Uninitialized);
	for (int i = 0; i < payload; ++i)
		blob[i] = char((i * 7919) & 0xff);          // never zero-heavy
	{
		QFile f(QDir(src_root).filePath(rel));
		f.open(QIODevice::WriteOnly); f.write(blob); f.close();
	}
	// a second file, so the primary is not at offset 0
	const QString rel2 = name + "/readme.txt";
	{
		QFile f(QDir(src_root).filePath(rel2));
		f.open(QIODevice::WriteOnly); f.write(QByteArray(300000, 'R')); f.close();
	}

	lt::file_storage fs;
	fs.add_file((rel2).toStdString(), 300000);       // first in the torrent
	fs.add_file((rel).toStdString(), payload);
	lt::create_torrent ct(fs, 32 * 1024);
	lt::set_piece_hashes(ct, QDir(src_root).absolutePath().toStdString(),
	                      [](lt::piece_index_t) {});
	std::vector<char> buf;
	lt::bencode(std::back_inserter(buf), ct.generate());
	const QString tpath = QDir(tmp).filePath("show.torrent");
	{ std::ofstream o(tpath.toStdString(), std::ios::binary);
		o.write(buf.data(), std::streamsize(buf.size())); }
	auto ti = std::make_shared<lt::torrent_info>(buf, lt::from_span);

	// --- throttled seeder, so the transfer stays in flight --------------
	lt::settings_pack sp;
	sp.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:6910");
	sp.set_bool(lt::settings_pack::enable_dht, false);
	sp.set_bool(lt::settings_pack::enable_lsd, false);
	sp.set_bool(lt::settings_pack::enable_upnp, false);
	sp.set_bool(lt::settings_pack::enable_natpmp, false);
	sp.set_int(lt::settings_pack::upload_rate_limit, 400 * 1024);   // ~15s for 6 MiB
	sp.set_int(lt::settings_pack::alert_mask, lt::alert_category::error);
	lt::session seeder{lt::session_params(sp)};
	// Peers on the local network are put in local_peer_class, which is exempt
	// from rate limits -- so an unmodified seeder saturates loopback and the
	// transfer is over before it can be observed. Clear the class for 127/8.
	{
		lt::ip_filter f = seeder.get_peer_class_filter();
		f.add_rule(lt::make_address("127.0.0.0"), lt::make_address("127.255.255.255"),
		            1 << static_cast<std::uint32_t>(lt::session::global_peer_class_id));
		seeder.set_peer_class_filter(f);
	}
	lt::add_torrent_params atp;
	atp.ti = ti;
	atp.save_path = QDir(src_root).absolutePath().toStdString();
	atp.flags |= lt::torrent_flags::seed_mode;
	lt::torrent_handle seed = seeder.add_torrent(atp);

	// --- our side --------------------------------------------------------
	local_proxy proxy;
	check(proxy.start(), "the local proxy is listening");

	download_manager m;
	auto *tor = new torrent_download_source;
	tor->set_listen_interfaces("127.0.0.1:6911");
	tor->set_state_directory(QDir(tmp).filePath("state"));
	tor->set_seed_ratio(0.0);
	m.add_source(tor);
	const QString dest = QDir(tmp).filePath("dst");
	QDir().mkpath(dest);
	m.set_directory(dest);
	m.set_consent("torrent", true);

	QString err;
	const int id = m.enqueue(QUrl::fromLocalFile(tpath), QString(), &err);
	check(id != 0, "job accepted");

	auto job = [&]() -> const download_job * {
		for (const download_job &j : m.jobs()) if (j.id == id) return &j;
		return nullptr;
	};
	auto pump = [&](int ms) {
		QElapsedTimer t; t.start();
		while (t.elapsed() < ms) {
			if (seed.is_valid())
				seed.connect_peer(lt::tcp::endpoint(
				  lt::make_address_v4("127.0.0.1"), 6911));
			std::vector<lt::alert *> junk; seeder.pop_alerts(&junk);
			spin(200);
		}
	};

	section("streaming priority and the readable prefix");

	// Wait for metadata/file list, then ask to stream the video.
	QElapsedTimer t; t.start();
	while (job()->files.size() < 2 && t.elapsed() < 30000)
		pump(300);
	check(job()->files.size() == 2,
	      QString("file list known, padding excluded (%1)").arg(job()->files.size()));

	tor->prioritize_streaming(id, rel, true);
	const QString local_file = QDir(dest).filePath(rel);

	// The proxy serves the *readable prefix*, not the sparse file's size.
	const QUrl url = proxy.publish_file(local_file, "video/x-matroska",
	                                     [&] { return tor->contiguous_bytes(id, rel); });
	check(url.isValid(), "published through the proxy");

	// A second publication that also declares the file's eventual size. Without
	// that the proxy can only promise what has arrived, and there is nothing to
	// hold for.
	const QUrl held = proxy.publish_file(
	  local_file, "video/x-matroska",
	  [&] { return tor->contiguous_bytes(id, rel); },
	  [&]() -> qint64 { return payload; });

	QNetworkAccessManager net;

	// Sample mid-flight.
	bool sampled = false, monotonic = true, never_over = true;
	qint64 last = 0, mid_value = 0;
	t.restart();
	while (t.elapsed() < 40000) {
		pump(500);
		const qint64 c = tor->contiguous_bytes(id, rel);
		// -1 is the "trust the file's size" sentinel, not a length, so it is
		// not a shrink. It appears once the source releases a finished job.
		if (c >= 0) {
			if (c < last) monotonic = false;
			last = c;
		}
		if (c > 0 && c < payload && !sampled) {
			sampled = true;
			mid_value = c;

			// The file on disk is already full size (sparse) — that is the trap.
			const qint64 on_disk = QFileInfo(local_file).size();
			check(on_disk >= c, QString("file is allocated ahead of the data "
			                             "(on disk %1, readable %2)")
			                        .arg(on_disk).arg(c));

			// Everything the proxy hands out must be real.
			int st = 0;
			const QByteArray got = fetch(net, url, 0, c - 1, &st);
			check(st == 206 || st == 200,
			      QString("prefix request succeeds (%1)").arg(st));
			check(got.size() == c,
			      QString("served exactly the readable prefix (%1 of %2)")
			          .arg(got.size()).arg(c));
			check(got == blob.left(got.size()),
			      "and every byte matches the original — no sparse-hole zeros");

			// Past the prefix must be refused, not answered with zeros.
			int st2 = 0;
			const QByteArray beyond = fetch(net, url, payload - 4096, payload - 1, &st2);
			check(st2 == 416 || beyond.isEmpty(),
			      QString("a range past the readable prefix is refused (%1, %2 bytes)")
			          .arg(st2).arg(beyond.size()));
			if (!beyond.isEmpty() && beyond.count('\0') == beyond.size())
				never_over = false;

			// The same request against the *held* publication must wait for
			// the bytes and then deliver them, rather than refusing.
			section("the proxy holds instead of closing short");
			const qint64 from = qMin<qint64>(payload - 1, c + 64 * 1024);
			const qint64 to   = qMin<qint64>(payload - 1, from + 32 * 1024 - 1);
			std::printf("  ..    asking for %lld-%lld with only %lld readable\n",
			             from, to, c);
			// Keep the swarm moving while the request is held; the timer fires
			// inside the nested loop the fetch runs.
			QTimer drive;
			QObject::connect(&drive, &QTimer::timeout, [&] {
				if (seed.is_valid())
					seed.connect_peer(lt::tcp::endpoint(
					  lt::make_address_v4("127.0.0.1"), 6911));
				std::vector<lt::alert *> junk;
				seeder.pop_alerts(&junk);
			});
			drive.start(300);
			QElapsedTimer held_t; held_t.start();
			int hst = 0;
			const QByteArray held_got = fetch(net, held, from, to, &hst);
			const qint64 waited = held_t.elapsed();
			drive.stop();

			check(hst == 206 || hst == 200,
			      QString("the held request succeeds (%1)").arg(hst));
			check(held_got.size() == to - from + 1,
			      QString("and delivers the whole slice (%1 of %2)")
			          .arg(held_got.size()).arg(to - from + 1));
			check(held_got == blob.mid(int(from), held_got.size()),
			      "with the real bytes, not zeros");
			check(waited > 50,
			      QString("having actually waited for them (%1 ms)").arg(waited));
			section("back to the unheld publication");
		}
		if (job()->terminal())
			break;
	}
	check(sampled, QString("caught the transfer mid-flight (prefix was %1 of %2)")
	                   .arg(mid_value).arg(payload));
	check(monotonic, "the readable prefix never went backwards");
	check(never_over, "the proxy never served a block of zeros");

	section("after completion");
	t.restart();
	while (!job()->terminal() && t.elapsed() < 60000)
		pump(500);
	check(job()->status == download_state::done, "the job completes");
	// Once the source releases a finished job it no longer has a handle, and
	// -1 ("trust the file's size") is then the correct answer: the file is
	// complete, so its size is finally an honest statement about its contents.
	const qint64 after = tor->contiguous_bytes(id, rel);
	check(after == payload || after == -1,
	      QString("the whole file is readable (%1)").arg(after));

	int st = 0;
	const QByteArray whole = fetch(net, url, 0, payload - 1, &st);
	check(whole.size() == payload, QString("the proxy serves it all (%1)").arg(whole.size()));
	check(whole == blob, "byte-identical to the original");

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
