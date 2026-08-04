#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"
#include "torrent_download_source.h"
#include "media_detector.h"
#include "mse_tap.h"

#include <QAction>
#include <QApplication>
#include <QLineEdit>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>
#include <cstdio>

// Where screenshots and captures land. Set HYDRA_TEST_OUT to move it.
static QString test_out() {
	const QByteArray e = qgetenv("HYDRA_TEST_OUT");
	return (e.isEmpty() ? QString("/tmp/hydra-test/")
	                    : QString::fromLocal8Bit(e) + "/");
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	qtwebengine_factory::register_url_schemes(torrent_download_source::url_schemes());
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);
	const QString target = argv[1];

	policy_engine policy; request_filter filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree("/home/nabbe/src/hydra/sample-tree.txt");
	w.resize(1150, 780); w.show();
	auto *det = w.findChild<media_detector *>();
	auto *tap = w.findChild<mse_tap *>();

	QTimer::singleShot(2000, [&] {
		auto *t = w.findChild<QTreeView *>();
		emit t->activated(t->model()->index(0, 0, t->model()->index(0, 0)));
	});
	QTimer::singleShot(6000, [&] {
		for (QLineEdit *e : w.findChildren<QLineEdit *>())
			if (e->placeholderText() == "Address") {
				e->setText(target); QMetaObject::invokeMethod(e, "returnPressed"); return;
		  }
	});
	QTimer::singleShot(20000, [&] {
		const QString host = QUrl(target).host();
		std::printf("detector=%d  tap_active=%d\n", det->count_for(host),
		             tap->active_for(host) ? 1 : 0);
		for (QAction *a : w.findChildren<QAction *>())
			if (a->text().startsWith("Media"))
				std::printf("badge=\"%s\"\n", qPrintable(a->text()));
		// Open the media list the badge points at.
		for (QAction *a : w.findChildren<QAction *>())
			if (a->text() == "Media" || a->text().startsWith("Media (")) { a->trigger(); return; }
	});
	QTimer::singleShot(23000, [&] {
		for (QWidget *x : QApplication::topLevelWidgets()) {
			if (!x->isVisible() || !x->windowTitle().contains("Media")) continue;
			auto *tree = x->findChild<QTreeWidget *>();
			std::printf("dialog rows=%d\n", tree ? tree->topLevelItemCount() : -1);
			for (int i = 0; tree && i < tree->topLevelItemCount(); ++i)
				std::printf("  row %d: [%s] %s\n", i,
				             qPrintable(tree->topLevelItem(i)->text(0)),
				             qPrintable(tree->topLevelItem(i)->text(1)));
			x->grab().save(test_out());
			x->close();
			return;
		}
		std::printf("NO MEDIA DIALOG\n");
	});
	QTimer::singleShot(26000, [] { std::printf("done\n"); qApp->quit(); });
	return app.exec();
}
