#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"
#include "torrent_download_source.h"
#include "media_detector.h"
#include "mse_tap.h"
#include "sample_tree.h"

#include <QAction>
#include <QApplication>
#include <QLineEdit>
#include <QMouseEvent>
#include <QTimer>
#include <QTreeView>
#include <QWebEngineView>
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
	const QString target = argc > 1 ? argv[1] : "";

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(shell::inert_sample_tree());
	w.resize(1200, 820);
	w.show();

	auto *tap = w.findChild<mse_tap *>();
	auto *det = w.findChild<media_detector *>();
	std::printf("mse_tap: %s\n", tap ? "found" : "MISSING");

	QTimer::singleShot(2000, [&] {
		auto *tree = w.findChild<QTreeView *>();
		emit tree->activated(tree->model()->index(0, 0, tree->model()->index(0, 0)));
	});
	QTimer::singleShot(6000, [&] {
		for (QLineEdit *e : w.findChildren<QLineEdit *>())
			if (e->placeholderText() == "Address") {
				e->setText(target);
			  QMetaObject::invokeMethod(e, "returnPressed");
			  std::printf("navigated\n");
			  return;
		  }
	});
	QTimer::singleShot(20000, [&] {
		const auto vs = w.findChildren<QWebEngineView *>();
		if (vs.isEmpty()) return;
		QWidget *t = vs.last()->focusProxy() ? vs.last()->focusProxy() : vs.last();
		const QPoint at(t->width()/2, t->height()/2);
		for (int i = 0; i < 2; ++i) {
			QMouseEvent p(QEvent::MouseButtonPress, at, t->mapToGlobal(at),
			               Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
			QMouseEvent r(QEvent::MouseButtonRelease, at, t->mapToGlobal(at),
			               Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
			QApplication::sendEvent(t, &p);
			QApplication::sendEvent(t, &r);
		}
		std::printf("clicked play\n");
	});
	auto report = [&](const char *when) {
		const QString host = QUrl(target).host();
		std::printf("\n--- %s ---\n", when);
		std::printf("detector items: %d\n", det ? det->count_for(host) : -1);
		std::printf("tap active: %s\n",
		             tap && tap->active_for(host) ? "yes" : "no");
		if (tap)
			for (const mse_stream &s : tap->streams_for(host))
				std::printf("   %s  bytes=%lld appends=%d pos=%.1f dur=%.1f\n",
				             qPrintable(s.mime), s.bytes, s.appends,
				             s.position, s.duration);
		for (QAction *a : w.findChildren<QAction *>())
			if (a->text().startsWith("Media"))
				std::printf("badge: visible=%d text=\"%s\"\n", a->isVisible(),
				             qPrintable(a->text()));
	};
	QTimer::singleShot(38000, [&] { report("after playback"); w.grab().save(OUTDIR + "40-tap.png"); });
	QTimer::singleShot(41000, [] { std::printf("done\n"); qApp->quit(); });
	return app.exec();
}
