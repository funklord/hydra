// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window.h"
#include "policy_engine.h"
#include "request_filter.h"
#include "qtwebengine_factory.h"
#include "torrent_download_source.h"

#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QFileInfo>
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
	qtwebengine_factory::register_url_schemes(torrent_download_source::url_schemes());

	QApplication app(argc, argv);
	app.setApplicationName("Hydra");

	// The only place in the tree that names a concrete web view backend
	// (architecture doc §19.2). Swapping in the Android System WebView is
	// meant to be a change to these two lines plus one new backend class.
	// Declaration order matters: each of these outlives the ones below it.
	policy_engine       policy;
	request_filter      filter(&policy);
	qtwebengine_factory factory(&filter);

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
	w.load_tree(tree_path);

	w.show();
	return app.exec();
}
