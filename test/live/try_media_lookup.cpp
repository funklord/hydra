// Find Media looks the page up when it opens, keeps the answer per page, and
// Refresh asks again -- in the real window, with the real yt-dlp, against a
// page served here. Live: it needs a display, yt-dlp and ffmpeg.
//
//     xvfb-run -a ./test/build-make/try_media_lookup
//
// **Why this exists**: asked for by the holder 2026-10-22. The lookup used
// to wait for a button and then close the dialog and reopen it; now it
// starts by itself and its rows arrive in the open dialog, and the unit
// suites can check the cache and the dialog apart but not the window
// joining them. The page is a plain <video>, which yt-dlp's generic
// extractor reads without any site of anybody's.
#include "main_window.h"
#include "media_detector.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "request_filter.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
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

// The page and its clip, and a count of how often the page was fetched:
// every fetch of the page is counted, the engine's and yt-dlp's alike --
// yt-dlp sends the same browser headers, so they cannot be told apart --
// and a count that does not move while the page stays loaded is a lookup
// that did not run.
class page_server : public QTcpServer {
public:
	QByteArray clip;
	int page_fetches = 0;
protected:
	void incomingConnection(qintptr fd) override {
		auto *s = new QTcpSocket(this);
		s->setSocketDescriptor(fd);
		connect(s, &QTcpSocket::readyRead, s, [this, s] {
			const QByteArray req = s->readAll();
			const bool want_clip = req.startsWith("GET /clip.mp4");
			const bool head = req.startsWith("HEAD ");
			if (req.startsWith("GET /watch"))
				++page_fetches;
			const QByteArray body = want_clip
			  ? clip
			  : QByteArray("<!doctype html><title>A clip</title>"
			               "<video src=\"/clip.mp4\" controls></video>");
			s->write("HTTP/1.1 200 OK\r\nContent-Type: " +
			         QByteArray(want_clip ? "video/mp4" : "text/html") +
			         "\r\nContent-Length: " + QByteArray::number(body.size()) +
			         "\r\nConnection: close\r\n\r\n" + (head ? QByteArray() : body));
			s->disconnectFromHost();
		});
		connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
	}
};

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

	page_server srv;
	{
		const QString clip = home.path() + "/clip.mp4";
		QProcess ff;
		ff.start("ffmpeg", { "-v", "error", "-f", "lavfi", "-i",
		                     "testsrc=duration=1:size=160x120:rate=10",
		                     "-pix_fmt", "yuv420p", "-y", clip });
		if (!ff.waitForFinished(30000) || ff.exitCode() != 0) {
			std::printf("ffmpeg could not make the clip\n");
			return 1;
		}
		QFile f(clip);
		f.open(QIODevice::ReadOnly);
		srv.clip = f.readAll();
	}
	srv.listen(QHostAddress::LocalHost, 0);
	const QString profile = home.path() + "/profile";
	QDir().mkpath(profile);
	{
		QFile tf(profile + "/tree.txt");
		tf.open(QIODevice::WriteOnly);
		tf.write(QString("- [f0] folder | Mine\n  - [a1] unopened | Clip | "
		                 "http://127.0.0.1:%1/watch | created=2026-01-01T00:00:00 | "
		                 "seen=2026-01-01T00:00:00\n").arg(srv.serverPort()).toUtf8());
	}
	policy_engine policy;
	request_filter filter(&policy);
	qtwebengine_factory factory(&filter);
	// A window on the tree with the clip's tab open, as a start of the
	// browser would leave it.
	auto start = [&] {
		auto *made = new main_window(&factory, &policy, &filter);
		made->load_tree(profile + "/tree.txt");
		made->resize(1000, 700);
		made->show();
		spin(500);
		auto *tree = made->findChild<QTreeView *>();
		emit tree->activated(tree->model()->index(0, 0, tree->model()->index(0, 0)));
		spin(3000);
		return made;
	};
	main_window *w = start();

	// Open Find Media, run `during` while it is up, and close it. The dialog
	// is modal, so `open_media` does not return until it closes: the checks
	// run from a timer inside its own event loop, and closing it is what
	// hands control back here.
	auto with_dialog = [&](const std::function<void(QDialog *)> &during) {
		auto *poll = new QTimer(w);
		poll->setInterval(50);
		int tries = 0;
		QObject::connect(poll, &QTimer::timeout, poll, [&, poll] {
			auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget());
			if (!dlg && ++tries < 100)
				return;
			poll->stop();
			poll->deleteLater();
			if (!dlg) {
				check(false, "Find Media opened");
				return;
			}
			during(dlg);
			dlg->reject();
		});
		poll->start();
		QMetaObject::invokeMethod(w, "open_media");
		spin(200);
	};
	auto note = [](QDialog *d) {
		auto *l = d->findChild<QLabel *>("media_lookup_note");
		return l ? l->text() : QString("(no note)");
	};
	// The looked-up rows only, named "[kind] title, ..." -- the page's own
	// request for the clip adds a row of detection's whenever the player
	// asks, which depends on timing and is not what is being checked.
	auto vouched_rows = [](QDialog *d) {
		auto *list = d->findChild<QTreeWidget *>();
		int n = 0;
		for (int i = 0; list && i < list->topLevelItemCount(); ++i)
			if (list->topLevelItem(i)->text(0).startsWith("["))
				++n;
		return list ? n : -1;
	};
	auto wait_done = [&](QDialog *d) {
		for (int i = 0; i < 600; ++i) {
			const QString t = note(d);
			if (t.startsWith("yt-dlp (") || t.startsWith("yt-dlp could not") ||
			    t.startsWith("yt-dlp found nothing"))
				return t;
			spin(100);
		}
		return note(d);
	};

	int rows_first = 0;
	with_dialog([&](QDialog *d) {
		check(note(d).startsWith("Looking"),
		      QString("opening it starts the lookup (%1)").arg(note(d)));
		const QString said = wait_done(d);
		rows_first = vouched_rows(d);
		check(said.startsWith("yt-dlp (") && rows_first > 0,
		      QString("and its rows arrive in the open dialog (%1 rows; %2)")
		          .arg(rows_first).arg(said));
	});
	const int fetches_after_first = srv.page_fetches;
	with_dialog([&](QDialog *d) {
		spin(500);
		check(note(d).startsWith("yt-dlp (") && vouched_rows(d) == rows_first,
		      QString("reopened, the answer is the cached one (%1)").arg(note(d)));
	});
	check(srv.page_fetches == fetches_after_first,
	      QString("without running yt-dlp again (page fetched %1 then %2 times)")
	          .arg(fetches_after_first).arg(srv.page_fetches));
	with_dialog([&](QDialog *d) {
		auto *refresh = d->findChild<QPushButton *>("media_refresh");
		if (refresh)
			refresh->click();
		check(note(d).startsWith("Looking"),
		      QString("Refresh asks again (%1)").arg(note(d)));
		wait_done(d);
		check(vouched_rows(d) == rows_first, "and lists the same rows once, not twice");
	});
	check(srv.page_fetches > fetches_after_first, "having run yt-dlp a second time");

	// **Reported 2026-10-22: the cache did not survive a restart.** The
	// browser ended and started again on the same profile, the clip's tab
	// open as it was.
	w->close();
	delete w;
	spin(500);
	w = start();
	// Taken once the engine has loaded the page again, which it must.
	const int fetches_before_restart = srv.page_fetches;
	// What the badge and Save read, asked directly: the looked-up rows are
	// back in the list before Find Media is opened. Detection's own row for
	// the clip cannot stand in for them here, since only vouched rows count.
	int vouched = 0;
	if (auto *det = w->findChild<media_detector *>())
		for (const media_item &m : det->items_for("127.0.0.1"))
			vouched += m.known_media ? 1 : 0;
	check(rows_first > 0 && vouched == rows_first,
	      QString("the cached rows are listed before Find Media is opened (%1 of %2)")
	          .arg(vouched).arg(rows_first));
	with_dialog([&](QDialog *d) {
		spin(500);
		check(note(d).startsWith("yt-dlp (") && vouched_rows(d) == rows_first,
		      QString("after a restart, the answer is the cached one (%1)").arg(note(d)));
	});
	check(srv.page_fetches == fetches_before_restart,
	      QString("without running yt-dlp again (%1 then %2)")
	          .arg(fetches_before_restart).arg(srv.page_fetches));
	delete w;

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
