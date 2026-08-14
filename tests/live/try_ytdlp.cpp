#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"
#include "torrent_download_source.h"
#include "media_detector.h"
#include "sample_tree.h"

#include <QAction>
#include <QApplication>
#include <QLineEdit>
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

static void grab(const QString &title, const QString &n) {
	for (QWidget *w : QApplication::topLevelWidgets())
		if (w->isVisible() && w->windowTitle().contains(title)) {
			w->grab().save(OUTDIR + n);
			return;
		}
	std::printf("NO WINDOW '%s'\n", qPrintable(title));
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	qtwebengine_factory::register_url_schemes(torrent_download_source::url_schemes());
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);
	// The same empty default, and it read worse here than anywhere: the log
	// said `navigated to ` with nothing after it, then `detector count for : 0`
	// and `NO WINDOW 'Watch or download'`, which is a report that the yt-dlp
	// handoff found nothing -- about a page that was never opened.
	if (argc < 2) {
		std::fprintf(stderr, "usage: try_ytdlp <url>\n"
		                      "needs a watch page; it asks yt-dlp what the video "
		                      "on that page actually is.\n");
		return 2;
	}
	const QString target = argv[1];

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(shell::inert_sample_tree());
	w.resize(1200, 820);
	w.show();
	auto *det = w.findChild<media_detector *>();

	QTimer::singleShot(2000, [&] {
		auto *tree = w.findChild<QTreeView *>();
		emit tree->activated(tree->model()->index(0, 0, tree->model()->index(0, 0)));
	});
	QTimer::singleShot(6000, [&] {
		for (QLineEdit *e : w.findChildren<QLineEdit *>())
			if (e->placeholderText() == "Address") {
				e->setText(target);
				QMetaObject::invokeMethod(e, "returnPressed");
				std::printf("navigated to %s\n", qPrintable(target));
				return;
			}
	});
	QTimer::singleShot(16000, [&] {
		for (QAction *a : w.findChildren<QAction *>())
			if (a->text().contains("Find Media")) {
				std::printf("triggering: %s\n", qPrintable(a->text()));
				a->trigger();
				return;
			}
		std::printf("NO ACTION\n");
	});
	QTimer::singleShot(40000, [&] {
		const QString host = QUrl(target).host();
		std::printf("detector count for %s: %d\n", qPrintable(host),
		             det ? det->count_for(host) : -1);
		for (const media_item &m : det->items_for(host))
			std::printf("   kind=%d  %s  |  %s\n", int(m.kind),
			             qPrintable(m.label.left(60)),
			             qPrintable(m.url.toString().left(80)));
		grab("Watch or download", "30-media.png");
		grab("Media", "30-media.png");
		grab("Hydra", "31-shell.png");
	});
	QTimer::singleShot(44000, [] { std::printf("done\n"); qApp->quit(); });
	return app.exec();
}
