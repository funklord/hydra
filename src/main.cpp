// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window.h"
#include "settings_dialog.h"   // settings_store
#include "theme.h"
#ifdef Q_OS_ANDROID
#include "android_dialogs.h"
#endif
#include "policy_engine.h"
#include "request_filter.h"
// The single place that names a concrete backend (architecture doc §19.2). The
// whole point of the seam is that this is the only file that has to know, and
// that is now measured rather than asserted: the other fifty-one translation
// units compile for arm64 unchanged.
#ifdef Q_OS_ANDROID
#include "android_view.h"
#else
#include "qtwebengine_factory.h"
#endif
#include "torrent_download_source.h"

#include <QApplication>
#include <QIcon>
#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QtGlobal>

int main(int argc, char *argv[]) {
	// Desktop Linux only: force the xcb platform plugin unless the environment
	// has already chosen one, so the X11 behaviour this design relies on
	// (architecture doc §2/§14) stays predictable, and a Wayland session runs
	// under XWayland. Every other target has one sensible platform plugin —
	// Windows, macOS, and Android each pick correctly on their own — so forcing
	// anything there would be actively wrong. Q_OS_LINUX is also defined on
	// Android, hence the second half of the guard.
#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
	if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
		qputenv("QT_QPA_PLATFORM", QByteArray("xcb"));
#endif

	// Recommended for Qt WebEngine.
	QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

	// Custom URL schemes must be registered before the engine initialises, so
	// this cannot move later. Each download source names the non-web schemes it
	// claims; without the registration Chromium treats them as external
	// protocols and drops such navigations before anything of ours can see them
	// (§11.4). The list is empty when the feature is not built, and then
	// nothing changes.
#ifndef Q_OS_ANDROID
	qtwebengine_factory::register_url_schemes(torrent_download_source::url_schemes());
#endif

	QApplication app(argc, argv);
#ifdef Q_OS_ANDROID
	// Dialogs here were laid out for a desktop and come up wider than a phone
	// screen, which puts their buttons off the edge with no way to reach them.
	android_dialogs::install();
#endif
	app.setApplicationName("Hydra");

	// The colour scheme, before anything is shown: applying it after the first
	// window is up means a visible flash of the wrong theme, which is the sort
	// of thing that looks like a bug in the window manager.
	//
	// The watcher outlives this scope and keeps following the desktop, so a
	// system that switches at sunset takes Hydra with it. It only acts while the
	// choice is "system" -- someone who picked Dark meant Dark.
	// The icon theme first, and before any icon is built. Qt6 finds one through
	// a platform-theme plugin, of which it ships Plasma's and GTK's; this
	// desktop is Trinity and loads neither, so QIcon::themeName() is empty and
	// every QIcon::fromTheme comes back null. The toolbar then draws nothing and
	// says nothing about why.
	if (const QString icons = theme::apply_icon_theme(
	        theme::resolve(settings_store::appearance()));
	    !icons.isEmpty())
		qInfo("icon theme: %s", qPrintable(icons));
	else
		qWarning("no icon theme found; toolbar buttons will fall back to text");

	auto *appearance = new theme::watcher(&app);
	appearance->set_choice(settings_store::appearance());
	// And the web engine, before anything creates a profile: this one is a
	// startup flag rather than a live setting, for the reason theme.h explains.
	theme::set_web_engine_scheme(theme::resolve(settings_store::appearance()));

	// Every size is added rather than one image scaled, because the 16px cut is
	// drawn pixel by pixel rather than resampled and would be thrown away by a
	// QIcon that only knew the large one. Qt then picks per use: the tab strip
	// gets the drawn 16, the alt-tab switcher gets 48, the about box gets 256.
	QIcon icon;
	for (int size : { 16, 24, 32, 48, 64, 128, 256 })
		icon.addFile(QString(":/icons/hydra-%1.png").arg(size),
		              QSize(size, size));
	QApplication::setWindowIcon(icon);

	// The only place in the tree that names a concrete web view backend
	// (architecture doc §19.2). Swapping in the Android System WebView is
	// meant to be a change to these two lines plus one new backend class.
	// Declaration order matters: each of these outlives the ones below it.
	policy_engine       policy;
	request_filter      filter(&policy);
#ifdef Q_OS_ANDROID
	android_factory factory(&filter);
#else
	qtwebengine_factory factory(&filter);
#endif

	main_window w(&factory, &policy, &filter);

	// **The argument may be a url rather than a tree.** The desktop entry says
	// `Exec=hydra %U` and claims http, https and text/html, so once this is
	// installed as the default browser every clicked link arrives as argv[1].
	// Read as a tree path it names no file, the window comes up empty, and the
	// link is gone -- which is what a browser that cannot open a link looks
	// like from the outside.
	//
	// Only schemes a page can be at. A path is not a url and `file:` is not
	// treated as one either: `hydra ./tree.txt` has always meant the tree, and
	// this must not quietly change what that does.
	QString open_arg;
	if (argc > 1) {
		const QUrl candidate = QUrl::fromUserInput(QString::fromLocal8Bit(argv[1]));
		const QString scheme = candidate.scheme();
		if (candidate.isValid() && (scheme == "http" || scheme == "https") &&
		    !QFileInfo::exists(QString::fromLocal8Bit(argv[1])))
			open_arg = candidate.toString();
	}

	// Tree file: first CLI arg, else ./sample-tree.txt next to the binary or cwd.
	QString tree_path = (argc > 1 && open_arg.isEmpty())
	                        ? QString::fromLocal8Bit(argv[1])
	                        : QStringLiteral("sample-tree.txt");
	if (!QFileInfo::exists(tree_path)) {
		const QString beside = QDir(QCoreApplication::applicationDirPath())
		                           .filePath("sample-tree.txt");
		if (QFileInfo::exists(beside))
			tree_path = beside;
	}
#ifndef Q_OS_ANDROID
	// **An installed copy has nowhere in the build tree to fall back to**, and
	// until this was here it fell back to a *relative* path -- so `hydra` run
	// from a package wrote its tree, its policy, its filters and its state
	// directory into whatever directory it happened to be launched from. From a
	// desktop entry that is the user's home; from a file manager it is whatever
	// folder was open. Found by running the packaged binary: two tabs appeared
	// in the repository's own `sample-tree.txt`, which is a tracked file.
	//
	// So the same answer Android already needed, for the same reason and with
	// the same seed: everything this program keeps lives beside the tree file,
	// so putting the tree in app data moves the whole set at once.
	//
	// Only when nothing was found. Running from a build or source directory
	// still picks up the `sample-tree.txt` next to it, which is what makes
	// `make run` work on a checkout.
	if (!QFileInfo::exists(tree_path)) {
		const QString dir =
		  QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
		QDir().mkpath(dir);
		tree_path = QDir(dir).filePath("tree.txt");
		if (!QFileInfo::exists(tree_path)) {
			QFile seed(":/sample-tree.txt");
			if (seed.open(QIODevice::ReadOnly)) {
				QFile out(tree_path);
				if (out.open(QIODevice::WriteOnly))
					out.write(seed.readAll());
			}
		}
	}
#endif
#ifdef Q_OS_ANDROID
	// There is no working directory worth the name on Android -- it is `/`, and
	// nothing is writable there. Everything this program keeps lives beside the
	// tree file (policy.json, state/, filters, site rules), so pointing the tree
	// at app storage moves the whole set at once.
	//
	// Measured before it was fixed: the first run on a phone came up with an
	// empty tree and no error, because it had looked for `./sample-tree.txt`
	// and there was no such thing. Nothing was broken; there was simply nowhere
	// for any of it to be.
	{
		const QString dir =
		  QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
		QDir().mkpath(dir);
		tree_path = QDir(dir).filePath("tree.txt");
		// First run gets the sample, so the app opens with something in it
		// rather than an empty pane that looks like a failure.
		if (!QFileInfo::exists(tree_path)) {
			QFile seed(":/sample-tree.txt");
			if (seed.open(QIODevice::ReadOnly)) {
				QFile out(tree_path);
				if (out.open(QIODevice::WriteOnly))
					out.write(seed.readAll());
			}
		}
	}
#endif
	w.load_tree(tree_path);
	// After the tree, so the tab lands in a loaded tree rather than being
	// dropped when the file replaces the model underneath it.
	if (!open_arg.isEmpty())
		w.open_url(QUrl(open_arg));

	w.show();
	return app.exec();
}
