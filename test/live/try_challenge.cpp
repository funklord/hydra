// Whether a Cloudflare "verify you are human" check passes in this browser,
// and what makes the difference. Live: it needs the network, and a display --
// run it under `xvfb-run -a`, since the check reads the graphics stack too.
//
//     xvfb-run -a ./test/build-make/try_challenge <url> [as-is|qt-ua|no-hook|both]
//
// Prints the page's title once a second for forty seconds. Cloudflare's
// interstitial is titled "Just a moment..." and the title changes when the
// check passes, so the line it ends on is the answer.
//
// **Why this exists**: the holder reported the check failing on coupert, and
// two things this browser does are known to be what such a check reads. The
// user agent claims Chrome 140 while the engine's client hints say 122, and a
// main-world MediaSource hook runs in every frame, the challenge's included.
// The modes take each away: `qt-ua` sends Qt's own user agent, `no-hook`
// removes the hook, `both` does both.
#include "main_window.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "request_filter.h"
#include "web_view_backend.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeView>
#include <QWebEngineProfile>
#include <cstdio>

static void spin(int ms) {
	QEventLoop l;
	QTimer::singleShot(ms, &l, &QEventLoop::quit);
	l.exec();
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
	if (argc < 2) {
		std::printf("usage: try_challenge <url> [as-is|qt-ua|no-hook|both]\n");
		return 2;
	}
	const QString url  = QString::fromLocal8Bit(argv[1]);
	const QString mode = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("as-is");

	const QString profile = home.path() + "/profile";
	QDir().mkpath(profile);
	{
		QFile tf(profile + "/tree.txt");
		tf.open(QIODevice::WriteOnly);
		tf.write(QString("- [f0] folder | Mine\n  - [a1] unopened | Page | %1 | "
		                 "created=2026-01-01T00:00:00 | seen=2026-01-01T00:00:00\n")
		             .arg(url).toUtf8());
	}

	policy_engine policy;
	request_filter filter(&policy);
	qtwebengine_factory factory(&filter);
	const QString ours = factory.profile()->httpUserAgent();
	if (mode == "qt-ua" || mode == "both") {
		// Qt's own string, as the engine would send it unasked: the corrected
		// one is set on the profile by the factory, so taking it back here is
		// the whole of the experiment.
		QWebEngineProfile plain;
		factory.profile()->setHttpUserAgent(plain.httpUserAgent());
	}
	std::printf("mode %s\nuser agent: %s\n", qPrintable(mode),
	            qPrintable(factory.profile()->httpUserAgent()));
	if (factory.profile()->httpUserAgent() != ours)
		std::printf("(hydra's would be: %s)\n", qPrintable(ours));

	main_window w(&factory, &policy, &filter);
	w.load_tree(profile + "/tree.txt");
	w.resize(1280, 900);
	w.show();
	spin(500);
	auto *tree = w.findChild<QTreeView *>();
	emit tree->activated(tree->model()->index(0, 0, tree->model()->index(0, 0)));
	spin(300);
	web_view_backend *view = w.findChild<web_view_backend *>();
	if (!view) {
		std::printf("no view\n");
		return 1;
	}
	if (mode == "no-hook" || mode == "both") {
		view->remove_script(QStringLiteral("hydra-mse-hook"));
		view->reload();
		std::printf("MediaSource hook removed\n");
	}

	QString last;
	for (int s = 0; s < 40; ++s) {
		spin(1000);
		const QString t = view->page_title();
		if (t != last) {
			std::printf("%2ds  %s\n", s + 1, qPrintable(t));
			last = t;
		}
	}
	std::printf("ended on: %s\n", qPrintable(last));
	return 0;
}
