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
// *sub-tab* now (architecture doc sec 5.5), so a driver pointed at a real site
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
#include <QUrl>

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

// The same copy with nothing in it that opens itself.
//
// **The example holds a suspended tab pointing at doc.qt.io**, and loading a
// tree restores what was suspended -- so every driver that opened the example
// fetched that site, and with it googletagmanager, amplitude, surveymonkey and
// nine other hosts. Seventy requests across twelve hosts, on a driver whose
// subject was a local fixture.
//
// A driver wants the tree's *shape* -- folders, nesting, a few entries -- and
// almost never wants it to start loading the web. So the copy is made inert:
// every entry becomes `unopened`, which is the type that means "a url and a
// title, nothing loaded". A driver that wants a page open opens one, which is
// what they all already do.
inline QString inert_sample_tree(const QString &out_dir = scratch_dir()) {
	const QString path = sample_tree_copy(out_dir);
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return path;
	QString text = QString::fromUtf8(f.readAll());
	f.close();
	// Only the type field, which is the word after the id in brackets.
	text.replace(QStringLiteral("] suspended | "), QStringLiteral("] unopened | "));
	text.replace(QStringLiteral("] open | "), QStringLiteral("] unopened | "));
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		f.write(text.toUtf8());
	return path;
}

// A tree holding one tab, at `url`.
//
// **For a driver that opens the first tab and then navigates it.** Those need
// somewhere to start, and the committed example's first entry is a real site --
// so a driver that activated row 0 fetched `doc.qt.io` and the ten hosts its
// page pulls in, whatever the driver's actual subject was. Making the copy
// inert does not help there: the driver opens the tab on purpose, and an
// unopened tab opens fine.
//
// So the tree it opens should be the one it means to look at.
inline QString single_tab_tree(const QString &url,
                                  const QString &out_dir = scratch_dir()) {
	QDir().mkpath(out_dir);
	const QString dest = QDir(out_dir).filePath("one-tab-tree.txt");
	QFile f(dest);
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		QString line = QStringLiteral("- [f0] folder | Fixture\n");
		line += QStringLiteral("  - [a1] unopened | fixture | ") + url;
		line += QStringLiteral("\n");
		f.write(line.toUtf8());
	}
	return dest;
}

// **A tree whose one tab is a local page.** For the drivers that open a tab in
// order to *have* one open, rather than to load anything in particular.
//
// `inert_sample_tree` is the wrong tool for that and was used for it three
// times. It marks every row unopened so nothing loads on startup, which is
// what "inert" means -- but a driver that then activates a row loads whatever
// that row points at, and in the committed example the first one is
// `doc.qt.io`. See *The drivers no longer fetch a real site to make a point*
// in project.md: that entry fixed `try_media`, `try_frame` and `try_mse`, and
// names this exact second half -- "the url the driver navigates to, and the
// tree it opens". Three more drivers had only the first half fixed.
//
// A real url is still what you pass when a real site is the question, which is
// how `try_extract` has always worked.
inline QString local_page_tree(const QString &out_dir = scratch_dir()) {
	QDir().mkpath(out_dir);
	const QString page = QDir(out_dir).filePath("local-page.html");
	QFile f(page);
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		f.write("<!doctype html><title>local fixture</title>"
		         "<style>body{background:#123;color:#eee;font:16px sans-serif}"
		         "h1{margin:2em}</style><h1>local fixture</h1>");
	f.close();
	return single_tab_tree(QUrl::fromLocalFile(page).toString(), out_dir);
}

}  // namespace shell
