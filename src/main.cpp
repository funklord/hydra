// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window.h"

#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QFileInfo>

int main(int argc, char *argv[]) {
	// X11 only, by design (architecture doc §2/§14). Force the xcb platform if
	// the environment hasn't already chosen one, so foreign-window and embedding
	// behavior stays predictable (and works under XWayland on Wayland sessions).
	if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
		qputenv("QT_QPA_PLATFORM", QByteArray("xcb"));

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
