// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A private copy of the committed example tree, for a driver to open.
//
// **A driver must never load `sample-tree.txt` itself.** The shell saves the
// tree whenever it changes -- a page's title arriving, a `seen=` timestamp, a
// tab opening -- so a driver pointed at the tracked file rewrites a file git is
// watching, every run. `make run` was given a copy for this reason after the
// example was reverted from git five times in one day, mostly by people who had
// not knowingly run anything against it. The drivers were not, and eleven of
// them still opened it directly.
//
// What that costs got worse rather than staying still. A page's new window is a
// *sub-tab* now (architecture doc §5.5), so a driver pointed at a real site
// saves what that site opened as children of the tab it came from. A sweep left
// two `fedoq.com/clicks/...` ad-redirect urls in the committed example --
// carrying screen size, timezone, browser version and the referring page -- as
// sub-tabs under the first entry. That is somebody's browsing, with tracking
// parameters, staged for commit into a public file.
//
// The source is found rather than spelled out, because several drivers had
// `/home/nabbe/src/hydra/sample-tree.txt` written into them, which is one
// machine's path and nobody else's.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>

namespace shell {

// The committed example, wherever this tree happens to be checked out.
inline QString sample_tree_source() {
	QStringList roots;
	roots << QCoreApplication::applicationDirPath() << QDir::currentPath();
	QStringList rels;
	rels << "sample-tree.txt" << "../sample-tree.txt";
	rels << "../../sample-tree.txt" << "../../../sample-tree.txt";
	for (const QString &root : roots)
		for (const QString &rel : rels) {
			const QString p = QDir(root).absoluteFilePath(rel);
			if (QFileInfo::exists(p))
				return QDir::cleanPath(p);
		}
	return QString();
}

// Where a driver may write. `HYDRA_TEST_OUT` is what the sweep sets per driver
// and what several drivers already read; the fallback is named after the
// running program so two drivers cannot share one copy and confuse each other.
inline QString scratch_dir() {
	if (qEnvironmentVariableIsSet("HYDRA_TEST_OUT"))
		return QString::fromLocal8Bit(qgetenv("HYDRA_TEST_OUT"));
	const QString path = QCoreApplication::applicationFilePath();
	return QStringLiteral("/tmp/hydra-") + QFileInfo(path).fileName();
}

// A copy of it under `out_dir`, which is the path a driver should load. Falls
// back to writing a minimal tree when the example cannot be found, so a driver
// still runs rather than opening nothing and failing somewhere less obvious.
inline QString sample_tree_copy(const QString &out_dir = scratch_dir()) {
	QDir().mkpath(out_dir);
	const QString dest = QDir(out_dir).filePath("sample-tree.txt");
	QFile::remove(dest);   // a copy from a previous run is not a fresh start
	const QString src = sample_tree_source();
	if (!src.isEmpty() && QFile::copy(src, dest)) {
		// `QFile::copy` keeps the source's permissions, and a read-only
		// checkout would then hand back a tree the shell cannot save to.
		QFile(dest).setPermissions(QFile::ReadOwner | QFile::WriteOwner);
		return dest;
	}
	QFile f(dest);
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		f.write("- [f0] folder | Work\n");
	return dest;
}

}  // namespace shell
