// Opens a tab in the real shell, then puts a real HTTP download and a real
// torrent through the real downloads window, on the real display.
#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"
#include "torrent_download_source.h"
#include "download_manager.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <QTreeView>

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

static void screen(const QString &n) {
	QProcess::execute("import", {"-window", "root", OUTDIR + n});
}

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
	write("sintel.mkv", 5 * 1024 * 1024);
	write("readme.txt", 2048);

	lt::file_storage fs;
	fs.add_file("Sintel/sintel.mkv", 5 * 1024 * 1024);
	fs.add_file("Sintel/readme.txt", 2048);
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
	sp.set_int(lt::settings_pack::upload_rate_limit, 350 * 1024);
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
	w.load_tree("/home/nabbe/src/hydra/sample-tree.txt");
	w.show();

	auto *dm  = w.findChild<download_manager *>();
	auto *tor = w.findChild<torrent_download_source *>();
	std::printf("download_manager: %s   torrent source: %s\n",
	             dm ? "found" : "MISSING", tor ? "found" : "MISSING");
	if (tor)
		tor->set_listen_interfaces("127.0.0.1:6931");
	if (dm)
		dm->set_directory(OUTDIR + "dl");
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
	auto *tick = new QTimer(&app);
	tick->setInterval(1600);
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
			const int id = dm->enqueue(QUrl("http://127.0.0.1:8830/trailer.mp4"),
			                            QString(), &err);
			std::printf("http download queued: id=%d %s\n", id, qPrintable(err));
			break;
		}
		case 6: {
			QString err;
			// A .torrent the torrent source will take — the consent gate fires.
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
			grab("Downloads", "12-downloads.png");
			break;
		case 14:
			grab("Downloads", "13-downloads-progress.png");
			break;
		case 20:
			grab("Downloads", "14-downloads-later.png");
			grab("Hydra", "15-shell.png");
			for (const download_job &j : dm->jobs())
				std::printf("job %d [%s] %s  %lld/%lld  %s\n", j.id,
				             qPrintable(j.source_id), qPrintable(j.path),
				             j.received, j.total, qPrintable(j.detail));
			break;
		case 22:
			std::printf("done\n");
			qApp->quit();
			break;
		default:
			break;
		}
	});
	tick->start();
	return app.exec();
}
