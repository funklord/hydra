// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window.h"
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
	app.setApplicationName("Hydra");

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

	// Tree file: first CLI arg, else ./sample-tree.txt next to the binary or cwd.
	QString tree_path = (argc > 1) ? QString::fromLocal8Bit(argv[1])
	                               : QStringLiteral("sample-tree.txt");
	if (!QFileInfo::exists(tree_path)) {
		const QString beside = QDir(QCoreApplication::applicationDirPath())
		                           .filePath("sample-tree.txt");
		if (QFileInfo::exists(beside))
			tree_path = beside;
	}
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

	w.show();
	return app.exec();
}
