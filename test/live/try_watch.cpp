// Opens a tab in the real shell, then puts a real HTTP download and a real
// torrent through the real downloads window, on the real display.
#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"
#include "torrent_download_source.h"
#include "download_manager.h"
#include "media_fixture.h"
#include "sample_tree.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QGuiApplication>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <QTreeView>
#include <QFileInfo>
#include <QTreeWidget>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/ip_filter.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_info.hpp>

#include <cstdio>
#include <fstream>
#include <memory>

namespace lt = libtorrent;

// Where screenshots and captures land. Set HYDRA_TEST_OUT to move it.
static QString test_out() {
	const QByteArray e = qgetenv("HYDRA_TEST_OUT");
	return (e.isEmpty() ? QString("/tmp/hydra-test/")
	                    : QString::fromLocal8Bit(e) + "/");
}

static const QString OUTDIR =
  test_out();

// **The `import`-based screenshot helper is gone, not silenced.** It shelled
// out to ImageMagick against the root window and had no caller left: `grab()`
// below replaced it, in-process, "so a blanked screen cannot turn the evidence
// into a black rectangle". Keeping it cost a warning on every rebuild and a
// dependency on the one tool whose absence is why `try_settings` skips.

// In-process capture of a specific window, so a blanked screen cannot turn the
// evidence into a black rectangle.
static void grab(const QString &title, const QString &n) {
	for (QWidget *w : QApplication::topLevelWidgets()) {
		if (w->isVisible() && w->windowTitle().contains(title)) {
			w->grab().save(OUTDIR + n);
			return;
		}
	}
	std::printf("NO WINDOW '%s'\n", qPrintable(title));
}

static QWidget *find_titled(const QString &title) {
	for (QWidget *w : QApplication::topLevelWidgets())
		if (w->isVisible() && w->windowTitle().contains(title))
			return w;
	return nullptr;
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	qtwebengine_factory::register_url_schemes(torrent_download_source::url_schemes());
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);
	app.setApplicationName("Hydra");

	// --- something for the http half to actually fetch ---------------------
	//
	// **The sibling driver's fix, which its own comment left for this one.**
	// `try_downloads` enqueued `http://127.0.0.1:8830/trailer.mp4` with nothing
	// listening on 8830 -- no fixture, no helper started, no port any other
	// driver serves -- so the job sat at `0/-1` and read in the log as a
	// download that stalled rather than a server that was never there. It was
	// pointed at the in-process media fixture, whose `/trailer.mp4` route
	// exists for this: 256 KiB, big enough that progress is a number that
	// moves. That comment ends "after this only try_watch belongs there", and
	// this is that.
	//
	// The sweep skipped this driver as needing "a live network", which was
	// never what it needed: the torrent half brings its own seeder on
	// 127.0.0.1 with no tracker and no DHT, and the http half wanted one local
	// file. Both are in-process now, so the skip is gone.
	media_fixture::server fixture;
	const QString http_base = fixture.start();
	if (http_base.isEmpty()) {
		std::printf("HYDRA-SKIP: the fixture server could not listen\n");
		return 1;
	}
	std::printf("serving: %s\n", qPrintable(http_base));

	// --- a torrent to actually download, with a seeder for it --------------
	const QString root = OUTDIR + "seed";
	QDir(root).removeRecursively();
	QDir().mkpath(root + "/Sintel");
	auto write = [&](const QString &rel, int bytes) {
		QFile f(root + "/Sintel/" + rel);
		f.open(QIODevice::WriteOnly);
		QByteArray b(bytes, Qt::Uninitialized);
		for (int i = 0; i < bytes; ++i) b[i] = char((i * 131) & 0xff);
		f.write(b);
	};
	// **The payload, and nothing in this tree produced it.** This copied
	// `$HYDRA_TEST_OUT/media/sintel.mkv` -- a real video, so that the external
	// player would have something it could decode -- and no driver, fixture or
	// make target writes that file. So the copy failed, `vsize` was 0, the
	// torrent's main file was empty, and the Watch press below waits for
	// `received > 1200000` bytes that could never arrive: the driver spun until
	// the sweep killed it at five minutes, with `video: 0 bytes` the only sign
	// and nobody reading it, because the sweep skipped this driver as needing
	// "a live network".
	//
	// A real file is still used when it is there -- that is what makes the
	// `pgrep ffplay` line below mean anything -- and otherwise five megabytes
	// of synthetic bytes stand in, which is what `try_downloads` does. Said
	// either way, because which one it was decides whether the player failing
	// to appear is a finding.
	const bool have_real =
	  QFile::copy(OUTDIR + "media/sintel.mkv", root + "/Sintel/sintel.mkv") &&
	  QFileInfo(root + "/Sintel/sintel.mkv").size() > 0;
	if (!have_real) {
		QFile::remove(root + "/Sintel/sintel.mkv");
		write("sintel.mkv", 5 * 1024 * 1024);
	}
	write("sample.mkv", 300 * 1024);     // a decoy that sorts first
	write("readme.txt", 2048);
	const qint64 vsize = QFileInfo(root + "/Sintel/sintel.mkv").size();
	std::printf("video: %lld bytes (%s)\n", vsize,
	             have_real ? "a real file, so a player can decode it"
	                        : "synthetic, so no player will decode it");

	lt::file_storage fs;
	fs.add_file("Sintel/readme.txt", 2048);
	fs.add_file("Sintel/sample.mkv", 300 * 1024);
	fs.add_file("Sintel/sintel.mkv", vsize);
	lt::create_torrent ct(fs, 32 * 1024);
	lt::set_piece_hashes(ct, QDir(root).absolutePath().toStdString(),
	                      [](lt::piece_index_t) {});
	std::vector<char> buf;
	lt::bencode(std::back_inserter(buf), ct.generate());
	const QString tpath = OUTDIR + "sintel.torrent";
	{ std::ofstream o(tpath.toStdString(), std::ios::binary);
		o.write(buf.data(), std::streamsize(buf.size())); }
	auto ti = std::make_shared<lt::torrent_info>(buf, lt::from_span);

	lt::settings_pack sp;
	sp.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:6930");
	sp.set_bool(lt::settings_pack::enable_dht, false);
	sp.set_bool(lt::settings_pack::enable_lsd, false);
	sp.set_bool(lt::settings_pack::enable_upnp, false);
	sp.set_bool(lt::settings_pack::enable_natpmp, false);
	sp.set_int(lt::settings_pack::upload_rate_limit, 220 * 1024);
	lt::session seeder{lt::session_params(sp)};
	{   // loopback peers are exempt from rate limits unless this class is cleared
		lt::ip_filter f = seeder.get_peer_class_filter();
		f.add_rule(lt::make_address("127.0.0.0"), lt::make_address("127.255.255.255"),
		            1 << static_cast<std::uint32_t>(lt::session::global_peer_class_id));
		seeder.set_peer_class_filter(f);
	}
	lt::add_torrent_params atp;
	atp.ti = ti;
	atp.save_path = QDir(root).absolutePath().toStdString();
	atp.flags |= lt::torrent_flags::seed_mode;
	lt::torrent_handle seed = seeder.add_torrent(atp);

	// --- the real shell ----------------------------------------------------
	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(shell::inert_sample_tree());
	w.show();

	auto *dm  = w.findChild<download_manager *>();
	auto *tor = w.findChild<torrent_download_source *>();
	std::printf("download_manager: %s   torrent source: %s\n",
	             dm ? "found" : "MISSING", tor ? "found" : "MISSING");
	if (tor)
		tor->set_listen_interfaces("127.0.0.1:6931");
	if (dm)
		dm->set_directory(OUTDIR + "dl");
	// **Emptied first, because last run's download is this run's problem.**
	// The seed root above is already cleared; the destination was not, so
	// libtorrent rechecked the files a previous run had finished, found every
	// piece present, and the torrent went straight to seeding. The Watch press
	// below needs it *mid-flight* -- `received > 1200000 && !complete()` -- so
	// a complete-on-arrival torrent means that branch is never taken and the
	// driver spins until the sweep kills it. Measured exactly that way: the
	// first run after the payload fix clicked Watch, the second could not.
	//
	// Named and under this driver's own output, which is the one shape a
	// wholesale removal is allowed in.
	QDir(OUTDIR + "dl").removeRecursively();
	QDir().mkpath(OUTDIR + "dl");

	// Keep introducing the seeder to us; no tracker, no DHT.
	auto *poke = new QTimer(&app);
	poke->setInterval(700);
	QObject::connect(poke, &QTimer::timeout, [&] {
		if (seed.is_valid())
			seed.connect_peer(lt::tcp::endpoint(
			  lt::make_address_v4("127.0.0.1"), 6931));
		std::vector<lt::alert *> junk;
		seeder.pop_alerts(&junk);
	});
	poke->start();

	int step = 0;
	bool watched = false;
	int after = 0;
	auto *tick = new QTimer(&app);
	tick->setInterval(1400);
	QObject::connect(tick, &QTimer::timeout, [&] {
		switch (step++) {
		case 0: {
			// Open a tab the way a user does: activate a row in the tree.
			auto *tree = w.findChild<QTreeView *>();
			const QModelIndex first = tree->model()->index(0, 0);
			const QModelIndex kid   = tree->model()->index(0, 0, first);
			std::printf("opening tab: %s\n",
			             qPrintable(kid.data().toString()));
			emit tree->activated(kid);
			break;
		}
		case 1: case 2: case 3:
			break;                               // let the page load
		case 4:
			grab("Hydra", "10-tab-open.png");
			std::printf("tab opened\n");
			break;
		case 5: {
			QString err;
			const int id = dm->enqueue(QUrl(http_base + "trailer.mp4"),
			                            QString(), &err);
			std::printf("http download queued: id=%d %s\n", id, qPrintable(err));
			break;
		}
		case 6: {
			QString err;
			// A .torrent the torrent source will take -- the consent gate fires.
			const int id = dm->enqueue(QUrl::fromLocalFile(tpath), QString(), &err);
			std::printf("torrent queued: id=%d %s\n", id, qPrintable(err));
			break;
		}
		case 7:
			if (auto *box = qobject_cast<QMessageBox *>(find_titled("not a private"))) {
				// In-process grab: `import` reaches into X, and a modal dialog
				// holding a pointer grab is exactly when that stalls.
				box->grab().save(OUTDIR + "11-consent.png");
				std::printf("consent dialog: %s\n", qPrintable(box->text().left(60)));
				box->button(QMessageBox::Yes)->click();
				std::printf("consent granted\n");
			} else {
				std::printf("NO CONSENT DIALOG\n");
			}
			break;
		case 8: {
			QAction *dl = nullptr;
			for (QAction *a : w.findChildren<QAction *>())
				if (a->text().contains("Downloads"))
					dl = a;
			if (dl) { std::printf("opening: %s\n", qPrintable(dl->text())); dl->trigger(); }
			break;
		}
		case 10:
			grab("Downloads", "20-downloads.png");
			break;
		default: {
			QWidget *dlwin = nullptr;
			for (QWidget *ww : QApplication::topLevelWidgets())
				if (ww->isVisible() && ww->windowTitle().contains("Downloads"))
					dlwin = ww;
			if (!dlwin)
				break;
			auto *tree = dlwin->findChild<QTreeWidget *>();
			auto *watch = dlwin->findChild<QPushButton *>();
			for (QPushButton *b2 : dlwin->findChildren<QPushButton *>())
				if (b2->text().contains("Watch"))
					watch = b2;

			// Find the torrent row and select it.
			const download_job *tj = nullptr;
			int rowidx = -1;
			for (int i = 0; i < dm->jobs().size(); ++i)
				if (dm->jobs()[i].source_id == "torrent") { tj = &dm->jobs()[i]; rowidx = i; }
			if (!tj || !tree || rowidx < 0)
				break;
			if (tree->currentItem() != tree->topLevelItem(rowidx))
				tree->setCurrentItem(tree->topLevelItem(rowidx));

			// Press Watch once, while it is genuinely still downloading.
			if (!watched && tj->received > 1200000 && !tj->complete()) {
				std::printf("torrent at %lld/%lld — Watch enabled: %s\n",
				             tj->received, tj->total,
				             watch && watch->isEnabled() ? "yes" : "NO");
				grab("Downloads", "21-before-watch.png");
				if (watch && watch->isEnabled()) {
					watch->click();
					watched = true;
					std::printf("Watch clicked\n");
				}
			} else if (watched && ++after == 8) {
				grab("Downloads", "22-after-watch.png");
				// **The whole screen, and only where there is one.** `grab()`
				// above takes one window; this is the shot that shows the
				// player *beside* the browser, which is the question at this
				// moment and which no per-window grab can answer. Offscreen it
				// cannot work at all -- it printed `import: unable to open X
				// server` into the log every run -- so it is asked for only
				// when a real display is there.
				if (QGuiApplication::platformName() != QLatin1String("offscreen"))
					QProcess::execute("import",
					                   {"-window", "root", OUTDIR + "23-screen.png"});
				QProcess p2;
				p2.start("pgrep", {"-a", "ffplay"});
				p2.waitForFinished(3000);
				const QString out = QString::fromUtf8(p2.readAllStandardOutput()).trimmed();
				std::printf("player process: %s\n",
				             out.isEmpty() ? "NONE" : qPrintable(out.left(140)));
				for (const download_job &j : dm->jobs())
					std::printf("job %d [%s] %lld/%lld %s\n", j.id,
					             qPrintable(j.source_id), j.received, j.total,
					             qPrintable(j.detail));
			} else if (watched && after > 40) {
				std::printf("done\n");
				qApp->quit();
			}
			break;
		}
		}
	});
	tick->start();
	return app.exec();
}
