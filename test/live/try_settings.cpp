// Runs the real main_window on the real display and drives the Settings dialog
// through its actual menu action -- no synthetic input available, so the action
// is triggered directly, but everything below that is the shipping code path.
#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"
#include "torrent_download_source.h"
#include "sample_tree.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QTimer>
#include <cstdio>

// Where screenshots and captures land. Set HYDRA_TEST_OUT to move it.
static QString test_out() {
	const QByteArray e = qgetenv("HYDRA_TEST_OUT");
	return (e.isEmpty() ? QString("/tmp/hydra-test/")
	                    : QString::fromLocal8Bit(e) + "/");
}

static const QString SHOT =
  test_out();

static void screen(const QString &name) {
	// **Make the directory first.** `import` cannot create one, so with a fresh
	// HYDRA_TEST_OUT every screenshot this driver exists to produce failed to
	// write -- while the run still printed "done" and exited 0. A driver whose
	// entire output is pictures, reporting success having written none, is the
	// apparatus lying in the usual direction.
	QDir().mkpath(SHOT);
	// The whole root window: proves this is genuinely on screen rather than a
	// widget rendered into an offscreen buffer.
	if (QProcess::execute("import", {"-window", "root", SHOT + name}) != 0)
		std::printf("  !!    could not write %s\n", qPrintable(SHOT + name));
}

// **Show a settings page, by the name a person reads.**
//
// This driver used to do it with `findChild<QTabWidget *>()` and
// `setCurrentIndex`, and the settings dialog is a QListWidget driving a
// QStackedWidget -- there is no QTabWidget in it. The lookup returned null
// inside an `if`, so the page never changed and nothing failed: three of the
// pictures were byte-identical captures of Privacy & security, filed as
// `02-settings-player`, `03-settings-downloads` and `04-settings-ai`. The
// behaviour underneath was fine, because a stacked page stays a child of the
// dialog whether or not it is showing, so the buttons this driver presses were
// found and pressed on pages nobody could see.
//
// By label rather than by row, so that reordering the sections cannot quietly
// photograph the wrong one; and it says so when it cannot, since a lookup that
// fails silently is the whole of what went wrong here.
static bool show_page(QDialog *d, const QString &label) {
	auto *list = d ? d->findChild<QListWidget *>("categories") : nullptr;
	if (!list) {
		std::printf("  !!    no settings category list, so no page was shown\n");
		return false;
	}
	// **Both sides through the same transformation.** Stripping `&` from the
	// item text alone compares "Media  players" against "Media & players" and
	// never matches -- the section called *Media & players* carries a literal
	// ampersand as well as a mnemonic one, and `remove` cannot tell them
	// apart. Normalising the wanted label the same way makes the comparison
	// about the words rather than about the markup.
	const QString want = QString(label).remove('&').simplified();
	for (int i = 0; i < list->count(); ++i)
		if (list->item(i)->text().remove('&').simplified() == want) {
			list->setCurrentRow(i);
			std::printf("showing page: %s\n", qPrintable(label));
			return true;
		}
	std::printf("  !!    no settings page named \"%s\"\n", qPrintable(label));
	return false;
}

static QDialog *find_settings() {
	for (QWidget *w : QApplication::topLevelWidgets())
		if (auto *d = qobject_cast<QDialog *>(w))
			if (d->windowTitle() == "Settings")
				return d;
	return nullptr;
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	qtwebengine_factory::register_url_schemes(torrent_download_source::url_schemes());
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);
	app.setApplicationName("Hydra");

	// **Refuse up front where the pictures cannot be taken.** This driver's
	// entire output is screenshots, and `screen()` takes them by running
	// `import -window root`, which needs a real X display. Offscreen -- the
	// sweep's default -- every one of the seven fails, the driver prints
	// `!!` seven times and exits 0, and the sweep reports it as a report-only
	// driver that ran to the end. That is the same claim a successful run
	// makes, about a run that produced nothing.
	//
	// The comment on `screen()` already names this shape for the case where
	// the directory did not exist; this is the other way in and it was still
	// open. Probed by taking one picture rather than by asking what platform
	// or display this is, for the reason try_keepass tests its socket instead
	// of looking for a process: the question is whether the thing works.
	{
		QDir().mkpath(SHOT);
		const QString probe = SHOT + "00-probe.png";
		QFile::remove(probe);
		const bool can_capture =
		  QProcess::execute("import", { "-window", "root", probe }) == 0
		  && QFileInfo(probe).size() > 0;
		QFile::remove(probe);
		if (!can_capture) {
			std::printf("HYDRA-SKIP: import(1) cannot capture here, so every "
			             "picture this driver exists to take would be missing\n");
			std::printf("done\n");
			return 1;
		}
	}

	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	w.load_tree(shell::inert_sample_tree());
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
		// A page change and its screenshot must not share a tick: import runs
		// as a subprocess and would capture the frame before the repaint.
		case 2:
			std::printf("settings open: %s\n",
			             find_settings() ? "yes" : "NO");
			show_page(find_settings(), "Media & players");
			break;
		case 3:
			screen("02-settings-player.png");
			break;
		case 4:
			show_page(find_settings(), "Downloads");
			break;
		case 5:
			screen("03-settings-downloads.png");
			break;
		case 6:
			show_page(find_settings(), "AI");
			break;
		case 7:
			screen("04-settings-ai.png");
			break;
		case 8:
			if (auto *d = find_settings()) {
				if (auto *b = d->findChild<QPushButton *>("check_local")) {
					std::printf("pressing Check now\n");
					b->click();
				}
			}
			break;
		case 9:
			screen("05-after-check.png");
			if (auto *d = find_settings())
				if (auto *l = d->findChild<QLabel *>("ai_status"))
					std::printf("status: %s\n", qPrintable(l->text()));
			break;
		case 10:
			if (auto *d = find_settings()) {
				show_page(d, "Media & players");
				if (auto *b = d->findChild<QPushButton *>("rescan_players")) {
					std::printf("pressing Rescan\n");
					b->click();
				}
			}
			break;
		case 11:
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
