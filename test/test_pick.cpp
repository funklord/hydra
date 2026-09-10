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
#include <QHeaderView>
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

	// **Reported from use: the window "doesn't scale to text size and doesn't
	// allow the user to adjust the field width".** Both were true and they
	// are separate faults.
	//
	// Not one section was `Interactive`, and Qt moves a divider only for
	// those: Name was `Stretch`, three were `ResizeToContents`, Progress was
	// `Fixed` at 170 px. So the table chose its own widths and refused every
	// attempt to change them -- measured as `any column draggable: no`.
	//
	// And the widths that were not chosen by content were pixels: 170 for
	// Progress, 880x460 for the window. A bar whose text grows with the font
	// inside a column that does not is the settings-lists fault again, where
	// five rows became two at double size.
	{
		auto *t = dlg.findChild<QTreeWidget *>();
		check(t != nullptr, "the downloads list is there");
		QHeaderView *const h = t ? t->header() : nullptr;

		int fixed = 0;
		for (int c = 0; h && c < h->count(); ++c)
			if (h->sectionResizeMode(c) != QHeaderView::Interactive)
				++fixed;
		check(h && fixed == 0,
		       QString("every column can be dragged (%1 of %2 cannot)")
		         .arg(fixed).arg(h ? h->count() : 0));

		// **The widths move with the font.** Asserted as a relationship
		// rather than against numbers: pinning 241 or 124 would go stale the
		// first time anything about the font changed, which is the failure
		// being fixed.
		QList<int> before;
		for (int c = 0; h && c < h->count(); ++c)
			before << h->sectionSize(c);

		const QFont was = QApplication::font();
		QFont bigger = was;
		bigger.setPointSizeF(was.pointSizeF() * 2);
		QApplication::setFont(bigger);
		downloads_dialog wide(&m, &players, &proxy);
		wide.show();
		spin(300);
		auto *t2 = wide.findChild<QTreeWidget *>();
		QHeaderView *const h2 = t2 ? t2->header() : nullptr;
		// **Every column except the last, and "every" is the point.**
		//
		// The first version of this said `grew >= 4` of five, to leave room
		// for the last section being sized by the window rather than the
		// font. Sabotaged by putting the old `170` back on Progress, it
		// passed: four still grew, because the pinned one was the fifth it
		// was already excusing. A threshold that tolerates one failure
		// cannot see one failure.
		//
		// The last section is genuinely the window's -- `stretchLastSection`
		// gives it the slack -- so it is excluded by position rather than by
		// a count, and every other column has to move.
		QStringList stuck;
		const int last = h2 ? h2->count() - 1 : 0;
		for (int c = 0; h2 && c < last && c < before.size(); ++c)
			if (h2->sectionSize(c) <= before.at(c))
				stuck << t2->headerItem()->text(c);
		check(h2 && stuck.isEmpty(),
		       QString("and every column but the last grows when the text "
		                "does (%1)")
		         .arg(stuck.isEmpty() ? QString("all of them")
		                               : QString("pinned: %1")
		                                   .arg(stuck.join(", "))));
		wide.close();
		QApplication::setFont(was);
	}

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

	// **The same address twice is one download.**
	//
	// Reported from use: clicking a magnet link sometimes produced two rows
	// and sometimes one. There was no check in `enqueue` at all -- a source
	// accepted the url and a job was built, however many times it was asked.
	// Why the engine's handler fires twice for some links is still open; this
	// is the half answerable where the jobs are made.
	const QUrl twice("magnet:?xt=urn:btih:twice");
	const int before = int(m.jobs().size());
	QString e2;
	const int first = m.enqueue(twice, QString(), &e2);
	check(first != 0, "a magnet is accepted once");
	const int again = m.enqueue(twice, QString(), &e2);
	check(again == first,
	       QString("and asking again is the same download (%1 then %2)")
	           .arg(first).arg(again));
	check(int(m.jobs().size()) == before + 1,
	       QString("with one row, not two (%1)")
	           .arg(int(m.jobs().size()) - before));

	// **But a finished one can be asked for again.** `is_terminal` is done,
	// failed or cancelled, and refusing those would make a failed download
	// unrepeatable -- which is a worse fault than the duplicate it prevents.
	m.cancel(first);
	const int retried = m.enqueue(twice, QString(), &e2);
	check(retried != 0 && retried != first,
	       QString("a cancelled one can be started afresh (%1 then %2)")
	           .arg(first).arg(retried));
	check(int(m.jobs().size()) == before + 2,
	       QString("which is a row of its own (%1)")
	           .arg(int(m.jobs().size()) - before));

	std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
