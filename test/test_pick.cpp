// Which file in a multi-file job does Watch aim at? Checked through the tree
// the user actually sees, since find_playable is the dialog's own business.
#include "download_manager.h"
#include "downloads_dialog.h"
#include "fake_sources.h"
#include "local_proxy.h"
#include "player_launcher.h"

#include <QApplication>
#include <QEventLoop>
#include <QTimer>
#include <QTreeWidget>
#include <cstdio>

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const QString &w) {
	if (ok) { ++g_pass; std::printf("  ok    %s\n", qPrintable(w)); }
	else    { ++g_fail; std::printf("  FAIL  %s\n", qPrintable(w)); }
}
static void spin(int ms) {
	QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec();
}

// The file the dialog marked as the one it would play.
static QString marked(QTreeWidget *tree, int row) {
	QTreeWidgetItem *top = tree->topLevelItem(row);
	for (int i = 0; i < top->childCount(); ++i)
		if (top->child(i)->text(4) == "would be played")
			return top->child(i)->text(0);
	return QString();
}

int main(int argc, char **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	QApplication app(argc, argv);

	download_manager m;
	auto *tor = new fake_torrent_source;
	m.add_source(tor);
	m.set_consent("torrent", true);
	player_launcher players;
	local_proxy proxy;

	QString e;
	// A sample clip sorted ahead of the feature -- the exact trap.
	const int a = m.enqueue(QUrl("magnet:?xt=urn:btih:a"), QString(), &e);
	tor->resolve(a, 4000, { {"Rel/sample.mkv", 42LL * 1024 * 1024},
		                       {"Rel/feature.mkv", 3LL * 1024 * 1024 * 1024},
		                       {"Rel/notes.txt", 4096} });

	// Nothing playable at all.
	const int b = m.enqueue(QUrl("magnet:?xt=urn:btih:b"), QString(), &e);
	tor->resolve(b, 4000, { {"Docs/readme.txt", 100}, {"Docs/cover.jpg", 900} });

	// Unknown sizes: order is the only information there is.
	const int c = m.enqueue(QUrl("magnet:?xt=urn:btih:c"), QString(), &e);
	tor->resolve(c, 4000, { {"X/first.mkv", -1}, {"X/second.mkv", -1} });

	spin(400);
	downloads_dialog dlg(&m, &players, &proxy);
	dlg.resize(900, 400);
	dlg.show();
	spin(700);

	auto *tree = dlg.findChild<QTreeWidget *>();
	check(tree && tree->topLevelItemCount() == 3, "three jobs listed");
	for (int i = 0; i < tree->topLevelItemCount(); ++i)
		tree->topLevelItem(i)->setExpanded(true);
	spin(200);

	check(marked(tree, 0) == "Rel/feature.mkv",
	      QString("the largest playable file wins, not the first (%1)")
	          .arg(marked(tree, 0)));
	check(marked(tree, 1).isEmpty(),
	      QString("a job with nothing playable marks nothing (%1)")
	          .arg(marked(tree, 1)));
	check(marked(tree, 2) == "X/first.mkv",
	      QString("with sizes unknown it falls back to order (%1)")
	          .arg(marked(tree, 2)));

	// And the sizes are shown, so the choice is inspectable.
	QTreeWidgetItem *feature = nullptr;
	for (int i = 0; i < tree->topLevelItem(0)->childCount(); ++i)
		if (tree->topLevelItem(0)->child(i)->text(0) == "Rel/feature.mkv")
			feature = tree->topLevelItem(0)->child(i);
	check(feature && feature->text(3).contains("GiB"),
	      QString("per-file sizes are displayed (%1)")
	          .arg(feature ? feature->text(3) : QString()));

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
