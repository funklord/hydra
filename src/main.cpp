// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window.h"

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

	QApplication app(argc, argv);
	app.setApplicationName("Hydra");

	main_window w;

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
