// Cancel a capture from the downloads window, mid-recording.
#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"
#include "torrent_download_source.h"
#include "download_manager.h"
#include "sample_tree.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>
#include <cstdio>

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	qtwebengine_factory::register_url_schemes(torrent_download_source::url_schemes());
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);
	const QString target = argv[1], outdir = argv[2];

	policy_engine policy; request_filter filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(shell::inert_sample_tree());
	w.resize(1100, 760); w.show();
	QDir().mkpath(outdir);
	auto *dm = w.findChild<download_manager *>();
	dm->set_directory(outdir);

	auto action = [&](const QString &f) -> QAction * {
		for (QAction *a : w.findChildren<QAction *>())
			if (a->text().contains(f)) return a;
		return nullptr;
	};
	auto dlwin = [&]() -> QWidget * {
		for (QWidget *x : QApplication::topLevelWidgets())
			if (x->isVisible() && x->windowTitle().contains("Downloads")) return x;
		return nullptr;
	};
	auto report = [&](const char *when) {
		for (const download_job &j : dm->jobs())
			std::printf("%-14s job %d [%s] status=%d received=%lld path=%s\n",
			             when, j.id, qPrintable(j.source_id), int(j.status),
			             j.received, qPrintable(QFileInfo(j.path).fileName()));
	};

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
	QTimer::singleShot(13000, [&] {
		if (QAction *a = action("Capture Playing")) { std::printf("arming\n"); a->trigger(); }
	});
	QTimer::singleShot(15000, [&] {
		if (QAction *d = action("Downloads")) d->trigger();
	});
	QTimer::singleShot(18000, [&] {
		report("before-cancel");
		QWidget *win = dlwin();
		if (!win) { std::printf("NO WINDOW\n"); return; }
		auto *tree = win->findChild<QTreeWidget *>();
		if (tree && tree->topLevelItemCount()) tree->setCurrentItem(tree->topLevelItem(0));
		for (QPushButton *b : win->findChildren<QPushButton *>())
			if (b->text().contains("Cancel") && b->isEnabled()) {
				std::printf("clicking Cancel\n"); b->click(); return;
		  }
		std::printf("NO ENABLED CANCEL\n");
	});
	QTimer::singleShot(21000, [&] { report("after-cancel"); });
	QTimer::singleShot(27000, [&] {
		report("later");
		if (QAction *a = action("Capture Playing"))
			std::printf("action checked=%d text=\"%s\"\n", a->isChecked(),
			             qPrintable(a->text()));
		QDir d(outdir);
		for (const QFileInfo &fi : d.entryInfoList(QDir::Files))
			std::printf("FILE %s %lld\n", qPrintable(fi.fileName()), fi.size());
		std::printf("done\n"); qApp->quit();
	});
	return app.exec();
}
