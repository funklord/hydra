// Runs the real main_window on the real display and drives the Settings dialog
// through its actual menu action -- no synthetic input available, so the action
// is triggered directly, but everything below that is the shipping code path.
#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"
#include "torrent_download_source.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QLabel>
#include <QProcess>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <cstdio>

static const QString SHOT =
	test_out();

static void screen(const QString &name) {
	// The whole root window: proves this is genuinely on screen rather than a
	// widget rendered into an offscreen buffer.
	QProcess::execute("import", {"-window", "root", SHOT + name});
}

static QDialog *find_settings() {
	for (QWidget *w : QApplication::topLevelWidgets())
		if (auto *d = qobject_cast<QDialog *>(w))
			if (d->windowTitle() == "Settings")
				return d;
	return nullptr;
}

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
	app.setApplicationName("Hydra");

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(QDir("/home/nabbe/src/hydra").filePath("sample-tree.txt"));
	w.show();

	int step = 0;
	auto *tick = new QTimer(&app);
	tick->setInterval(1500);
	QObject::connect(tick, &QTimer::timeout, [&] {
		switch (step++) {
		case 0:
			screen("01-app.png");
			std::printf("app shown\n");
			break;
		case 1: {
			// The real menu action, found by its text.
			QAction *settings = nullptr;
			for (QAction *a : w.findChildren<QAction *>())
				if (a->text().contains("Settings"))
					settings = a;
			if (!settings) { std::printf("NO SETTINGS ACTION\n"); qApp->quit(); return; }
			std::printf("triggering: %s\n", qPrintable(settings->text()));
			// exec() blocks here, so the remaining steps run inside its loop.
			QTimer::singleShot(0, [settings] { settings->trigger(); });
			break;
		}
		case 2:
			screen("02-settings-player.png");
			std::printf("settings open: %s\n",
			             find_settings() ? "yes" : "NO");
			break;
		// A tab switch and its screenshot must not share a tick: import runs
		// as a subprocess and would capture the frame before the repaint.
		case 3:
			if (auto *d = find_settings())
				if (auto *t = d->findChild<QTabWidget *>())
					t->setCurrentIndex(1);
			break;
		case 4:
			screen("03-settings-downloads.png");
			break;
		case 5:
			if (auto *d = find_settings())
				if (auto *t = d->findChild<QTabWidget *>())
					t->setCurrentIndex(2);
			break;
		case 6:
			screen("04-settings-ai.png");
			break;
		case 7:
			if (auto *d = find_settings()) {
				if (auto *b = d->findChild<QPushButton *>("check_local")) {
					std::printf("pressing Check now\n");
					b->click();
				}
			}
			break;
		case 8:
			screen("05-after-check.png");
			if (auto *d = find_settings())
				if (auto *l = d->findChild<QLabel *>("ai_status"))
					std::printf("status: %s\n", qPrintable(l->text()));
			break;
		case 9:
			if (auto *d = find_settings()) {
				if (auto *t = d->findChild<QTabWidget *>())
					t->setCurrentIndex(0);
				if (auto *b = d->findChild<QPushButton *>("rescan_players")) {
					std::printf("pressing Rescan\n");
					b->click();
				}
			}
			break;
		case 10:
			screen("06-after-rescan.png");
			if (auto *d = find_settings())
				d->reject();          // Cancel
			std::printf("dialog cancelled\n");
			break;
		default:
			screen("07-back-to-app.png");
			std::printf("done\n");
			qApp->quit();
		}
	});
	tick->start();
	return app.exec();
}
