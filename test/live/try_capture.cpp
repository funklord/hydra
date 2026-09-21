#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"
#include "torrent_download_source.h"
#include "download_manager.h"
#include "mse_tap.h"
#include "sample_tree.h"
#include "media_fixture.h"

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
	// **A fixture of its own, because the default was a port nobody served.**
	// This read `http://127.0.0.1:8840/index.html` and started no server, so
	// every sweep navigated nowhere, captured nothing for sixty seconds and
	// reported "done" -- a report-only success over a page that does not
	// exist. `media_fixture.h` records the same fault being fixed in
	// try_downloads, and this driver was the one it missed. A real url is
	// still what the argument is for.
	media_fixture::server fixture;
	const bool own_fixture = argc <= 1;
	const QString target = argc > 1 ? QString::fromLocal8Bit(argv[1])
	                                 : fixture.start();
	if (target.isEmpty()) {
		std::printf("HYDRA-SKIP: the fixture server could not listen\n");
		return 1;
	}
	std::printf("serving: %s\n", qPrintable(target));
	const QString outdir = argc > 2 ? argv[2] : QDir::temp().filePath("hydra-cap");

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	// A local page: this driver activates a row to have a tab open, not
	// to load anything in particular, and the committed example's first
	// row is `doc.qt.io`. See `local_page_tree` in sample_tree.h.
	w.load_tree(shell::local_page_tree());
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
					// **Written where this driver's other output goes, and
					// the status read.** This said
					// `test_out() + "scratchpad/live/..."` -- a path segment
					// from somebody's own scratch layout, which no other
					// driver uses and which nothing creates. `QPixmap::save`
					// answers false for a directory that is not there and the
					// result went unread, so both pictures of the downloads
					// window during a capture have never been written: zero
					// across every run in this tree's history. The same shape
					// try_settings' `screen()` carries a paragraph about, in a
					// driver that had not learned it.
					const QString shot =
					  test_out() + QString("50-capjob-%1.png").arg(t);
					QDir().mkpath(test_out());
					if (ww->grab().save(shot))
						std::printf("  shot  %s\n", qPrintable(shot));
					else
						std::printf("  !!    could not write %s\n",
						             qPrintable(shot));
					// **Every row, with the count, rather than the first
					// one.** The downloads list survives a run -- the drivers
					// keep their state under ~/.qttest on purpose -- so
					// `topLevelItem(0)` is whatever the oldest run left, and
					// this driver spent its output describing a previous run's
					// failed capture as though it were this one's. Measured:
					// the row printed here read `...-015247.mp4 | 0 B | Failed
					// -- Nothing was captured` while the file this run wrote
					// was `...-095427.mp4`, 2048 bytes.
					//
					// A report-only driver exists to be read, so the answer is
					// to print what is there and let the reader see which row
					// is theirs, not to guess better.
					auto *tree = ww->findChild<QTreeWidget *>();
					if (tree) {
						std::printf("t+%-6d %d row(s) in the list\n", t,
						             tree->topLevelItemCount());
						for (int i = 0; i < tree->topLevelItemCount(); ++i)
							std::printf("t+%-6d   row %d: %s | %s | %s | %s\n",
							             t, i,
							             qPrintable(tree->topLevelItem(i)->text(0)),
							             qPrintable(tree->topLevelItem(i)->text(1)),
							             qPrintable(tree->topLevelItem(i)->text(3)),
							             qPrintable(tree->topLevelItem(i)->text(4)));
					}
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
		// **What the fixture was actually asked for.** `media_fixture` keeps
		// this list for a driver that wants to say what was fetched rather
		// than trusting that it was, and until now no driver used it. It is
		// the evidence this one lacked: with the target pointed at a closed
		// port, "navigated" still printed and nothing said the page had not
		// been served. An empty list while serving our own fixture means the
		// run measured nothing, whatever the rest of the output says.
		if (own_fixture) {
			std::printf("fixture served %d request(s)%s\n",
			             int(fixture.seen.size()),
			             fixture.seen.isEmpty()
			                 ? "  <- nothing was fetched; this run measured "
			                   "nothing"
			                 : ":");
			for (const QString &path : std::as_const(fixture.seen))
				std::printf("  asked: %s\n", qPrintable(path));
		}
		QDir d(outdir);
		for (const QFileInfo &fi : d.entryInfoList(QDir::Files, QDir::Time))
			std::printf("FILE %s %lld bytes\n", qPrintable(fi.fileName()), fi.size());
		std::printf("done\n");
		qApp->quit();
	});
	return app.exec();
}
