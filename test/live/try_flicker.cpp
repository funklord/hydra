// What is actually on screen in the moments after a tab is opened?
#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"
#include "torrent_download_source.h"
#include "sample_tree.h"

#include <QApplication>
#include <QPlatformSurfaceEvent>
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QProcess>
#include <QTimer>
#include <QTreeView>
#include <QUrl>
#include <cstdio>

// Where screenshots and captures land. Set HYDRA_TEST_OUT to move it.
static QString test_out() {
	const QByteArray e = qgetenv("HYDRA_TEST_OUT");
	return (e.isEmpty() ? QString("/tmp/hydra-test/")
	                    : QString::fromLocal8Bit(e) + "/");
}

static const QString OUTDIR =
  test_out();

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	qtwebengine_factory::register_url_schemes(torrent_download_source::url_schemes());
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);
	QDir().mkpath(OUTDIR);

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	// Log exactly when the platform surface goes away and comes back.
	class watcher : public QObject {
	public:
		bool eventFilter(QObject *o, QEvent *e) override {
			if (e->type() == QEvent::PlatformSurface) {
				auto *pe = static_cast<QPlatformSurfaceEvent *>(e);
				std::printf("HYDRA-SURFACE %s\n",
				             pe->surfaceEventType() ==
				                 QPlatformSurfaceEvent::SurfaceCreated
				                 ? "created" : "about-to-be-destroyed");
			} else if (e->type() == QEvent::WinIdChange) {
				std::printf("HYDRA-SURFACE winIdChange\n");
			}
			return QObject::eventFilter(o, e);
		}
	};
	w.installEventFilter(new watcher);

	// **A local page, not the committed example's first tab.** This opened
	// `inert_sample_tree()` and activated its first child, which in the sample
	// is `doc.qt.io` -- so a sweep fetched a real site and pulled amplitude and
	// Simple Analytics with it, which is how this was noticed: the driver's own
	// output carried the Amplitude logger's warnings. See `local_page_tree`.
	//
	// It also makes the measurement mean something. What this reports is how
	// the shell paints in the moments after a tab opens, and while the page
	// came off the network those timings moved with a remote site's week rather
	// than with this code.
	w.load_tree(shell::local_page_tree());
	w.resize(1100, 780);
	w.show();

	QElapsedTimer t;
	// Sample densely right after the switch, then thin out.
	const QList<int> at = { 0, 40, 90, 150, 240, 360, 520, 750, 1100, 1700, 2600 };

	QTimer::singleShot(2500, [&] {
		auto *tree = w.findChild<QTreeView *>();
		const QModelIndex first = tree->model()->index(0, 0);
		const QModelIndex kid   = tree->model()->index(0, 0, first);
		std::printf("activating: %s\n", qPrintable(kid.data().toString()));
		t.start();
		emit tree->activated(kid);
		for (int ms : at) {
			QTimer::singleShot(ms, [&w, ms, &t] {
				const QPixmap p = w.grab();
				p.save(OUTDIR + QString("f%1.png").arg(ms, 5, 10, QChar('0')));
				std::printf("t+%-5d grabbed (elapsed %lld)\n", ms, t.elapsed());
			});
		}
		QTimer::singleShot(22000, [] {
			std::printf("done\n");
			qApp->quit();
		});
	});
	return app.exec();
}
