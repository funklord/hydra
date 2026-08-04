#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"
#include "torrent_download_source.h"
#include "download_manager.h"
#include "mse_tap.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QLineEdit>
#include <QTimer>
#include <QTreeView>
#include <QStatusBar>
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
	const QString target = argc > 1 ? argv[1] : "http://127.0.0.1:8840/index.html";
	const QString outdir = argc > 2 ? argv[2] : QDir::temp().filePath("hydra-cap");

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree("/home/nabbe/src/hydra/sample-tree.txt");
	w.resize(1100, 760);
	w.show();
	QDir().mkpath(outdir);
	if (auto *dm = w.findChild<download_manager *>())
		dm->set_directory(outdir);

	auto action = [&](const QString &frag) -> QAction * {
		for (QAction *a : w.findChildren<QAction *>())
			if (a->text().contains(frag)) return a;
		return nullptr;
	};

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
	QTimer::singleShot(13000, [&] {
		QAction *a = action("Capture Playing");
		if (!a) { std::printf("NO CAPTURE ACTION\n"); return; }
		std::printf("arming capture\n");
		a->trigger();          // injects and reloads
	});
	// Open the downloads window and look at the capture as a job.
	QTimer::singleShot(17000, [&] {
		QAction *d = action("Downloads");
		if (d) { std::printf("opening downloads\n"); d->trigger(); }
	});
	for (int t : { 20000, 24000 }) {
		QTimer::singleShot(t, [&, t] {
			for (QWidget *ww : QApplication::topLevelWidgets())
				if (ww->isVisible() && ww->windowTitle().contains("Downloads")) {
					ww->grab().save((test_out() +
					                 "scratchpad/live/50-capjob-%1.png").arg(t));
				  auto *tree = ww->findChild<QTreeWidget *>();
				  if (tree && tree->topLevelItemCount())
					  std::printf("t+%-6d row: %s | %s | %s | %s\n", t,
					               qPrintable(tree->topLevelItem(0)->text(0)),
					               qPrintable(tree->topLevelItem(0)->text(1)),
					               qPrintable(tree->topLevelItem(0)->text(3)),
					               qPrintable(tree->topLevelItem(0)->text(4)));
				  return;
			  }
			std::printf("t+%-6d no downloads window\n", t);
		});
	}
	for (int t : { 22000, 30000, 45000, 60000 }) {
		QTimer::singleShot(t, [&, t] {
			QAction *a = action("Capture Playing");
			QString bar;
			for (QStatusBar *sb : w.findChildren<QStatusBar *>())
				bar = sb->currentMessage();
			std::printf("t+%-6d action=\"%s\"  status=\"%s\"\n", t,
			             a ? qPrintable(a->text()) : "?", qPrintable(bar));
		});
	}
	QTimer::singleShot(75000, [&] {
		QAction *a = action("Capture Playing");
		if (a) { std::printf("stopping capture\n"); a->trigger(); }
	});
	QTimer::singleShot(79000, [&] {
		QDir d(outdir);
		for (const QFileInfo &fi : d.entryInfoList(QDir::Files, QDir::Time))
			std::printf("FILE %s %lld bytes\n", qPrintable(fi.fileName()), fi.size());
		std::printf("done\n");
		qApp->quit();
	});
	return app.exec();
}
