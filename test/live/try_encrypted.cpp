// An encrypted (AES-128) HLS stream saved from a page, end to end, in the
// real window: the page asks for the stream, Find Media lists it, Download
// is pressed, and the file in Downloads must play. Live: it needs a display,
// ffmpeg and yt-dlp, and nothing beyond this machine.
//
//     xvfb-run -a ./test/build-make/try_encrypted
//
// **Why this exists**: the holder, 2026-11-09, on the ffmpeg decryption:
// "i don't know of such sites, you should make tests for it." The suite
// (`test_assembly`) holds `stream_assembly` alone; this holds the chain a
// person triggers, which the unit suites cannot join.
//
// The stream is made here by ffmpeg: six seconds of test picture as an
// AES-128 HLS master and media playlist, its key served beside it. The page
// asks for the master from script, which is how a site's own player asks,
// and names it in a <video> too, so yt-dlp's generic extractor finds it.
// Two routes are then pressed, the two a person meets:
//
//   - detection's row, which this browser fetched: the playlist is handed
//     to ffmpeg, which fetches the key and decrypts;
//   - yt-dlp's row, which yt-dlp fetches and decrypts itself.
//
// Each must leave a file that ffmpeg decodes, and the key must have been
// fetched -- a file made of ciphertext would decode as nothing.
//
// What makes it stop: every wait is bounded, the window is closed and
// deleted, and the server and the scratch HOME are this process's objects.
#include "main_window.h"
#include "media_detector.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "request_filter.h"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>
#include <cstdio>
#include <functional>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void spin(int ms) {
	QEventLoop l;
	QTimer::singleShot(ms, &l, &QEventLoop::quit);
	l.exec();
}

// Serves the page and every file of the stream, and counts the key's
// fetches: the one request no route can skip and still produce a picture.
class stream_server : public QTcpServer {
public:
	QHash<QByteArray, QByteArray> files;
	int key_fetches = 0;
protected:
	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, s, [this, s] {
			const QByteArray req = s->readAll();
			const bool head = req.startsWith("HEAD ");
			const int at = req.indexOf(' ') + 1;
			QByteArray path = req.mid(at, req.indexOf(' ', at) - at);
			if (const int q = path.indexOf('?'); q >= 0)
				path.truncate(q);
			if (path == "/enc/k.key")
				++key_fetches;
			const bool known = files.contains(path);
			const QByteArray body = files.value(path);
			const QByteArray type =
			  path.endsWith(".m3u8") ? "application/vnd.apple.mpegurl"
			  : path.endsWith(".ts") ? "video/mp2t"
			  : path.startsWith("/watch") ? "text/html"
			  : "application/octet-stream";
			s->write(QByteArray(known ? "HTTP/1.1 200 OK" : "HTTP/1.1 404 Not Found") +
			         "\r\nContent-Type: " + type + "\r\nContent-Length: " +
			         QByteArray::number(body.size()) +
			         "\r\nConnection: close\r\n\r\n" + (head ? QByteArray() : body));
			s->disconnectFromHost();
		});
		connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
	}
};

static bool decodes(const QString &file) {
	QProcess p;
	p.setProcessChannelMode(QProcess::MergedChannels);
	p.start("ffmpeg", { "-v", "error", "-i", file, "-f", "null", "-" });
	return p.waitForFinished(30000) && p.exitCode() == 0 &&
	       p.readAll().trimmed().isEmpty();
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QTemporaryDir home;
	const QByteArray base = home.path().toLocal8Bit();
	qputenv("HOME", base);
	qputenv("XDG_CONFIG_HOME", base + "/config");
	qputenv("XDG_DATA_HOME", base + "/data");
	qputenv("XDG_CACHE_HOME", base + "/cache");
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);

	stream_server srv;
	srv.listen(QHostAddress::LocalHost, 0);
	const QString origin = QString("http://127.0.0.1:%1").arg(srv.serverPort());

	// The stream, made by ffmpeg with a key it is told to name by address.
	{
		const QString dir = home.path() + "/enc";
		QDir().mkpath(dir);
		QFile k(dir + "/k.key");
		k.open(QIODevice::WriteOnly);
		k.write(QByteArray::fromHex("0f1e2d3c4b5a69788796a5b4c3d2e1f0"));
		k.close();
		QFile info(dir + "/k.info");
		info.open(QIODevice::WriteOnly);
		info.write((origin + "/enc/k.key\n" + dir + "/k.key\n").toUtf8());
		info.close();
		QProcess ff;
		ff.setProcessChannelMode(QProcess::MergedChannels);
		ff.start("ffmpeg", { "-v", "error", "-y", "-f", "lavfi", "-i",
		                     "testsrc=duration=6:size=160x120:rate=10",
		                     "-c:v", "mpeg2video", "-f", "hls", "-hls_time", "2",
		                     "-hls_playlist_type", "vod",
		                     "-hls_key_info_file", dir + "/k.info",
		                     "-hls_segment_filename", dir + "/s%d.ts",
		                     dir + "/media.m3u8" });
		if (!ff.waitForFinished(60000) || ff.exitCode() != 0) {
			std::printf("ffmpeg could not make the stream: %s\n",
			            ff.readAll().constData());
			return 1;
		}
		for (const QString &f : QDir(dir).entryList({ "*.ts", "*.m3u8", "k.key" })) {
			QFile in(dir + "/" + f);
			in.open(QIODevice::ReadOnly);
			srv.files["/enc/" + f.toUtf8()] = in.readAll();
		}
		srv.files["/enc/master.m3u8"] =
		  "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=200000,RESOLUTION=160x120\n"
		  "media.m3u8\n";
		srv.files["/watch"] =
		  "<!doctype html><title>An encrypted clip</title>"
		  "<video src=\"/enc/master.m3u8\" controls></video>"
		  "<script>fetch('/enc/master.m3u8').then(r => r.text())"
		  ".then(t => fetch('/enc/media.m3u8'));</script>";
	}
	check(srv.files.value("/enc/media.m3u8").contains("METHOD=AES-128") &&
	          !srv.files.value("/enc/s0.ts").startsWith('\x47'),
	      "the stream is encrypted: its playlist says AES-128 and a segment is "
	      "not plain MPEG-TS");

	const QString profile = home.path() + "/profile";
	QDir().mkpath(profile);
	{
		QFile tf(profile + "/tree.txt");
		tf.open(QIODevice::WriteOnly);
		tf.write(QString("- [f0] folder | Mine\n  - [a1] unopened | Clip | "
		                 "%1/watch | created=2026-01-01T00:00:00 | "
		                 "seen=2026-01-01T00:00:00\n").arg(origin).toUtf8());
	}
	policy_engine policy;
	request_filter filter(&policy);
	qtwebengine_factory factory(&filter);
	auto *w = new main_window(&factory, &policy, &filter);
	w->load_tree(profile + "/tree.txt");
	w->resize(1000, 700);
	w->show();
	spin(500);
	auto *tree = w->findChild<QTreeView *>();
	emit tree->activated(tree->model()->index(0, 0, tree->model()->index(0, 0)));
	spin(4000);

	// Everything the window says on its status bar, for a failure to quote.
	QStringList said;
	if (auto *bar = w->findChild<QStatusBar *>())
		QObject::connect(bar, &QStatusBar::messageChanged,
		                  [&said](const QString &m) { if (!m.isEmpty()) said << m; });
	const QDir downloads(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
	const auto files_now = [&downloads] {
		return downloads.entryList(QDir::Files, QDir::Time);
	};

	// Open Find Media, let `during` press what it likes, close it.
	auto with_dialog = [&](const std::function<void(QDialog *)> &during) {
		QMetaObject::invokeMethod(w, "open_media");
		QDialog *dlg = nullptr;
		for (int i = 0; i < 100 && !dlg; ++i) {
			for (QWidget *x : QApplication::topLevelWidgets())
				if (auto *d = qobject_cast<QDialog *>(x); d && d->isVisible())
					dlg = d;
			if (!dlg)
				spin(50);
		}
		if (!dlg) {
			check(false, "Find Media opened");
			return;
		}
		during(dlg);
		dlg->reject();
		spin(200);
	};
	auto note = [](QDialog *d) {
		auto *l = d->findChild<QLabel *>("media_lookup_note");
		return l ? l->text() : QString("(no note)");
	};
	auto wait_lookup = [&](QDialog *d) {
		for (int i = 0; i < 900; ++i) {
			const QString t = note(d);
			if (t.startsWith("yt-dlp"))
				return t;
			spin(100);
		}
		return note(d);
	};
	// Every row's text, for a failure to show what was there instead.
	auto rows = [](QTreeWidget *list) {
		QStringList out;
		QList<QTreeWidgetItem *> all;
		for (int i = 0; list && i < list->topLevelItemCount(); ++i) {
			all << list->topLevelItem(i);
			for (int c = 0; c < list->topLevelItem(i)->childCount(); ++c)
				all << list->topLevelItem(i)->child(c);
		}
		for (QTreeWidgetItem *it : all) {
			QStringList cols;
			for (int c = 0; c < it->columnCount(); ++c)
				cols << it->text(c);
			out << QString("[%1] %2").arg(it->data(0, Qt::UserRole).toBool()
			                                ? "yt-dlp" : "seen", cols.join(" | "));
		}
		return out;
	};
	// Press Download on the first row that is (or is not) yt-dlp's and names
	// the stream, and wait for a file in Downloads that is not one already.
	auto press = [&](QDialog *d, bool vouched, QString *got) {
		auto *list = d->findChild<QTreeWidget *>();
		// Detection's rows sit in an "Also on this page" group, so children
		// are rows too.
		QList<QTreeWidgetItem *> all;
		for (int i = 0; list && i < list->topLevelItemCount(); ++i) {
			all << list->topLevelItem(i);
			list->topLevelItem(i)->setExpanded(true);
			for (int c = 0; c < list->topLevelItem(i)->childCount(); ++c)
				all << list->topLevelItem(i)->child(c);
		}
		spin(200);
		for (QTreeWidgetItem *it : all) {
			if (it->data(0, Qt::UserRole).toBool() != vouched)
				continue;
			QPushButton *download = nullptr;
			for (int c = 0; c < list->columnCount() && !download; ++c)
				if (QWidget *cell = list->itemWidget(it, c))
					for (auto *b : cell->findChildren<QPushButton *>())
						if (b->toolTip() == "Download" && b->isVisible() && b->isEnabled())
							download = b;
			if (!download)
				continue;
			const QStringList before = files_now();
			download->click();
			for (int t = 0; t < 1200; ++t) {
				spin(100);
				for (const QString &f : files_now())
					if (!before.contains(f) && f.endsWith(".mp4") && QFileInfo(downloads.filePath(f)).size() > 0) {
						spin(1500);   // the rewrap renames as it ends
						*got = downloads.filePath(f);
						return true;
					}
			}
			*got = QString("no new .mp4 under %1; there: %2; said: %3")
			         .arg(downloads.path(), files_now().join(", "),
			              QStringList(said).join(" | "));
			return false;
		}
		*got = "no row with a Download button: " + rows(list).join(" / ");
		return false;
	};

	with_dialog([&](QDialog *d) {
		const QString said = wait_lookup(d);
		auto *list = d->findChild<QTreeWidget *>();
		std::printf("  lookup: %s\n  rows:\n    %s\n", qPrintable(said),
		            qPrintable(rows(list).join("\n    ")));

		const int keys_before = srv.key_fetches;
		QString got;
		const bool saved = press(d, false, &got);
		check(saved && decodes(got),
		      QString("detection's row: Download leaves a file that plays (%1)").arg(got));
		check(srv.key_fetches > keys_before,
		      QString("and the key was fetched for it (%1 times)")
		          .arg(srv.key_fetches - keys_before));
		std::printf("  status: %s\n", qPrintable(note(d)));

		const int keys_mid = srv.key_fetches;
		QString got2;
		const bool saved2 = press(d, true, &got2);
		check(saved2 && decodes(got2),
		      QString("yt-dlp's row: Download leaves a file that plays (%1)").arg(got2));
		check(srv.key_fetches > keys_mid,
		      QString("and the key was fetched for it (%1 times)")
		          .arg(srv.key_fetches - keys_mid));
	});

	w->close();
	delete w;
	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
