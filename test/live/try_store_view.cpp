// The app's half of the store screenshots, on the desktop: each view opens
// over the demo profile and says it is drawn. Live: it needs a display.
//
//     xvfb-run -a ./test/build-make/try_store_view <view> android/assets/store
//
// With `-s "-screen 0 320x700x24"` after `-a`, Settings and Find Media are
// also checked to fit a phone's width.
//
// **Why this exists**: `make android-screenshots` waits for a view to log
// `store-view-ready <name>` and then publishes what it captures, so two things
// must hold before a phone is involved -- every view announces itself, and
// what it shows is the demo set rather than the person's own profile. The
// phone adds only the WebView's half (serving `hydra.example` from the
// package and announcing a drawn page), which this cannot reach: there is
// no `hydra.example` here, so the page views stand on a failed load.
#include "download_manager.h"
#include "main_window.h"
#include "policy_engine.h"
#include "qtwebengine_factory.h"
#include "request_filter.h"
#include "store_view.h"

#include <QAbstractButton>
#include <QDialog>
#include <QLabel>
#include <algorithm>
#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void spin(int ms) {
	QEventLoop l;
	QTimer::singleShot(ms, &l, &QEventLoop::quit);
	l.exec();
}

static QStringList g_ready;
// The narrowest the modal dialog of a view can be, and what holds it there.
static int         g_modal_min = -1;
static QStringList g_widest;

// **A phone is 320 points wide**: a Galaxy Z Fold3's cover screen, 840
// pixels at a scale of 2.625, where Settings ran off the right edge. The
// holder reported it 2026-10-22. A dialog whose minimum width is more than
// this cannot be used there, and which widgets set the minimum is the
// thing to know.
static constexpr int k_phone_width = 320;

static void measure(QWidget *dlg) {
	g_modal_min = dlg->minimumSizeHint().width();
	QList<QPair<int, QString>> w;
	for (QWidget *c : dlg->findChildren<QWidget *>()) {
		const int m = qMax(c->minimumSizeHint().width(), c->minimumWidth());
		if (m <= k_phone_width - 40)
			continue;
		QString what = c->metaObject()->className();
		if (!c->objectName().isEmpty())
			what += " " + c->objectName();
		if (auto *b = qobject_cast<QAbstractButton *>(c))
			what += " '" + b->text() + "'";
		if (auto *l = qobject_cast<QLabel *>(c))
			what += " '" + l->text().left(40) + "'";
		w << qMakePair(m, what);
	}
	std::sort(w.begin(), w.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
	for (int i = 0; i < qMin(12, int(w.size())); ++i)
		g_widest << QString("%1  %2").arg(w[i].first).arg(w[i].second);
}
static QtMessageHandler g_prior = nullptr;
static void listen(QtMsgType t, const QMessageLogContext &c, const QString &m) {
	if (m.startsWith("store-view-ready ")) {
		g_ready << m.mid(17);
		// The dialog a view opened, measured and closed. Settings is modal and
		// its exec() would hold the wait below until somebody closed it; Find
		// Media is not, so it is found among the windows rather than asked
		// for as the modal one.
		QTimer::singleShot(0, qApp, [] {
			QWidget *dlg = QApplication::activeModalWidget();
			if (!dlg)
				for (QWidget *x : QApplication::topLevelWidgets())
					if (qobject_cast<QDialog *>(x) && x->isVisible())
						dlg = x;
			if (dlg) {
				measure(dlg);
				dlg->close();
			}
		});
	}
	g_prior(t, c, m);
}

int main(int argc, char *argv[]) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	if (argc < 3) {
		std::printf("usage: try_store_view <view> <seed dir>  (views: %s)\n",
		            qPrintable(store_view::names().join(", ")));
		return 2;
	}
	QTemporaryDir home;
	const QByteArray base = home.path().toLocal8Bit();
	qputenv("HOME", base);
	qputenv("XDG_CONFIG_HOME", base + "/config");
	qputenv("XDG_DATA_HOME", base + "/data");
	qputenv("XDG_CACHE_HOME", base + "/cache");
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication app(argc, argv);
	app.setApplicationName("Hydra");
	g_prior = qInstallMessageHandler(listen);
	const QString view = QString::fromLocal8Bit(argv[1]);

	// The person's own profile, standing where theirs would: nothing of it
	// may reach a view. Every live driver starts in test mode
	// (`live_paths.cpp`), which is the very switch `prepare_profile` throws,
	// so it is turned off here for the stand-in -- safe because HOME and the
	// XDG paths are this run's own temporary directory. Without that the
	// stand-in and the demo were one directory and this check passed over
	// the wipe it exists to catch.
	QStandardPaths::setTestModeEnabled(false);
	const QString own = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	QDir().mkpath(own);
	{
		QFile f(own + "/tree.txt");
		f.open(QIODevice::WriteOnly);
		f.write("- [p0] folder | Private\n  - [p1] unopened | Bank | "
		        "https://bank.invalid/ | created=2026-01-01T00:00:00 | "
		        "seen=2026-01-01T00:00:00\n");
	}

	QString why;
	check(store_view::prepare_profile(QString::fromLocal8Bit(argv[2]), &why),
	      QString("the demo profile is laid down (%1)").arg(why));
	const QString demo = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	check(demo != own && demo.contains("qttest"),
	      QString("and it is not the person's (%1, theirs %2)").arg(demo, own));
	{
		QFile f(own + "/tree.txt");
		check(f.open(QIODevice::ReadOnly) && f.readAll().contains("bank.invalid"),
		      "whose tree is left where it was");
	}

	policy_engine policy;
	request_filter filter(&policy);
	qtwebengine_factory factory(&filter);
	main_window w(&factory, &policy, &filter);
	check(w.load_tree(demo + "/tree.txt"), "the demo tree loads");
	w.resize(412, 860);   // a phone's width, so the tree is a drawer
	w.show();
	spin(500);
	w.show_store_view(view);
	for (int i = 0; i < 300 && !g_ready.contains(view); ++i)
		spin(100);
	check(g_ready.contains(view), QString("the %1 view says it is drawn").arg(view));
	// On a phone-sized screen only: a dialog may lay itself out for the
	// screen it is on, and Settings does. Run under
	// `xvfb-run -a -s "-screen 0 320x700x24"` for this check.
	const QScreen *screen = QGuiApplication::primaryScreen();
	const bool phone = screen && screen->availableGeometry().width() <= k_phone_width;
	if ((view == "settings" || view == "media") && !phone)
		std::printf("  --    not on a phone-sized screen, so whether it fits one is not "
		            "checked\n");
	if ((view == "settings" || view == "media") && phone) {
		spin(300);
		check(g_modal_min >= 0 && g_modal_min <= k_phone_width,
		      QString("and fits a phone, %1 points wide (it needs %2)")
		          .arg(k_phone_width).arg(g_modal_min));
		if (g_modal_min > k_phone_width)
			for (const QString &line : g_widest)
				std::printf("        %s\n", qPrintable(line));
	}
	if (view == "downloads") {
		download_manager *dm = w.findChild<download_manager *>();
		check(dm && dm->jobs().size() == 4,
		      QString("with the demo's four downloads (%1)").arg(dm ? dm->jobs().size() : -1));
	}
	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
